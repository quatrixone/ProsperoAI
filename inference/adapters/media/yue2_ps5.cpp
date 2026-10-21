#include "runtime_residency.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// YuE2 song generation through audio.cpp and the PS5 Vulkan backend.
#include "audiocpp.h"
#include "../../../include/generated_output_path.hpp"
#include "../../../include/direct_memory.hpp"
#include "../../../include/runtime_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C"
{
    int sceKernelDebugOutText(int, const char *);
    std::uint64_t sceKernelGetProcessTime(void);
}

namespace
{
audiocpp_registry *g_registry;
audiocpp_model *g_model;
audiocpp_options *g_options;
audiocpp_session *g_session;
std::string g_model_root;

void release_engine()
{
    prospero_runtime::set_model_resident(false);
    audiocpp_session_free(g_session);
    g_session = nullptr;
    audiocpp_options_free(g_options);
    g_options = nullptr;
    audiocpp_model_free(g_model);
    g_model = nullptr;
    audiocpp_registry_free(g_registry);
    g_registry = nullptr;
    g_model_root.clear();
}

void write_u16(std::FILE *file, std::uint16_t value)
{
    const unsigned char bytes[] = {static_cast<unsigned char>(value & 0xff),
                                   static_cast<unsigned char>((value >> 8) & 0xff)};
    std::fwrite(bytes, 1, sizeof(bytes), file);
}

void write_u32(std::FILE *file, std::uint32_t value)
{
    write_u16(file, static_cast<std::uint16_t>(value));
    write_u16(file, static_cast<std::uint16_t>(value >> 16));
}

bool write_wav(const char *path, const float *samples, std::size_t frames, int sample_rate,
               int channels)
{
    if (!samples || !frames || sample_rate <= 0 || channels <= 0 || channels > 8 ||
        frames > (UINT32_MAX - 36u) / (static_cast<std::uint64_t>(channels) * 2u))
        return false;
    const std::size_t sample_count = frames * static_cast<std::size_t>(channels);
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(sample_count * 2u);
    std::FILE *file = std::fopen(path, "wbx");
    if (!file)
        return false;
    std::fwrite("RIFF", 1, 4, file);
    write_u32(file, 36u + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    write_u32(file, 16);
    write_u16(file, 1);
    write_u16(file, static_cast<std::uint16_t>(channels));
    write_u32(file, static_cast<std::uint32_t>(sample_rate));
    write_u32(file, static_cast<std::uint32_t>(sample_rate * channels * 2));
    write_u16(file, static_cast<std::uint16_t>(channels * 2));
    write_u16(file, 16);
    std::fwrite("data", 1, 4, file);
    write_u32(file, data_bytes);
    for (std::size_t i = 0; i < sample_count; ++i)
    {
        float value = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0.0f;
        const auto pcm = static_cast<std::int16_t>(value < 0 ? value * 32768.0f : value * 32767.0f);
        write_u16(file, static_cast<std::uint16_t>(pcm));
    }
    const bool complete = std::ferror(file) == 0;
    return std::fclose(file) == 0 && complete;
}

void log_error(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "[prosperoai:yue2] %s: %.420s\n", stage,
                  audiocpp_last_error());
    sceKernelDebugOutText(0, line);
}

void log_note(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "[prosperoai:yue2] %s\n", stage);
    sceKernelDebugOutText(0, line);
}

// Engine options a user may override through the audio_options setting. Keys
// outside this list never reach the engine; path-valued keys are restricted to
// the app work tree so a bad setting cannot make the engine read any file.
bool option_key_allowed(const std::string &key)
{
    static constexpr const char *keys[] = {"cot",
                                           "stop_after",
                                           "export_semantic",
                                           "guidance_scale",
                                           "num_inference_steps",
                                           "abc",
                                           "abc_file",
                                           "semantic_prefix",
                                           "semantic_prefix_file",
                                           "nar_noise_file",
                                           "abc_temperature",
                                           "abc_top_p",
                                           "abc_top_k",
                                           "abc_repetition_penalty",
                                           "abc_penalty_window",
                                           "abc_min_tokens",
                                           "abc_max_tokens",
                                           "semantic_temperature",
                                           "semantic_top_p",
                                           "semantic_top_k",
                                           "semantic_repetition_penalty",
                                           "semantic_penalty_window",
                                           "semantic_min_tokens",
                                           "semantic_max_tokens"};
    for (const char *candidate : keys)
        if (key == candidate)
            return true;
    return false;
}

bool option_path_key(const std::string &key)
{
    return key == "abc_file" || key == "semantic_prefix_file" || key == "nar_noise_file";
}

// Applies "key=value" lines from the setting after the adapter defaults, so an
// override always wins. Unknown keys are skipped with a note; a malformed line
// or a key the engine rejects aborts the request so the mistake stays visible.
bool forward_option_overrides(audiocpp_request *request, const std::string &blob,
                              std::string &stop_after, char *response,
                              std::size_t response_capacity)
{
    std::size_t pos = 0;
    while (pos < blob.size())
    {
        const std::size_t eol = blob.find('\n', pos);
        const std::string line =
            blob.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? blob.size() : eol + 1;
        if (line.empty())
            continue;
        const std::size_t eq = line.find('=');
        const std::string key = eq == std::string::npos ? "" : line.substr(0, eq);
        const std::string value = eq == std::string::npos ? "" : line.substr(eq + 1);
        if (key.empty() || value.empty() || value.size() > 1900)
        {
            log_note(("malformed audio_options line: " + line).c_str());
            std::snprintf(response, response_capacity, "YuE2: malformed audio_options line");
            return false;
        }
        if (!option_key_allowed(key))
        {
            log_note(("audio_options: unknown key ignored: " + key).c_str());
            continue;
        }
        if (option_path_key(key) && (value.find("..") != std::string::npos ||
                                     value.rfind("/download0/ProsperoAI/work/", 0) != 0))
        {
            std::snprintf(response, response_capacity,
                          "YuE2: %s must stay under /download0/ProsperoAI/work/", key.c_str());
            return false;
        }
        if (key == "stop_after")
            stop_after = value;
        if (audiocpp_request_set_option(request, key.c_str(), value.c_str()) != AUDIOCPP_OK)
        {
            log_error("audio_options rejected");
            std::snprintf(response, response_capacity, "YuE2 rejected audio option '%s': %s",
                          key.c_str(), audiocpp_last_error());
            return false;
        }
    }
    return true;
}

// Saves every artifact the run produced (score.abc, semantic.json, ...) next to
// the WAV, where the media allowlist in http_server serves them for download.
// Returns a comma-separated list of the written paths for the response text.
std::string write_result_artifacts(const audiocpp_result *result, const char *output_path)
{
    std::string base = output_path;
    const std::size_t dot = base.rfind('.');
    const std::size_t slash = base.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base = base.substr(0, dot);
    std::string note;
    for (std::size_t i = 0; i < audiocpp_result_artifact_count(result); ++i)
    {
        audiocpp_artifact_kind kind = AUDIOCPP_ARTIFACT_CUSTOM;
        const char *id = nullptr;
        const void *payload = nullptr;
        std::size_t bytes = 0;
        if (audiocpp_result_artifact(result, i, &kind, &id, &payload, &bytes) != AUDIOCPP_OK ||
            !payload || !bytes)
            continue;
        std::string name = id && *id ? id : "artifact";
        for (char &c : name)
            if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
                  c == '-' || c == '_'))
                c = '_';
        std::string extension = ".bin";
        if (name == "score")
            extension = ".abc";
        else if (name == "semantic")
            extension = ".json";
        else if (kind == AUDIOCPP_ARTIFACT_MIDI)
            extension = ".mid";
        const std::string path = base + "-" + name + extension;
        std::FILE *file = std::fopen(path.c_str(), "wbx");
        if (!file || std::fwrite(payload, 1, bytes, file) != bytes)
        {
            if (file)
                std::fclose(file);
            log_note(("artifact write failed: " + path).c_str());
            continue;
        }
        if (std::fclose(file))
            continue;
        if (!note.empty())
            note += ", ";
        note += path;
    }
    return note;
}

void log_memory(const char *stage)
{
    char line[160];
    std::snprintf(line, sizeof(line),
                  "[prosperoai:yue2] %s: largest free block %llu MiB, total free %llu MiB, "
                  "flexible free %llu MiB\n",
                  stage,
                  static_cast<unsigned long long>(prospero_memory::largest_free_block() >> 20),
                  static_cast<unsigned long long>(prospero_memory::free_total() >> 20),
                  static_cast<unsigned long long>(prospero_memory::flexible_free() >> 20));
    sceKernelDebugOutText(0, line);
}

// PS5 VRAM budget: the pinned upstream sidecar ships desktop-sized VAE decode
// tiles (1024 latent frames) whose graph fails to allocate next to the
// resident model ("Oobleck audio VAE decode backend buffer allocation
// failed"). Clamp the tile keys to the values proven on PS5 on every model
// load, so installer re-downloads of the hash-pinned sidecar cannot resurrect
// the failure. preset_installed() checks this sidecar's BYTE SIZE against the
// pin (1378), not its hash, so the rewrite must preserve the original size
// exactly or the model flips to "not installed" after every update. We pad
// the tail with spaces (legal JSON whitespace) back to the original length.
// Keys absent (older configs) get the proven pair appended.
void clamp_vae_tiles(const std::string &root)
{
    const std::string path = root + "/sidecars/yue2-vae-config.json";
    std::string text;
    if (std::FILE *f = std::fopen(path.c_str(), "rb"))
    {
        char buf[512];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        std::fclose(f);
    }
    else
    {
        return;
    }
    const std::size_t original_size = text.size();
    auto tile_value = [&text](const char *key, long long fallback) -> long long
    {
        const std::size_t k = text.find(key);
        if (k == std::string::npos)
            return fallback;
        std::size_t p = k + std::strlen(key);
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' ||
                                   text[p] == '\r'))
            ++p;
        long long value = 0;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9')
            value = value * 10 + (text[p++] - '0');
        return value;
    };
    auto rewrite = [&text](const char *key, long long value)
    {
        const std::size_t k = text.find(key);
        if (k == std::string::npos)
            return;
        std::size_t p = k + std::strlen(key);
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' ||
                                   text[p] == '\r'))
            ++p;
        std::size_t q = p;
        while (q < text.size() && text[q] >= '0' && text[q] <= '9')
            ++q;
        text.replace(p, q - p, std::to_string(value));
    };
    const long long core = tile_value("\"decode_core_frames\":", -1);
    const long long halo = tile_value("\"decode_halo_frames\":", -1);
    bool changed = false;
    if (core == -1 || halo == -1)
    {
        // Keys absent (older configs fall back to the engine's 1024/16
        // desktop default): append the proven PS5 pair before the final '}'.
        const std::size_t end = text.rfind('}');
        if (end != std::string::npos)
        {
            text.insert(end, ",\"decode_core_frames\":512,\"decode_halo_frames\":32");
            changed = true;
        }
    }
    else if (core > 512)
    {
        rewrite("\"decode_core_frames\":", 512);
        rewrite("\"decode_halo_frames\":", 32);
        changed = true;
    }
    if (changed)
    {
        // Preserve the pinned byte size exactly: preset_installed() compares
        // st_size to the pin (1378) and a shorter/longer file reads as
        // "not installed". Pad the tail with spaces (JSON ignores trailing
        // whitespace) back to the original length; if the clamp somehow grew
        // the text past it, skip the write rather than break the pin.
        if (text.size() < original_size)
            text.append(original_size - text.size(), ' ');
        if (text.size() == original_size)
        {
            if (std::FILE *w = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(text.data(), 1, text.size(), w);
                std::fclose(w);
                log_error("clamped VAE decode tiles to 512/32 for PS5 VRAM budget");
            }
        }
        else
        {
            log_error("VAE tile clamp skipped: rewrite would change pinned size");
        }
    }
}

bool ensure_engine(const std::string &root, void (*progress)(const char *))
{
    if (g_model && g_session && g_model_root == root)
        return true;
    release_engine();
    // Rewrite PS5 VAE decode tiles before the engine parses the sidecar,
    // preserving the pinned byte size so the installed-check keeps passing.
    clamp_vae_tiles(root);
    const audiocpp_model_config model_config = {"yue2", nullptr, nullptr, nullptr};
    if (audiocpp_registry_create(nullptr, &g_registry) != AUDIOCPP_OK ||
        audiocpp_model_load(g_registry, root.c_str(), &model_config, nullptr, &g_model) !=
            AUDIOCPP_OK)
    {
        log_error("model load failed");
        release_engine();
        return false;
    }
    g_options = audiocpp_options_create();
    if (!g_options)
    {
        log_error("session options creation failed");
        release_engine();
        return false;
    }
    // audio.cpp pruned the YuE2 graph arena defaults to 32 MiB (2026-10-02),
    // which only fits short generations; longer songs fail deterministically
    // in the NAR/VAE stages ("Oobleck audio VAE decode backend buffer
    // allocation failed"). PS5 Pro exposes a much larger device heap than the
    // base model, so size the arenas per console profile.
    const char *profile = getenv("PROSPERO_VULKAN_PROFILE");
    const bool ps5_pro = profile && std::string(profile) == "ps5pro";
    static const struct
    {
        const char *key;
        const char *ps5_value;
        const char *ps5pro_value;
    } k_arena_options[] = {
        {"yue2.ar_prefill_graph_arena_mb", "128", "128"},
        {"yue2.ar_decode_graph_arena_mb", "64", "64"},
        {"yue2.nar_graph_arena_mb", "512", "512"},
        {"yue2.vae_graph_arena_mb", "64", "512"},
        // Long songs with a big ABC prefix used to die with "failed to allocate
        // Yue2 NAR graph": eager NAR attention auto-tiling keeps each score
        // matrix under 3 GiB (audio.cpp nar_runtime.cpp kAttentionScoreTileBytes),
        // and a 3 GiB tile does not coexist with weights + KV + prefix-state on
        // the 12 GiB base heap. Base PS5 pins the eager path with 64-row tiles
        // (score tile <= ~100 MiB even at the full 24576-token context:
        // 64 rows x 24576 kv x 16 heads x 4 B; throughput is not the constraint
        // on this device). The AR prefix-state graph has
        // no tiling, so long ABC prefixes stay bounded by abc_max_tokens.
        {"yue2.attention", "eager", "auto"},
        {"yue2.attention_tile_rows", "64", "256"},
    };
    for (const auto &arena : k_arena_options)
    {
        const char *value = ps5_pro ? arena.ps5pro_value : arena.ps5_value;
        if (audiocpp_options_set(g_options, arena.key, value) != AUDIOCPP_OK)
            log_error((std::string("arena option rejected: ") + arena.key).c_str());
    }
    if (progress)
        progress("Loading YuE2 on Vulkan");
    const audiocpp_backend_config backend_config = {"vulkan", 0, 4};
    if (audiocpp_session_create(g_model, "gen", "offline", &backend_config, g_options,
                                &g_session) != AUDIOCPP_OK)
    {
        log_error("Vulkan session creation failed");
        release_engine();
        return false;
    }
    g_model_root = root;
    prospero_runtime::set_model_resident(true);
    return true;
}
} // namespace

extern "C" void ps5_yue2_release()
{
    release_engine();
}

extern "C" int ps5_yue2_generate(const char *root, const char *prompt, char *response,
                                 std::size_t response_capacity, std::uint64_t *elapsed_microseconds,
                                 void (*progress)(const char *))
{
    if (!root || !*root || !prompt || !*prompt || !response || !response_capacity)
        return 1;
    // Contiguous-memory gate (cold engine only): a cold YuE2 load takes
    // ~3.2 GiB of weights plus KV, then asks ggml-vulkan for the NAR graph
    // buffers (348 MiB observed). When the largest block is already small,
    // the engine loads and generation dies minutes later with "failed to
    // allocate Yue2 NAR graph" (validated 20.10: cold load at 6071 MiB
    // failed, 7686 and 8136 MiB succeeded). A warm engine reserved its
    // graph already — skip the gate then so back-to-back generations never
    // restart.
    if (!(g_model && g_session && g_model_root == root) &&
        prospero_memory::free_total() < (2500ull << 20))
    {
        const unsigned long long mb =
            static_cast<unsigned long long>(prospero_memory::free_total() >> 20);
        log_memory("memory gate");
        std::snprintf(response, response_capacity,
                      "NEEDS_RESTART GPU memory low: only %llu MiB free and a cold YuE2 load "
                      "needs ~2.5 GiB free. Restart the title and retry.",
                      mb);
        return 1;
    }
    char output[160];
    if (!prospero_generated_output_path(output, sizeof(output), "audio", "yue2", "wav"))
        return 1;
    const std::string model_root(root);
    audiocpp_request *request = nullptr;
    audiocpp_result *result = nullptr;
    const std::uint64_t start = sceKernelGetProcessTime();
    log_memory("before request");
    int result_code = 0;
    do
    {
        if (!ensure_engine(model_root, progress))
        {
            std::snprintf(response, response_capacity, "Could not load YuE2 engine: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        request = audiocpp_request_create();
        char seed[16];
        const auto settings = prospero_settings::get();
        std::snprintf(seed, sizeof(seed), "%u", settings.audio_seed);
        // YuE2 derives song length from the lyric structure, so audio_duration_sec
        // is intentionally not forwarded. The prompt carries the style description.
        if (!request || audiocpp_request_set_text(request, prompt, nullptr) != AUDIOCPP_OK ||
            audiocpp_request_set_option(request, "style", prompt) != AUDIOCPP_OK ||
            // cot=melody is the proven safe default: full-length lyric runs with
            // cot=off blow up allocating the Qwen batched decode graph (observed
            // on fresh boots, 240 s lyric sets). Explicit audio_options cot=...
            // below still wins, so off/full stay reachable for short lyrics.
            audiocpp_request_set_option(request, "cot", "melody") != AUDIOCPP_OK ||
            audiocpp_request_set_option(request, "seed", seed) != AUDIOCPP_OK ||
            // Cap the semantic stream (~12.5 tokens/s audio) so the VAE decode
            // buffer stays inside the ps5pro graph arena; uncapped runs emit
            // multi-minute streams that fail allocation at decode time.
            audiocpp_request_set_option(request, "semantic_max_tokens", "10000") != AUDIOCPP_OK ||
            (!settings.audio_lyrics.empty() &&
             audiocpp_request_set_option(request, "lyrics", settings.audio_lyrics.c_str()) !=
                 AUDIOCPP_OK))
        {
            log_error("request setup failed");
            result_code = 3;
            break;
        }
        // User overrides go last so they win over the defaults above.
        std::string stop_after = "audio";
        if (!forward_option_overrides(request, settings.audio_options, stop_after, response,
                                      response_capacity))
        {
            log_error("audio_options");
            result_code = 3;
            break;
        }
        if (progress)
            progress("Generating music with ggml Vulkan");
        if (audiocpp_session_run(g_session, request, &result) != AUDIOCPP_OK)
        {
            log_error("Vulkan generation failed");
            std::snprintf(response, response_capacity, "YuE2 generation failed: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        // stop_after=abc|semantic runs no audio stage; the run's value is in its
        // artifacts (score.abc, semantic.json) saved next to the unused WAV name.
        if (stop_after != "audio")
        {
            const std::string artifacts = write_result_artifacts(result, output);
            std::snprintf(response, response_capacity,
                          artifacts.empty()
                              ? "YuE2 stopped after the '%s' stage; no artifacts returned"
                              : "YuE2 stopped after the '%s' stage. Artifacts: %s",
                          stop_after.c_str(), artifacts.c_str());
            break;
        }
        const float *samples = nullptr;
        std::size_t frames = 0;
        int sample_rate = 0, channels = 0;
        if (audiocpp_result_audio(result, &samples, &frames, &sample_rate, &channels) !=
                AUDIOCPP_OK ||
            !write_wav(output, samples, frames, sample_rate, channels))
        {
            log_error("WAV output failed");
            std::remove(output);
            result_code = 5;
            break;
        }
        std::snprintf(response, response_capacity, "Music generated locally at %s", output);
        const std::string artifacts = write_result_artifacts(result, output);
        const std::size_t used = std::strlen(response);
        if (!artifacts.empty() && used + artifacts.size() + 13 < response_capacity)
        {
            std::snprintf(response + used, response_capacity - used, " Artifacts: %s",
                          artifacts.c_str());
        }
    } while (false);
    audiocpp_result_free(result);
    audiocpp_request_free(request);
    // Same allocator hygiene as the ACE-Step path: free the session between
    // requests so the VAE workspace cannot strand the PS5 heap.
    release_engine();
    log_memory("after release");
    if (elapsed_microseconds)
        *elapsed_microseconds = sceKernelGetProcessTime() - start;
    return result_code;
}
