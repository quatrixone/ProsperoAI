#include <stdexcept>
#include "runtime_residency.hpp"
// ProsperoAI - llama.cpp inference through the PS5 RADV Vulkan backend.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PS5_LLAMA_VULKAN
#include "gpt_runtime.hpp"
#include "reasoning_prompt.hpp"
#include "minicpm5_chat.hpp"
#include "inference_config.hpp"
#ifdef PROSPERO_VULKAN_RUNTIME
#include "runtime_vulkan.hpp"
#endif
#include "model_paths.hpp"
#include "model_presets.hpp"
#include "kv_budget.hpp"
#include "direct_memory.hpp"
#include "prefix_reuse.hpp"
#include "runtime_settings.hpp"
#include "generation_limits.hpp"
#include "llama.h"
#include "ggml-backend.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#ifdef PROSPERO_EXPERT_STREAM
#include "streaming/expert_layout.hpp"
#include "streaming/expert_stream.hpp"
#endif

#ifdef PROSPERO_VULKAN_RUNTIME
namespace prospero_vulkan {
#endif
extern "C" int sceKernelDebugOutText(int, const char *);
namespace {
struct ModelFile { std::string id, path; };
std::vector<ModelFile> files;
std::mutex runtime_mutex;
bool scanned, initialized, context_full;
bool verbose_logging;
std::atomic<bool> streaming_load_logging{false};

#ifdef PROSPERO_EXPERT_STREAM
// Routed experts are read per token, so the cache, its buffer type and the
// layout they were built from live as long as the model does.
prospero_streaming::RoutedExpertLayout expert_layout;
std::unique_ptr<prospero_streaming::ExpertCache> expert_cache;
std::unique_ptr<prospero_streaming::ExpertStream> expert_stream;
std::vector<llama_model_tensor_buft_override> expert_overrides;
unsigned expert_stream_slots = 16;

// Optional tuning from the app folder: one line, "slots[ workers]". Defaults
// stay in place when the file is absent or unreadable.
void read_streaming_tuning(unsigned &slots, unsigned &workers) {
    std::FILE *file = std::fopen("/app0/expert_cache.txt", "r");
    if (!file) return;
    unsigned read_slots = 0, read_workers = 0;
    const int fields = std::fscanf(file, "%u %u", &read_slots, &read_workers);
    std::fclose(file);
    if (fields >= 1 && read_slots >= 1 && read_slots <= 128) slots = read_slots;
    if (fields >= 2 && read_workers >= 1 && read_workers <= 8) workers = read_workers;
}
#endif
bool prefix_cache_enabled = true;
unsigned selected;
llama_model *model;
llama_context *context;
unsigned requested_context_setting;   // from gpt_runtime_settings_t::context_length
bool requested_kv_is_auto = true;     // from gpt_runtime_settings_t::kv_cache
std::string requested_kv_setting;     // "f16"/"q8_0"/"q4_0" when not Auto
unsigned requested_batch_setting;
unsigned requested_ubatch_setting;
std::uint64_t model_load_free_bytes;
std::vector<llama_token> previous_tokens; // the last turn's token sequence, for prefix reuse

// Derived from llama.cpp's own model-introspection API (llama_model_n_layer/n_head_kv,
// llama_model_is_hybrid - see llama.h), rather than raw GGUF key-value metadata: these are
// the stable, typed accessors llama.cpp's own common/fit.cpp uses for the same purpose.
// A hybrid model's recurrent layers do not all keep a full KV cache (docs/PLAN_CONTEXT_
// MODELS_VULKAN.md's table shows Qwen3.5 9B uses only 8 of 32), but llama.h has no public
// accessor for that subset count; using the full layer count here overstates KV need, which
// only makes Auto pick a smaller context than necessary - safe, not console-verified.
unsigned model_kv_layers(const llama_model *m) { return static_cast<unsigned>(llama_model_n_layer(m)); }
unsigned model_head_dim(const llama_model *m) {
    const int heads = llama_model_n_head(m);
    return heads > 0 ? static_cast<unsigned>(llama_model_n_embd(m) / heads) : 0;
}
bool model_is_hybrid(const llama_model *m) { return llama_model_is_hybrid(m) || llama_model_is_recurrent(m); }
// llama_model_n_head_kv reports layer 0. In a hybrid model such as Granite 4.0 H that
// layer can be recurrent with no KV heads, which made the estimate 0 MiB and rejected the
// model. Use every attention head instead: an overestimate, like the layer count above.
unsigned model_kv_heads(const llama_model *m) {
    const int heads = llama_model_n_head_kv(m);
    if (heads > 0) return static_cast<unsigned>(heads);
    return model_is_hybrid(m) ? static_cast<unsigned>(std::max(0, llama_model_n_head(m))) : 0;
}
enum ggml_type parse_kv_type_setting(const std::string &value) {
    if (value == "q8_0") return GGML_TYPE_Q8_0;
    if (value == "q4_0") return GGML_TYPE_Q4_0;
    return GGML_TYPE_F16;
}

void log_message(ggml_log_level, const char *text, void *) {
    char line[2048];
    std::snprintf(line, sizeof(line), "[ProsperoAI/Vulkan] %s", text);
    sceKernelDebugOutText(0, line);
}
void backend_log_message(ggml_log_level level, const char *text, void *data) {
    // Kernel debug output is synchronous. llama.cpp emits thousands of debug
    // messages while loading vocabulary/tensors and reserving graphs; keep that
    // diagnostic work out of normal startup, while retaining warnings/errors.
    if (verbose_logging || level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR ||
        (streaming_load_logging.load(std::memory_order_relaxed) && level == GGML_LOG_LEVEL_INFO))
        log_message(level, text, data);
}
bool media_bundle(const char *name) {
    // Keep bundles installed by older releases out of text-model discovery even though
    // their retired catalog preset is no longer offered. Do not delete user model data.
    if (std::strcmp(name, "kokoro-82m-fp16") == 0) return true;
    for (std::size_t i = 0; i < prospero_model_download::preset_count; ++i)
        if (prospero_model_download::presets[i].kind != 0 &&
            std::strcmp(prospero_model_download::presets[i].id, name) == 0)
            return true;
    return false;
}
void scan(const std::string &root, int depth) {
    DIR *dir = opendir(root.c_str());
    if (!dir) {
        char line[512];
        std::snprintf(line, sizeof(line), "models: opendir %s failed errno=%d\n", root.c_str(), errno);
        log_message(GGML_LOG_LEVEL_ERROR, line, nullptr);
        return;
    }
    while (auto *entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || files.size() >= 32) continue;
        std::string path = root + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            char line[512];
            std::snprintf(line, sizeof(line), "models: stat %s failed errno=%d\n", path.c_str(), errno);
            log_message(GGML_LOG_LEVEL_ERROR, line, nullptr);
            continue;
        }
        if (S_ISDIR(st.st_mode) && depth == 0) {
            // A media or speech bundle is not a folder of text models. The speech bundle
            // carries its own GGUF model and voice pack, and letting llama.cpp see them
            // put two bogus text models in the catalog.
            if (!media_bundle(entry->d_name)) scan(path, 1);
        }
        else if (S_ISREG(st.st_mode) && path.size() > 5 && path.substr(path.size() - 5) == ".gguf") {
            std::string id = path.substr(std::strlen(prospero::model_root()) + 1);
            if (id.find_first_of("\"\\\r\n") == std::string::npos) files.push_back({id, path});
        }
    }
    closedir(dir);
}
void scan_models() {
    if (scanned) return;
    scanned = true;
    // The per-boot mount helper attaches the shared folder as the title starts.
    // Discovery runs on the app worker; give that narrow mount time to appear.
    struct stat root;
    for (unsigned retry = 0; retry < 60 && stat(prospero::model_root(), &root) != 0; ++retry)
        usleep(50000);
    scan(prospero::model_root(), 0);
    std::sort(files.begin(), files.end(), [](const ModelFile &a, const ModelFile &b) { return a.id < b.id; });
    char line[128];
    std::snprintf(line, sizeof(line), "models: discovered %zu GGUF files\n", files.size());
    log_message(GGML_LOG_LEVEL_INFO, line, nullptr);
}
void release_model() {
    prospero_runtime::set_model_resident(false);
#ifdef PROSPERO_EXPERT_STREAM
    // Copies in flight hold cache leases, and the buffer type outlives nothing.
    if (expert_stream) expert_stream->drain();
#endif
    if (context) llama_free(context);
    if (model) llama_model_free(model);
    context = nullptr;
    model = nullptr;
    model_load_free_bytes = 0;
#ifdef PROSPERO_EXPERT_STREAM
    if (expert_stream) {
        prospero_streaming::ExpertStream::uninstall();
        expert_stream.reset();
    }
    expert_cache.reset();
    expert_overrides.clear();
#endif
    // The KV cache that previous_tokens describes no longer exists once the context is
    // freed; keeping it would make the next call "reuse" a prefix that was never decoded
    // into the new context.
    previous_tokens.clear();
}
void configure_context(unsigned context_length, const char *kv_cache,
                       unsigned batch_length = 0, unsigned ubatch_length = 0) {
    const bool kv_is_auto = !kv_cache || !std::strcmp(kv_cache, "auto");
    const std::string kv_setting = kv_cache ? kv_cache : "";
    if (context_length != requested_context_setting || kv_is_auto != requested_kv_is_auto ||
        kv_setting != requested_kv_setting || batch_length != requested_batch_setting ||
        ubatch_length != requested_ubatch_setting) {
        if (context) llama_free(context);
        context = nullptr;
        previous_tokens.clear();
        requested_context_setting = context_length;
        requested_kv_is_auto = kv_is_auto;
        requested_kv_setting = kv_setting;
        requested_batch_setting = batch_length;
        requested_ubatch_setting = ubatch_length;
    }
}
// A failed decode may leave some ubatches resident. Never advertise those as
// a complete reusable prefix, including errors after prefill or in callbacks.
struct CacheTransaction {
    llama_memory_t memory;
    bool committed = false;
    ~CacheTransaction() {
        if (!committed) {
            llama_memory_clear(memory, true);
            previous_tokens.clear();
        }
    }
};

std::size_t reuse_prompt_prefix(llama_memory_t memory, const std::vector<llama_token> &tokens) {
    std::size_t reused = 0;
    // Recurrent/hybrid state cannot in general rewind to an arbitrary prefix.
    // Keep their original full-prefill behavior until separately validated.
    const bool reusable = prefix_cache_enabled && memory && !previous_tokens.empty() &&
        !llama_model_is_recurrent(model) && !llama_model_is_hybrid(model) &&
        llama_memory_seq_pos_min(memory, 0) == 0 &&
        llama_memory_seq_pos_max(memory, 0) == static_cast<llama_pos>(previous_tokens.size() - 1);
    if (reusable) {
        const auto end = std::min(previous_tokens.size(), tokens.size() - 1);
        while (reused < end && previous_tokens[reused] == tokens[reused]) ++reused;
        // Always evaluate at least the final prompt token to produce fresh
        // logits. A repeated or shortened prompt must discard the old reply.
        if (reused && !llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(reused), -1))
            reused = 0;
    }
    if (!reused) llama_memory_clear(memory, true);
    previous_tokens.resize(reused);
    return reused;
}

bool prepare() {
    scan_models();
    if (files.empty()) return false;
    if (context) return true;
    const int64_t load_start = ggml_time_us();
    if (!initialized) {
        verbose_logging = access("/app0/vulkan_verbose_logging.txt", F_OK) == 0;
        // A title has no launch shell where profiling variables can be exported.
        // This package marker exposes the pinned backend's timestamp-query logger.
        if (access("/app0/vulkan_profile.txt", F_OK) == 0) {
            setenv("GGML_VK_PERF_LOGGER", "1", 1);
            setenv("GGML_VK_PERF_LOGGER_FREQUENCY", "1", 1);
            verbose_logging = true;
        }
        llama_log_set(backend_log_message, nullptr);
        ggml_log_set(backend_log_message, nullptr);
        setenv("GGML_NO_BACKTRACE", "1", 1);
        llama_backend_init();
        initialized = true;
    }
    ggml_backend_dev_t devices[] = {ggml_backend_dev_by_name("Vulkan0"), nullptr};
    const int64_t device_ready = ggml_time_us();
    if (!devices[0]) return false;
    size_t free_bytes = 0, total_bytes = 0;
    ggml_backend_dev_memory(devices[0], &free_bytes, &total_bytes);
    const size_t free_before_model = free_bytes;
    char memory_line[256];
    std::snprintf(memory_line, sizeof(memory_line),
                  "Vulkan memory: reported free=%zu total=%zu bytes\n", free_bytes, total_bytes);
    log_message(GGML_LOG_LEVEL_INFO, memory_line, nullptr);
    auto mp = llama_model_default_params();
    // PTQ1_0 now has an upstream Vulkan dequant/mat-vec path. Place Bonsai
    // weights on Vulkan too; unsupported graph operations can still fall back
    // through the normal llama.cpp scheduler.
    mp.devices = devices;
    mp.n_gpu_layers = 99;
    mp.load_mode = LLAMA_LOAD_MODE_NONE;
    const bool gemma4_streaming_requested = access("/app0/gemma4_streaming.txt", F_OK) == 0;
    if (gemma4_streaming_requested) {
        const auto & model_id = files[selected].id;
        if (model_id.find("gemma4") == std::string::npos &&
            model_id.find("gemma-4") == std::string::npos) {
            log_message(GGML_LOG_LEVEL_ERROR,
                        "Gemma 4 lazy streaming enabled, but selected model name is not Gemma 4\n", nullptr);
            return false;
        }
#ifdef PROSPERO_EXPERT_STREAM
        // Routed experts stay on disk: their tensors get address space from the
        // streaming buffer type, and the scheduler's expert copy reads the ones a
        // token selects out of the bounded cache. Everything else loads normally.
        try {
            const auto &path = files[selected].path;
            expert_layout = prospero_streaming::read_routed_expert_layout(path);
            unsigned slots = 16, workers = 2;
            read_streaming_tuning(slots, workers);
            if (slots < expert_layout.experts_used || slots > 16)
                throw std::runtime_error("Experimental PS5 cache requires routed-expert count <= slots <= 16");

            std::uint64_t budget = 0;
            unsigned streamed = 0;
            std::string reason;
            for (const auto &tensor : expert_layout.tensors) {
                if (!tensor.expert_major) continue;   // per-expert scalars stay resident
                const auto type = static_cast<ggml_type>(tensor.type);
                const auto columns = static_cast<std::int64_t>(
                    tensor.extent.row_bytes / ggml_type_size(type) * ggml_blck_size(type));
                if (!prospero_streaming::device_runs_routed_experts(
                        devices[0], tensor.type, columns,
                        static_cast<std::int64_t>(tensor.extent.rows_per_expert),
                        static_cast<std::int64_t>(tensor.extent.experts),
                        std::max(1u, expert_layout.experts_used), 1, &reason)) {
                    char line[256];
                    std::snprintf(line, sizeof(line),
                                  "Gemma 4 streaming needs the GPU to run routed experts: %s\n",
                                  reason.c_str());
                    log_message(GGML_LOG_LEVEL_ERROR, line, nullptr);
                    return false;
                }
                budget += ((tensor.bytes_per_expert + 63) & ~std::uint64_t(63))
                          * std::min<std::uint64_t>(slots, tensor.extent.experts);
                ++streamed;
            }
            constexpr std::uint64_t cache_ceiling = 2ULL << 30;
            constexpr std::uint64_t runtime_reserve = 4ULL << 30;
            const auto available = prospero_memory::free_total();
            if (!budget || budget > cache_ceiling || available > total_bytes ||
                available <= runtime_reserve || budget > available - runtime_reserve)
                throw std::runtime_error("Expert cache exceeds the safe experimental memory budget");
            expert_cache = std::make_unique<prospero_streaming::ExpertCache>(
                path.c_str(), static_cast<std::size_t>(budget), workers);
            expert_stream = std::make_unique<prospero_streaming::ExpertStream>(
                *expert_cache, [](const std::string &name,
                                  prospero_streaming::StreamedTensor &out) {
                    for (const auto &tensor : expert_layout.tensors)
                        if (tensor.expert_major && tensor.name == name) {
                            out.extent = tensor.extent;
                            out.slots = expert_stream_slots;
                            return true;
                        }
                    return false;
                });
            expert_stream_slots = slots;
            expert_stream->install();
            expert_overrides = {{"_exps\\.weight$", expert_stream->buffer_type()},
                                {nullptr, nullptr}};
            mp.tensor_buft_overrides = expert_overrides.data();
            char line[256];
            std::snprintf(line, sizeof(line),
                          "Gemma 4 streaming: %u routed layers, %u streamed tensors, "
                          "%u experts (%u used), cache %llu MiB for %u experts per tensor\n",
                          expert_layout.layers, streamed, expert_layout.experts,
                          expert_layout.experts_used,
                          (unsigned long long) (budget / (1024 * 1024)), slots);
            log_message(GGML_LOG_LEVEL_INFO, line, nullptr);
        } catch (const std::exception &failure) {
            char line[256];
            std::snprintf(line, sizeof(line), "Gemma 4 streaming setup failed: %s\n",
                          failure.what());
            log_message(GGML_LOG_LEVEL_ERROR, line, nullptr);
            return false;
        }
#else
        log_message(GGML_LOG_LEVEL_ERROR,
                    "This build cannot stream Gemma 4 routed experts\n", nullptr);
        return false;
#endif
    }
    if (!model) {
        model_load_free_bytes = free_before_model;
        streaming_load_logging = gemma4_streaming_requested;
        model = llama_model_load_from_file(files[selected].path.c_str(), mp);
        streaming_load_logging = false;
        prospero_runtime::set_model_resident(model != nullptr);
        if (!model) model_load_free_bytes = 0;
    }
    if (!model) return false;
    const int64_t weights_ready = ggml_time_us();
    prefix_cache_enabled = access("/app0/disable_prefix_cache.txt", F_OK) != 0;
    auto cp = llama_context_default_params();
    unsigned requested_context = 4096;
    enum ggml_type kv_type = GGML_TYPE_F16;
    prospero_kv::ModelKvShape shape{model_kv_layers(model), model_kv_heads(model),
                                   model_head_dim(model), static_cast<unsigned>(llama_model_n_ctx_train(model))};
    // Query the remaining heap after weights load: do not subtract weights a second time.
    ggml_backend_dev_memory(devices[0], &free_bytes, &total_bytes);
    const auto reported_free_bytes = prospero_kv::validated_free_bytes(free_bytes, total_bytes);
    if (reported_free_bytes) {
        free_bytes = reported_free_bytes;
    } else {
        // Some RADV allocations make the post-load free-size query wrap. Estimate
        // remaining capacity conservatively from the valid pre-load query and the
        // complete model size; do not turn the wrapped result into a false zero.
        const auto model_bytes = llama_model_size(model);
        free_bytes = model_load_free_bytes > model_bytes
            ? model_load_free_bytes - model_bytes : 0;
        char estimate[192];
        std::snprintf(estimate, sizeof(estimate),
                      "Vulkan free query invalid after model load; conservative remainder=%llu MiB from pre-load free=%llu MiB and model=%llu MiB.\n",
                      static_cast<unsigned long long>(free_bytes / (1024 * 1024)),
                      static_cast<unsigned long long>(model_load_free_bytes / (1024 * 1024)),
                      static_cast<unsigned long long>(model_bytes / (1024 * 1024)));
        log_message(GGML_LOG_LEVEL_WARN, estimate, nullptr);
    }
    // free_bytes is the largest contiguous block. Vulkan buffers here are at most the
    // 128 MiB suballocation block (KV is per-layer tensors), so budget the total instead.
    const std::uint64_t largest_block = free_bytes;
    if (const auto total_free = prospero_memory::free_total(); total_free > free_bytes) free_bytes = total_free;
    char free_line[160];
    std::snprintf(free_line, sizeof(free_line), "Vulkan memory after weights: largest block=%llu MiB, total free=%llu MiB\n",
                  static_cast<unsigned long long>(largest_block >> 20), static_cast<unsigned long long>(free_bytes >> 20));
    log_message(GGML_LOG_LEVEL_INFO, free_line, nullptr);
    const auto fixed_type = requested_kv_setting == "q8_0" ? prospero_kv::KvType::Q8_0
                          : requested_kv_setting == "q4_0" ? prospero_kv::KvType::Q4_0 : prospero_kv::KvType::F16;
    // The Vulkan heap is queried after model weights have loaded. Keep 512 MiB for
    // graph/workspace allocations while allowing large PTQ models to fit their KV cache.
    constexpr std::uint64_t kVulkanSafetyReserve = 512ULL * 1024 * 1024;
    const auto choice = prospero_kv::choose_auto(shape, 0, kVulkanSafetyReserve, free_bytes,
                                               requested_kv_is_auto ? nullptr : &fixed_type);
    if (!choice.context_length)
    {
        char error[256];
        std::snprintf(error, sizeof(error),
                      "Insufficient Vulkan memory: free=%llu MiB, total=%llu MiB, minimum F16 KV=%llu MiB, reserve=%llu MiB.\n",
                      static_cast<unsigned long long>(free_bytes / (1024 * 1024)),
                      static_cast<unsigned long long>(total_bytes / (1024 * 1024)),
                      static_cast<unsigned long long>(prospero_kv::kv_bytes_for_context(
                          shape, prospero_kv::KvType::F16,
                          std::min<unsigned>(4096, shape.n_ctx_train)) / (1024 * 1024)),
                      static_cast<unsigned long long>(kVulkanSafetyReserve / (1024 * 1024)));
        log_message(GGML_LOG_LEVEL_ERROR, error, nullptr);
        return false;
    }
    requested_context = requested_context_setting ? requested_context_setting : choice.context_length;
    if (FILE *config = std::fopen("/app0/context_size.txt", "r")) {
        unsigned value = 0;
        if (std::fscanf(config, "%u", &value) == 1 && value >= 512 && value <= 16384)
            requested_context = value;
        std::fclose(config);
    }
    cp.n_ctx = std::min<uint32_t>(requested_context, llama_model_n_ctx_train(model));
    kv_type = requested_kv_is_auto
        ? (choice.context_length == cp.n_ctx && choice.kv_type == prospero_kv::KvType::Q8_0
               ? GGML_TYPE_Q8_0
               : cp.n_ctx <= 8192 ? GGML_TYPE_F16 : GGML_TYPE_Q8_0)
        : parse_kv_type_setting(requested_kv_setting);
    const auto effective_kv_type = kv_type == GGML_TYPE_F16 ? prospero_kv::KvType::F16
                                  : kv_type == GGML_TYPE_Q8_0 ? prospero_kv::KvType::Q8_0
                                                              : prospero_kv::KvType::Q4_0;
    const auto kv_required = prospero_kv::kv_bytes_for_context(shape, effective_kv_type, cp.n_ctx);
    const auto safe_budget = free_bytes > kVulkanSafetyReserve ? free_bytes - kVulkanSafetyReserve : 0;
    char budget_line[224];
    std::snprintf(budget_line, sizeof(budget_line),
                  "KV budget: free=%llu MiB reserve=%llu MiB ctx=%u type=%s cache=%llu MiB\n",
                  static_cast<unsigned long long>(free_bytes / (1024 * 1024)),
                  static_cast<unsigned long long>(kVulkanSafetyReserve / (1024 * 1024)),
                  cp.n_ctx, ggml_type_name(kv_type),
                  static_cast<unsigned long long>(kv_required / (1024 * 1024)));
    log_message(GGML_LOG_LEVEL_INFO, budget_line, nullptr);
    if (!kv_required || kv_required > safe_budget)
    {
        char error[192];
        std::snprintf(error, sizeof(error),
                      "KV cache needs %llu MiB; safe Vulkan budget is %llu MiB (includes 512 MiB reserve).\n",
                      static_cast<unsigned long long>(kv_required / (1024 * 1024)),
                      static_cast<unsigned long long>(safe_budget / (1024 * 1024)));
        log_message(GGML_LOG_LEVEL_ERROR, error, nullptr);
        return false;
    }
    cp.type_k = kv_type;
    cp.type_v = kv_type;
    cp.n_batch = requested_batch_setting ? requested_batch_setting
        : prospero::read_inference_setting("/app0", "batch_size.txt",
                                            prospero::vulkan_profile_batch_default(), 32, 2048);
    cp.n_ubatch = requested_ubatch_setting ? requested_ubatch_setting
        : prospero::read_inference_setting("/app0", "ubatch_size.txt",
                                            prospero::vulkan_profile_batch_default(), 32, 2048);
    cp.n_ubatch = std::min(cp.n_batch, cp.n_ubatch);
    cp.n_threads = prospero::read_inference_setting("/app0", "threads.txt", 4, 1, 16);
    cp.n_threads_batch = prospero::read_inference_setting("/app0", "threads_batch.txt", 4, 1, 16);
    // 0=auto, 1=disabled, 2=enabled. Auto retains backend support checks.
    const unsigned flash = prospero::read_inference_setting("/app0", "flash_attention.txt", 0, 0, 2);
    cp.flash_attn_type = flash == 1 ? LLAMA_FLASH_ATTN_TYPE_DISABLED :
                        flash == 2 ? LLAMA_FLASH_ATTN_TYPE_ENABLED : LLAMA_FLASH_ATTN_TYPE_AUTO;
    if (kv_type != GGML_TYPE_F16) cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cp.no_perf = false;
    cp.offload_kqv = true;
    cp.op_offload = true;
    context = llama_init_from_model(model, cp);
    prospero_runtime::set_model_resident(model != nullptr);
    if (!context) { previous_tokens.clear(); return false; }
    char config_line[256];
    std::snprintf(config_line, sizeof(config_line),
                  "inference config: requested=%u effective_ctx=%u kv=%s batch=%u ubatch=%u threads=%d threads_batch=%d flash=%s\n",
                  requested_context, llama_n_ctx(context), ggml_type_name(kv_type),
                  llama_n_batch(context), llama_n_ubatch(context), cp.n_threads, cp.n_threads_batch,
                  llama_flash_attn_type_name(cp.flash_attn_type));
    log_message(GGML_LOG_LEVEL_INFO, config_line, nullptr);
    char timing_line[256];
    std::snprintf(timing_line, sizeof(timing_line),
                  "model ready: device=%.2f ms weights=%.2f ms context=%.2f ms total=%.2f ms\n",
                  (device_ready - load_start) / 1000.0,
                  (weights_ready - device_ready) / 1000.0,
                  (ggml_time_us() - weights_ready) / 1000.0,
                  (ggml_time_us() - load_start) / 1000.0);
    log_message(GGML_LOG_LEVEL_INFO, timing_line, nullptr);
    return true;
}
}

bool gpt_runtime_available() { std::lock_guard<std::mutex> lock(runtime_mutex); scan_models(); return !files.empty(); }
unsigned gpt_runtime_model_count() { std::lock_guard<std::mutex> lock(runtime_mutex); scan_models(); return files.size(); }
unsigned gpt_runtime_selected_model() { std::lock_guard<std::mutex> lock(runtime_mutex); return selected; }
const char *gpt_runtime_model_id(unsigned i) { std::lock_guard<std::mutex> lock(runtime_mutex); scan_models(); return i < files.size() ? files[i].id.c_str() : ""; }
const char *gpt_runtime_model_name(unsigned i) { return gpt_runtime_model_id(i); }
const char *gpt_runtime_model_purpose(unsigned) { return "text-to-text"; }
const char *gpt_runtime_name() { return gpt_runtime_model_name(gpt_runtime_selected_model()); }
const char *gpt_runtime_backend() { return "llama.cpp Vulkan / RADV"; }
const char *gpt_runtime_purpose() { return "text-to-text"; }
bool gpt_runtime_context_full() { std::lock_guard<std::mutex> lock(runtime_mutex); return context_full; }
std::uint64_t gpt_runtime_vulkan_heap_bytes() {
    auto *device = ggml_backend_dev_by_name("Vulkan0");
    if (!device) return 0;
    size_t free_bytes = 0, total_bytes = 0;
    ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
    return total_bytes;
}
bool gpt_runtime_select_model(unsigned i) {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    scan_models();
    if (i >= files.size()) return false;
    if (i != selected) { release_model(); selected = i; }
    context_full = false;
    return true;
}

void gpt_runtime_refresh_models() {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    std::string selected_id = selected < files.size() ? files[selected].id : std::string();
    files.clear(); scanned = false; scan_models();
    auto found = std::find_if(files.begin(), files.end(), [&](const ModelFile &file) { return file.id == selected_id; });
    if (found != files.end()) selected = static_cast<unsigned>(found-files.begin());
    else { release_model(); selected = 0; }
}

int gpt_runtime_prepare() {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    try {
        const auto settings = prospero_settings::get();
        configure_context(settings.context, "auto");
        if (prepare()) return 0;
        release_model(); // a refused model must not keep its weights resident
        return 1;
    }
    catch (const std::exception &e) { log_message(GGML_LOG_LEVEL_ERROR, e.what(), nullptr); release_model(); return 1; }
}

int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
    const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,
    gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress) {
    if (!messages || !count || !output || !capacity) return 1;
    std::lock_guard<std::mutex> lock(runtime_mutex);
    output[0] = 0;
    if (stats) *stats = {};
    context_full = false;
    try {
        if (settings.model_id) {
            scan_models();
            auto found = std::find_if(files.begin(), files.end(), [&](const ModelFile &file) {
                return file.id == settings.model_id;
            });
            if (found == files.end()) { std::snprintf(output, capacity, "Requested model is not installed."); return 1; }
            unsigned index = static_cast<unsigned>(found - files.begin());
            if (index != selected) { release_model(); selected = index; }
        }
        if (settings.context_tokens &&
            (settings.context_tokens < 512 ||
             settings.context_tokens > prospero_generation::MaxContextTokens)) {
            std::snprintf(output, capacity,
                          "Context window must be between 512 and 262144 tokens.");
            return 1;
        }
        if ((settings.batch_length && (settings.batch_length < 32 || settings.batch_length > 2048)) ||
            (settings.ubatch_length && (settings.ubatch_length < 32 || settings.ubatch_length > 2048))) {
            std::snprintf(output, capacity, "Batch sizes must be between 32 and 2048.");
            return 1;
        }
        configure_context(settings.context_tokens ? settings.context_tokens : settings.context_length,
                          settings.kv_cache, settings.batch_length, settings.ubatch_length);
        if (!prepare()) {
            release_model(); // a refused model must not keep its weights resident
            std::snprintf(output, capacity, "Unable to load a GGUF model on Vulkan.");
            return 1;
        }
        std::string prompt;
        const char *tmpl = llama_model_chat_template(model, nullptr);
        char tokenizer_pre[32] = {};
        llama_model_meta_val_str(model, "tokenizer.ggml.pre", tokenizer_pre, sizeof(tokenizer_pre));
        const bool minicpm5 = std::strcmp(tokenizer_pre, "minicpm5") == 0;
        if (minicpm5) {
            if (!prospero::minicpm5_chat(messages, count, prompt)) {
                std::snprintf(output, capacity, "MiniCPM5 supports system, user and assistant text messages.");
                return 1;
            }
        } else if (tmpl) {
            std::vector<llama_chat_message> chat;
            for (unsigned i = 0; i < count; ++i)
                if (messages[i].role && messages[i].content) chat.push_back({messages[i].role, messages[i].content});
            int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
            if (n <= 0) {
#ifdef PROSPERO_EXPERT_STREAM
                // The pinned llama.cpp does not know Gemma 4's template. The
                // streaming experiment measures decode, not instruction
                // following, so fall back to the raw user text and say so.
                if (access("/app0/gemma4_streaming.txt", F_OK) == 0) {
                    log_message(GGML_LOG_LEVEL_WARN,
                                "Unknown chat template; sending the raw prompt instead\n", nullptr);
                    for (unsigned i = count; i > 0; --i)
                        if (messages[i - 1].content && messages[i - 1].role &&
                            !std::strcmp(messages[i - 1].role, "user")) {
                            prompt = messages[i - 1].content;
                            break;
                        }
                } else
#endif
                { std::snprintf(output, capacity, "Unsupported model chat template."); return 1; }
            }
            if (n > 0) {
            std::vector<char> text(n + 1);
            n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, text.data(), n);
            if (n <= 0 || n >= (int)text.size()) return 1;
            prompt.assign(text.data(), n);
            }
        } else {
            for (unsigned i = count; i > 0; --i)
                if (messages[i - 1].content && messages[i - 1].role && !std::strcmp(messages[i - 1].role, "user")) {
                    prompt = messages[i - 1].content; break;
                }
        }
        prospero::reasoning_prompt(prompt, tmpl, minicpm5, settings.thinking);
        const auto *vocab = llama_model_get_vocab(model);
        int n = -llama_tokenize(vocab, prompt.data(), prompt.size(), nullptr, 0, !minicpm5, true);
        if (n <= 0) return 1;
        // No requested limit means fill the remaining context, like an unset num_predict
        // elsewhere; the clamp below still bounds it to what the context actually has left.
        unsigned limit = std::min(
            settings.max_output_tokens ? settings.max_output_tokens
                                       : prospero_generation::MaxOutputTokens,
            prospero_generation::MaxOutputTokens);
        if ((unsigned)n + 1 >= llama_n_ctx(context)) {
            context_full = true;
            std::snprintf(output, capacity, "Conversation exceeds this model's context window."); return 1;
        }
        limit = std::min(limit, llama_n_ctx(context) - (unsigned)n);
        std::vector<llama_token> tokens(n);
        if (llama_tokenize(vocab, prompt.data(), prompt.size(), tokens.data(), n, !minicpm5, true) < 0) return 1;
        const auto memory = llama_get_memory(context);
        CacheTransaction cache_transaction{memory};
        int64_t start = ggml_time_us();
        const auto reused = reuse_prompt_prefix(memory, tokens);
        previous_tokens.reserve(llama_n_ctx(context));
        if (stats) stats->reused_tokens = static_cast<unsigned>(reused);
        for (int pos = static_cast<int>(reused); pos < n; ) {
            int batch_count = std::min(n - pos, static_cast<int>(llama_n_batch(context)));
            auto batch = llama_batch_get_one(tokens.data() + pos, batch_count);
#ifdef PROSPERO_EXPERT_STREAM
            // Experts already uploaded stay valid only while the graph keeps its
            // shape, because the scheduler may move its copy tensors otherwise.
            if (expert_stream) expert_stream->begin_graph(static_cast<std::uint64_t>(batch_count));
#endif
            const int decode_result = llama_decode(context, batch);
            if (decode_result != 0 || (expert_stream && expert_stream->failed())) {
                previous_tokens.clear();
                if (expert_stream && expert_stream->failed())
                    std::snprintf(output, capacity, "Expert streaming failed while reading model weights.");
                release_model();
                return 1;
            }
            previous_tokens.insert(previous_tokens.end(), tokens.begin() + pos, tokens.begin() + pos + batch_count);
            pos += batch_count;
        }
        if (stats) { stats->prompt_tokens = n; stats->prefill_microseconds = ggml_time_us() - start; }
        std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
            llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
        if (settings.grammar) {
            auto *grammar = llama_sampler_init_grammar(vocab, settings.grammar, "root");
            if (!grammar) return 1;
            llama_sampler_chain_add(sampler.get(), grammar);
        }
        if (settings.temperature > 0) {
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_k(40));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(0.95f, 1));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(settings.temperature));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        } else llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
        std::size_t used = 0;
        // Qwen's opening marker is already in the prompt. Include it in the
        // displayed response so clients can separate reasoning from the answer.
        // It is not another generated token or another KV entry.
        if (settings.thinking && prompt.ends_with("<think>\n")) {
            constexpr char marker[] = "<think>\n";
            if (capacity <= sizeof(marker) - 1) return 1;
            std::memcpy(output, marker, sizeof(marker));
            used = sizeof(marker) - 1;
            if (progress) progress(output);
        }
        int result = 0;
        for (unsigned i = 0; i < limit; ++i) {
            llama_token token = llama_sampler_sample(sampler.get(), context, -1);
            if (llama_vocab_is_eog(vocab, token)) break;
            char piece[1024];
            int length = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
            if (length < 0) { result = 1; break; }
            if (used + length >= capacity) { if (stats) stats->output_limit_reached = true; break; }
            std::memcpy(output + used, piece, length);
            used += length;
            output[used] = 0;
            if (stats) { ++stats->generated_tokens; stats->output_limit_reached = i + 1 == limit; }
            if (progress) progress(output);
            if (i + 1 < limit) {
                auto batch = llama_batch_get_one(&token, 1);
#ifdef PROSPERO_EXPERT_STREAM
                if (expert_stream) expert_stream->begin_graph(1);
#endif
                const int decode_result = llama_decode(context, batch);
                if (decode_result != 0 || (expert_stream && expert_stream->failed())) {
                    previous_tokens.clear();
                    if (expert_stream && expert_stream->failed())
                        std::snprintf(output, capacity, "Expert streaming failed while reading model weights.");
                    release_model();
                    result = 1;
                    break;
                }
                previous_tokens.push_back(token);
            }
        }
        sampler.reset();
        cache_transaction.committed = result == 0;
        if (stats) stats->elapsed_microseconds = ggml_time_us() - start;
        if (stats) {
            char line[384];
            std::snprintf(line, sizeof(line),
                "gpu_stats rc=%08X prompt=%u generated=%u reused=%u prefill_us=%llu elapsed_us=%llu\n",
                result, stats->prompt_tokens, stats->generated_tokens, stats->reused_tokens,
                static_cast<unsigned long long>(stats->prefill_microseconds),
                static_cast<unsigned long long>(stats->elapsed_microseconds));
            log_message(GGML_LOG_LEVEL_INFO, line, nullptr);
        }
        llama_perf_context_print(context);
        return result;
    } catch (const std::exception &e) {
        log_message(GGML_LOG_LEVEL_ERROR, e.what(), nullptr);
        std::snprintf(output, capacity, "Vulkan inference failed: %s", e.what());
        release_model();
        return 1;
    }
}
#ifdef PROSPERO_VULKAN_RUNTIME
void release_model_memory() { std::lock_guard<std::mutex> lock(runtime_mutex); release_model(); }
} // namespace prospero_vulkan
#endif
#endif
