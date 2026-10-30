// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PROSPERO_VULKAN_RUNTIME
#include "gpt_runtime.hpp"
#include "runtime_residency.hpp"
#include "runtime_adapter.hpp"
#include "runtime_vulkan.hpp"
#include "model_paths.hpp"
#include "model_presets.hpp"
#include "model_downloader_ps5.hpp"
#include "storage.hpp"
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <cctype>
#include <sys/stat.h>
#include "direct_memory.hpp"
extern "C"
{
    int sceKernelDebugOutText(int, const char *);
}
namespace
{
using prospero::inference::Adapter;
using prospero::inference::GenerationRequest;
using prospero::inference::Model;
using prospero::inference::Modality;
using prospero::inference::find_adapter;
using prospero::inference::is_media;
using prospero::inference::modality_name;

bool modality_from_preset_kind(int kind, Modality &modality)
{
    switch (kind)
    {
    case 1:
        modality = Modality::Image;
        return true;
    case 2:
        modality = Modality::Audio;
        return true;
    case 3:
        modality = Modality::Speech;
        return true;
    default:
        return false;
    }
}

std::vector<Model> models;
unsigned selected = 0;
bool scanned = false;
std::mutex mutex;
std::mutex inference_mutex;
std::atomic<unsigned> active_generations{0};
std::atomic<std::uint64_t> last_inference_activity_us{0};
std::atomic<bool> runtime_idle_unloaded{false};
std::atomic<unsigned> configured_idle_timeout_seconds{120};

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void note_inference_activity()
{
    last_inference_activity_us.store(monotonic_us(), std::memory_order_release);
    runtime_idle_unloaded.store(false, std::memory_order_release);
}
bool exists(const std::string &path)
{
    struct stat st
    {
    };
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
bool supported_diffusers_directory(const std::string &path, std::string &family)
{
    const std::string index = path + "/model_index.json";
    std::FILE *file = std::fopen(index.c_str(), "rb");
    if (!file)
        return false;
    char contents[8192]{};
    const std::size_t count = std::fread(contents, 1, sizeof(contents) - 1, file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
        return false;
    if (std::strstr(contents, "Flux2KleinPipeline"))
        family = "FLUX.2 Klein";
    else if (std::strstr(contents, "QwenImage21Pipeline"))
        family = "Qwen-Image 2.1";
    else if (std::strstr(contents, "StableDiffusion3Pipeline"))
        family = "SD3";
    else if (std::strstr(contents, "StableDiffusionXLPipeline"))
        family = "SDXL";
    else
        return false;
    return count > 0;
}
bool supported_qwen_image_2_1_directory(const std::string &path)
{
    return exists(path + "/qwen_image_2.1-Q2_K.gguf") &&
           exists(path + "/Qwen3VL-8B-Instruct-Q4_K_M.gguf") &&
           exists(path + "/qwen_image_2.1_vae_bf16.safetensors");
}
void scan()
{
    if (scanned)
        return;
    scanned = true;
    for (unsigned i = 0, n = prospero_vulkan::gpt_runtime_model_count(); i < n; ++i)
    {
        const std::string id = prospero_vulkan::gpt_runtime_model_id(i);
        bool belongs_to_media_preset = false;
        for (const auto &preset : prospero_model_download::presets)
        {
            if (preset.kind == 0)
                continue;
            for (std::size_t file = 0; file < preset.count; ++file)
            {
                const std::string &source = preset.files[file].source;
                if (source.substr(source.find_last_of('/') + 1) == id)
                    belongs_to_media_preset = true;
            }
        }
        if (!belongs_to_media_preset)
            models.push_back({id, prospero_vulkan::gpt_runtime_model_name(i), "", Modality::Text, i});
    }
    // Preserve installed legacy bundle IDs using the equivalent pinned GGUF. Never
    // read model.ps5lm into an inference backend or delete the user's old bundle.
    for (const auto &preset : prospero_model_download::presets)
    {
        const std::string id = preset.id;
        if (preset.kind || id.size() < 5 || id.substr(id.size() - 5) != ".gguf")
            continue;
        const std::string legacy = id.substr(0, id.size() - 5);
        if (!exists(std::string(prospero::model_root()) + "/" + legacy + "/model.ps5lm"))
            continue;
        const std::string source = preset.files[0].source;
        const std::string basename = source.substr(source.find_last_of('/') + 1);
        const auto found = std::find_if(models.begin(), models.end(), [&](const Model &m)
                                        { return m.id == id || m.id == basename; });
        if (found != models.end())
        {
            const unsigned index = found->backend_index;
            models.push_back(
                {legacy, std::string(preset.name) + " (GGUF)", "", Modality::Text, index});
        }
    }
    for (const auto &preset : prospero_model_download::presets)
    {
        Modality modality;
        if (!modality_from_preset_kind(preset.kind, modality))
            continue;
        const std::string model_root = prospero::model_root();
        std::string root = model_root + "/" + preset.id;
        bool complete = true;
        for (std::size_t i = 0; i < preset.count; ++i)
        {
            const std::string path = preset.files[i].path;
            const std::string file_path = path.compare(0, 7, "voices/") == 0
                ? std::string(prospero::storage().data_root) + "/" + path
                : root + "/" + path;
            if (path.compare(0, 9, "licenses/") != 0 && !exists(file_path))
            {
                complete = false;
                break;
            }
        }
        // Older installs may have a model file directly in models/ rather than inside
        // its preset folder. Accept that layout for a single-file preset without copying
        // a multi-gigabyte GGUF just to normalize its directory structure.
        if (!complete && preset.count == 1 &&
            std::string(preset.files[0].path).find('/') == std::string::npos &&
            exists(model_root + "/" + preset.files[0].path))
        {
            complete = true;
            root = model_root;
        }
        if (complete)
            models.push_back({preset.id, preset.name, root, modality, 0});
    }
    const std::string model_root = prospero::model_root();
    if (DIR *directory = opendir(model_root.c_str()))
    {
        while (const dirent *entry = readdir(directory))
        {
            const std::string id = entry->d_name;
            if (id.empty() || id == "." || id == ".." || id[0] == '.')
                continue;
            bool safe = true;
            for (const unsigned char c : id)
                if (!std::isalnum(c) && c != '-' && c != '_' && c != '.')
                    safe = false;
            if (!safe || std::any_of(models.begin(), models.end(), [&](const Model &model)
                                     { return model.id == id; }))
                continue;
            const std::string root = model_root + "/" + id;
            struct stat info
            {
            };
            if (stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
                continue;
            std::string family;
            if (supported_diffusers_directory(root, family) &&
                (family != "Qwen-Image 2.1" || supported_qwen_image_2_1_directory(root)))
                models.push_back({id, id + " (" + family + ")", root, Modality::Image, 0});
            else if (supported_qwen_image_2_1_directory(root))
                models.push_back({id, id + " (Qwen-Image 2.1)", root, Modality::Image, 0});
        }
        closedir(directory);
    }
}
// Written at every change of model, after the one in memory was let go: with one
// model in memory at a time, the total free memory comes back to what it was.
void log_change(const Model &from, const Model &to)
{
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[prosperoai] model change: %s -> %s, largest free block %llu MiB, "
                  "total free %llu MiB, vulkan heap %llu MiB\n",
                  from.id.c_str(), to.id.c_str(),
                  static_cast<unsigned long long>(prospero_memory::largest_free_block() >> 20),
                  static_cast<unsigned long long>(prospero_memory::free_total() >> 20),
                  static_cast<unsigned long long>(
                      prospero_vulkan::gpt_runtime_vulkan_heap_bytes() >> 20));
    sceKernelDebugOutText(0, line);
}
} // namespace
bool gpt_runtime_available()
{
    return gpt_runtime_model_count() != 0;
}
unsigned gpt_runtime_generations_running()
{
    return active_generations.load(std::memory_order_acquire);
}
unsigned gpt_runtime_model_count()
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    return models.size();
}
unsigned gpt_runtime_selected_model()
{
    std::lock_guard<std::mutex> lock(mutex);
    return selected;
}
const char *gpt_runtime_model_id(unsigned i)
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    thread_local std::string id;
    id = i < models.size() ? models[i].id : "";
    return id.c_str();
}
const char *gpt_runtime_model_name(unsigned i)
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    thread_local std::string name;
    name = i < models.size() ? models[i].name : "";
    return name.c_str();
}
const char *gpt_runtime_model_purpose(unsigned i)
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    return i < models.size() ? modality_name(models[i].modality) : "unknown";
}
bool gpt_runtime_model_is_qwen_image_2_1(const char *model_id)
{
    if (!model_id || !*model_id)
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    const auto found = std::find_if(models.begin(), models.end(), [&](const Model &model)
                                    { return model.id == model_id; });
    return found != models.end() && found->modality == Modality::Image &&
           supported_qwen_image_2_1_directory(found->root);
}
const char *gpt_runtime_name()
{
    return gpt_runtime_model_name(gpt_runtime_selected_model());
}
const char *gpt_runtime_purpose()
{
    return gpt_runtime_model_purpose(gpt_runtime_selected_model());
}
const char *gpt_runtime_backend()
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    if (selected < models.size() && is_media(models[selected].modality))
    {
        const Adapter *adapter = find_adapter(models[selected].modality);
        return adapter ? adapter->name : "Unknown adapter";
    }
    return prospero_vulkan::gpt_runtime_backend();
}
bool gpt_runtime_context_full()
{
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    if (selected >= models.size())
        return false;
    return models[selected].modality == Modality::Text &&
           prospero_vulkan::gpt_runtime_context_full();
}
std::uint64_t gpt_runtime_vulkan_heap_bytes()
{
    return prospero_vulkan::gpt_runtime_vulkan_heap_bytes();
}
bool gpt_runtime_vulkan_memory(std::uint64_t *free_bytes, std::uint64_t *total_bytes)
{
    return prospero_vulkan::gpt_runtime_vulkan_memory(free_bytes, total_bytes);
}
namespace
{
bool select_model_locked(unsigned index)
{
    scan();
    if (index >= models.size())
        return false;
    if (index == selected)
        return true;
    // One model in memory: what the other runtimes hold goes before the next is loaded.
    const Modality from = models[selected].modality;
    const Modality to = models[index].modality;
    // The active adapter owns its backend teardown. Text-to-text model changes are
    // handled by llama.cpp's own selection path, which can retain an identical model.
    if (!(from == Modality::Text && to == Modality::Text))
    {
        const Adapter *adapter = find_adapter(from);
        if (adapter && adapter->release)
            adapter->release();
    }
    log_change(models[selected], models[index]);
    if (to == Modality::Text &&
        !prospero_vulkan::gpt_runtime_select_model(models[index].backend_index))
        return false;
    selected = index;
    return true;
}
} // namespace
bool gpt_runtime_loaded_model(char *id, std::size_t id_capacity, char *name, std::size_t name_capacity)
{
    if (id && id_capacity) id[0]='\0';
    if (name && name_capacity) name[0]='\0';
    std::lock_guard<std::mutex> lock(mutex);
    if (!prospero_runtime::model_resident.load(std::memory_order_acquire) || selected >= models.size()) return false;
    if (id && id_capacity) std::snprintf(id,id_capacity,"%s",models[selected].id.c_str());
    if (name && name_capacity) std::snprintf(name,name_capacity,"%s",models[selected].name.c_str());
    return true;
}
bool gpt_runtime_select_model(unsigned index)
{
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    const bool selected_ok = select_model_locked(index);
    if (selected_ok)
        note_inference_activity();
    return selected_ok;
}
bool gpt_runtime_delete_model(const char *raw_id)
{
    if (!raw_id || !*raw_id)
        return false;
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    const std::string id(raw_id);
    const auto found = std::find_if(models.begin(), models.end(),
                                    [&](const Model &model) { return model.id == id; });
    if (found == models.end() || (selected < models.size() && models[selected].id == id))
        return false;
    if (!prospero_model_download::delete_model_files(id.c_str()))
        return false;
    const std::string selected_id = selected < models.size() ? models[selected].id : std::string();
    prospero_vulkan::gpt_runtime_refresh_models();
    models.clear();
    scanned = false;
    scan();
    const auto next = std::find_if(models.begin(), models.end(),
                                   [&](const Model &model) { return model.id == selected_id; });
    selected = next == models.end() ? 0 : static_cast<unsigned>(next - models.begin());
    return true;
}
namespace
{
void check_idle_unload(unsigned timeout_seconds)
{
    if (runtime_idle_unloaded.load(std::memory_order_acquire))
        return;
    const std::uint64_t now = monotonic_us();
    const std::uint64_t last = last_inference_activity_us.load(std::memory_order_acquire);
    if (last == 0 || now < last || now - last < static_cast<std::uint64_t>(timeout_seconds) * 1000000)
        return;
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    const std::uint64_t refreshed = last_inference_activity_us.load(std::memory_order_acquire);
    if (runtime_idle_unloaded.load(std::memory_order_acquire) || now < refreshed ||
        now - refreshed < static_cast<std::uint64_t>(timeout_seconds) * 1000000)
        return;
    scan();
    if (selected < models.size())
    {
        const Adapter *adapter = find_adapter(models[selected].modality);
        if (adapter && adapter->release)
            adapter->release();
        char line[192];
        std::snprintf(line, sizeof(line), "[prosperoai] idle model released after %u seconds: %s\n",
                      timeout_seconds, models[selected].id.c_str());
        sceKernelDebugOutText(0, line);
    }
    runtime_idle_unloaded.store(true, std::memory_order_release);
}
} // namespace
void gpt_runtime_check_idle_unload(unsigned timeout_seconds)
{
    check_idle_unload(timeout_seconds);
}
void gpt_runtime_check_configured_idle_unload()
{
    check_idle_unload(configured_idle_timeout_seconds.load(std::memory_order_acquire));
}
void gpt_runtime_set_idle_unload_timeout(unsigned timeout_seconds)
{
    if (timeout_seconds == 0)
        return;
    configured_idle_timeout_seconds.store(timeout_seconds, std::memory_order_release);
    note_inference_activity();
}
void gpt_runtime_refresh_models()
{
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    scan();
    const std::string id = selected < models.size() ? models[selected].id : "";
    prospero_vulkan::gpt_runtime_refresh_models();
    models.clear();
    scanned = false;
    scan();
    const auto found =
        std::find_if(models.begin(), models.end(), [&](const Model &m) { return m.id == id; });
    selected = found == models.end() ? 0 : found - models.begin();
}
int gpt_runtime_prepare()
{
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
        return 1;
    Modality modality;
    {
        std::lock_guard<std::mutex> lock(mutex);
        scan();
        if (selected >= models.size())
            return 1;
        modality = models[selected].modality;
    }
    if (modality == Modality::Text)
    {
        const int result = prospero_vulkan::gpt_runtime_prepare();
        if (result == 0)
            note_inference_activity();
        return result;
    }
    note_inference_activity();
    return 0;
}
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,
                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress,
                         const char *image_path)
{
    if (!messages || !count || !output || !capacity)
        return 1;
    std::unique_lock<std::mutex> inference(inference_mutex, std::try_to_lock);
    if (!inference.owns_lock())
    {
        std::snprintf(output, capacity, "Another generation is running. Please wait.");
        return 1;
    }
    note_inference_activity();
    Model model;
    {
        std::lock_guard<std::mutex> lock(mutex);
        scan();
        if (selected >= models.size())
            return 1;
        if (settings.model_id && *settings.model_id)
        {
            const auto found = std::find_if(models.begin(), models.end(), [&](const Model &m)
                                            { return m.id == settings.model_id; });
            if (found == models.end() || !select_model_locked(found - models.begin()))
                return 1;
        }
        model = models[selected];
    }
    // Inference owns its backend, but does not hold the catalog lock. HTTP model
    // lists and the native UI remain responsive while media generation runs.
    const Adapter *adapter = find_adapter(model.modality);
    if (!adapter || !adapter->generate)
    {
        std::snprintf(output, capacity, "No runtime adapter is registered for %s.",
                      modality_name(model.modality));
        return 1;
    }
    struct GenerationCounterGuard
    {
        GenerationCounterGuard() { active_generations.fetch_add(1, std::memory_order_acq_rel); }
        ~GenerationCounterGuard() { active_generations.fetch_sub(1, std::memory_order_acq_rel); }
    } generation_guard;
    const int result = adapter->generate(
        GenerationRequest{model, messages, count, settings, output, capacity, stats, progress,
                          image_path});
    note_inference_activity();
    return result;
}
#endif
