// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_vulkan.hpp"
#include "runtime_residency.hpp"
#include "runtime_adapter.hpp"
#include "model_paths.hpp"
#include "model_presets.hpp"
#include "model_downloader_ps5.hpp"
#include "storage.hpp"
#include <cassert>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <thread>
#include <chrono>
namespace
{
unsigned text_selected = 0, released = 0, stopped = 0, generated = 0, ace_generated = 0;
std::atomic<bool> block_voice{false}, voice_entered{false}, voice_release{false};
} // namespace
namespace prospero_vulkan
{
unsigned gpt_runtime_model_count()
{
    return 2;
}
const char *gpt_runtime_model_id(unsigned i)
{
    return i ? "text1" : "Mistral-7B-Instruct-v0.3.Q4_0.gguf";
}
const char *gpt_runtime_model_name(unsigned i)
{
    return gpt_runtime_model_id(i);
}
const char *gpt_runtime_backend()
{
    return "Vulkan";
}
bool gpt_runtime_context_full()
{
    return true;
}
std::uint64_t gpt_runtime_vulkan_heap_bytes()
{
    return 0;
}
bool gpt_runtime_select_model(unsigned i)
{
    if (text_selected != i)
        prospero_runtime::set_model_resident(false);
    text_selected = i;
    return i < 2;
}
void release_model_memory()
{
    ++released;
    prospero_runtime::set_model_resident(false);
}
void gpt_runtime_refresh_models()
{
}
int gpt_runtime_prepare()
{
    prospero_runtime::set_model_resident(true);
    return 0;
}
int gpt_runtime_generate(const gpt_runtime_message_t *, unsigned,
                         const gpt_runtime_settings_t &settings, char *out, std::size_t n,
                         gpt_runtime_stats_t *, gpt_runtime_progress_fn)
{
    assert(settings.model_id &&
           std::strcmp(settings.model_id, gpt_runtime_model_id(text_selected)) == 0);
    std::snprintf(out, n, "text%u", text_selected);
    return 0;
}
} // namespace prospero_vulkan
namespace prospero_model_download
{
bool delete_model_files(const char *)
{
    return true;
}
} // namespace prospero_model_download
namespace prospero
{
const Storage &storage()
{
    static const Storage state = []
    {
        Storage value;
        value.data_root = model_root();
        value.model_root = model_root();
        return value;
    }();
    return state;
}
} // namespace prospero
extern "C"
{
    std::int64_t sceKernelGetDirectMemorySize()
    {
        return 0;
    }
    int sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t *,
                                           std::size_t *free_bytes)
    {
        *free_bytes = 0;
        return 0;
    }
    int sceKernelDirectMemoryQuery(std::int64_t, int, void *, std::size_t)
    {
        return -1;
    }
    int sceKernelAvailableFlexibleMemorySize(std::size_t *)
    {
        return -1;
    }
    int sceKernelDebugOutText(int, const char *)
    {
        return 0;
    }
    void ps5_media_stop()
    {
        ++stopped;
    }
    void ps5_sd_shutdown()
    {
    }
    void ps5_stable_audio_release()
    {
    }
    void ps5_ace_step_release()
    {
    }
    void ps5_chatterbox_release()
    {
    }
    int ps5_ace_step_generate(const char *root, const char *prompt, char *out, std::size_t n,
                              std::uint64_t *elapsed, gpt_runtime_progress_fn)
    {
        assert(std::strcmp(root, "/models/ace-step-1.5-turbo-q8-0") == 0);
        assert(std::strcmp(prompt, "A bright synth melody") == 0);
        ++ace_generated;
        *elapsed = 456;
        std::snprintf(out, n, "ace-step.wav");
        return 0;
    }
    void ps5_yue2_release()
    {
    }
    int ps5_yue2_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                          gpt_runtime_progress_fn)
    {
        return 1;
    }
    void ps5_sheetsage_release()
    {
    }
    int ps5_sheetsage_transcribe(const char *, const char *, char *, std::size_t, std::uint64_t *,
                                 gpt_runtime_progress_fn)
    {
        return 1;
    }
    void ps5_stems_release()
    {
    }
    int ps5_stems_separate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                           gpt_runtime_progress_fn)
    {
        return 1;
    }
    int ps5_sd_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                        gpt_runtime_progress_fn)
    {
        return 1;
    }
    int ps5_stable_audio_generate(const char *, char *, std::size_t, std::uint64_t *,
                                  gpt_runtime_progress_fn)
    {
        return 1;
    }
    int ps5_kokoro_tts_generate(const char *root, const char *voice, const char *prompt, char *out,
                                std::size_t n, std::uint64_t *elapsed, gpt_runtime_progress_fn)
    {
        assert(std::string(root) == std::string(prospero::model_root()) + "/kokoro-82m-vulkan");
        assert(std::strcmp(voice, "af_heart") == 0);
        assert(std::strcmp(prompt, "Hello") == 0);
        ++generated;
        if (block_voice.load())
        {
            voice_entered.store(true);
            while (!voice_release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        *elapsed = 123;
        std::snprintf(out, n, "voice.wav");
        return 0;
    }
    int ps5_chatterbox_tts_generate(const char *, const char *, const char *, const char *, char *,
                                    std::size_t, std::uint64_t *, gpt_runtime_progress_fn)
    {
        return 1;
    }
}
int main()
{
    {
        const auto *adapter =
            prospero::inference::find_adapter(prospero::inference::Modality::Audio);
        assert(adapter);
        prospero::inference::Model ace_model{"ace-step-1.5-turbo-q8-0", "ACE-Step 1.5",
                                             "/models/ace-step-1.5-turbo-q8-0",
                                             prospero::inference::Modality::Audio, 0};
        const gpt_runtime_message_t ace_messages[] = {{"user", "A bright synth melody"}};
        gpt_runtime_settings_t ace_settings{};
        char ace_output[64]{};
        gpt_runtime_stats_t ace_stats{};
        const prospero::inference::GenerationRequest ace_request{
            ace_model,  ace_messages,       1,          ace_settings,
            ace_output, sizeof(ace_output), &ace_stats, nullptr};
        assert(adapter->generate(ace_request) == 0);
        assert(ace_generated == 1 && ace_stats.elapsed_microseconds == 456);
        assert(std::strcmp(ace_output, "ace-step.wav") == 0);
    }
    assert(gpt_runtime_model_count() == 2);
    assert(gpt_runtime_select_model(1));
    assert(text_selected == 1);
    const auto voice_preset = std::find_if(
        std::begin(prospero_model_download::presets), std::end(prospero_model_download::presets),
        [](const auto &preset) { return std::strcmp(preset.id, "kokoro-82m-vulkan") == 0; });
    assert(voice_preset != std::end(prospero_model_download::presets));
    const auto &voice = *voice_preset;
    for (std::size_t i = 0; i < voice.count; ++i)
    {
        const std::string relative = voice.files[i].path;
        if (relative.compare(0, 9, "licenses/") == 0)
            continue;
        const auto path = relative.compare(0, 7, "voices/") == 0
                              ? std::filesystem::path(prospero::storage().data_root) / relative
                              : std::filesystem::path(prospero::model_root()) / voice.id / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
    }
    gpt_runtime_refresh_models();
    assert(gpt_runtime_model_count() == 3 && gpt_runtime_selected_model() == 1);
    assert(std::strcmp(gpt_runtime_model_purpose(2), "text-to-speech") == 0);
    assert(!gpt_runtime_select_model(3));
    assert(gpt_runtime_select_model(2) && released == 1);
    assert(gpt_runtime_prepare() == 0 && !gpt_runtime_context_full());
    const gpt_runtime_message_t messages[] = {{"user", "Hello"}};
    char out[100]{};
    gpt_runtime_stats_t stats{};
    assert(gpt_runtime_generate(messages, 1, {}, out, sizeof(out), &stats, nullptr) == 0);
    assert(generated == 1 && stats.generated_tokens == 1 && stats.elapsed_microseconds == 123);
    assert(std::strcmp(out, "voice.wav") == 0);
    block_voice.store(true);
    gpt_runtime_settings_t voice_settings{};
    voice_settings.model_id = "kokoro-82m-vulkan";
    std::thread worker(
        [&]
        {
            assert(gpt_runtime_generate(messages, 1, voice_settings, out, sizeof(out), &stats,
                                        nullptr) == 0);
        });
    for (int i = 0; i < 1000 && !voice_entered.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(voice_entered.load());
    gpt_runtime_check_idle_unload(0);
    assert(released == 1); // an in-flight generation owns the runtime lock
    assert(gpt_runtime_model_count() == 3);
    gpt_runtime_refresh_models();
    assert(!gpt_runtime_select_model(0));
    assert(std::strcmp(gpt_runtime_model_purpose(2), "text-to-speech") == 0);
    voice_release.store(true);
    worker.join();
    assert(gpt_runtime_select_model(0) && stopped == 1);
    assert(gpt_runtime_generate(messages, 1, {}, out, sizeof(out), &stats, nullptr) == 0);
    assert(std::strcmp(out, "text0") == 0);
    assert(gpt_runtime_model_count() == 3);
    // Existing bundle IDs resolve to their equivalent GGUF on Vulkan, preserving
    // saved conversations without linking any AGC runtime or loading the bundle.
    const auto legacy = std::filesystem::path(prospero::model_root()) /
                        "Mistral-7B-Instruct-v0.3.Q4_0" / "model.ps5lm";
    std::filesystem::create_directories(legacy.parent_path());
    std::ofstream(legacy) << "legacy data must remain untouched";
    gpt_runtime_refresh_models();
    assert(gpt_runtime_model_count() == 4);
    assert(std::strcmp(gpt_runtime_model_id(2), "Mistral-7B-Instruct-v0.3.Q4_0") == 0);
    assert(std::strcmp(gpt_runtime_model_purpose(3), "text-to-speech") == 0);
    assert(gpt_runtime_select_model(2));
    assert(std::strcmp(gpt_runtime_backend(), "Vulkan") == 0);
    assert(gpt_runtime_prepare() == 0);
    char loaded_id[256]{}, loaded_name[256]{};
    assert(
        gpt_runtime_loaded_model(loaded_id, sizeof(loaded_id), loaded_name, sizeof(loaded_name)));
    assert(std::strcmp(loaded_id, gpt_runtime_model_id(gpt_runtime_selected_model())) == 0);
    assert(gpt_runtime_generate(messages, 1, {}, out, sizeof(out), &stats, nullptr) == 0);
    assert(std::strcmp(out, "text0") == 0);
    assert(std::filesystem::file_size(legacy) > 0);
    assert(gpt_runtime_select_model(0));
    gpt_runtime_check_idle_unload(0);
    assert(released == 2);
}
