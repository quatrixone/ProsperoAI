#include "runtime_residency.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// SheetSage2 audio-to-ABC transcription (Cover) through audio.cpp on Vulkan.
#include "audiocpp.h"
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
#include <vector>

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

void log_error(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "[prosperoai:sheetsage] %s: %.420s\n", stage,
                  audiocpp_last_error());
    sceKernelDebugOutText(0, line);
}

void log_note(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "[prosperoai:sheetsage] %s\n", stage);
    sceKernelDebugOutText(0, line);
}

void log_memory(const char *stage)
{
    char line[160];
    std::snprintf(line, sizeof(line),
                  "[prosperoai:sheetsage] %s: largest free block %llu MiB, total free %llu MiB, "
                  "flexible free %llu MiB\n",
                  stage,
                  static_cast<unsigned long long>(prospero_memory::largest_free_block() >> 20),
                  static_cast<unsigned long long>(prospero_memory::free_total() >> 20),
                  static_cast<unsigned long long>(prospero_memory::flexible_free() >> 20));
    sceKernelDebugOutText(0, line);
}

// Persistent stage trail beside the uploads: sceKernelDebugOutText is not
// remotely readable on retail, so Cover failures must be diagnosable from
// the filesystem alone (read via the Pegasus file API).
void stage_log(const char *what)
{
    std::FILE *file = std::fopen("/download0/ProsperoAI/work/audio/sheetsage-stage.log", "a");
    if (!file)
        return;
    std::fprintf(file, "%s\n", what);
    std::fclose(file);
}

void stage_error(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "%s: %.400s", stage, audiocpp_last_error());
    stage_log(line);
    log_error(stage);
}

// Reads a little-endian PCM16 (mono or stereo, downmixed) WAV into float
// [-1, 1]. The browser side of Cover always uploads 16 kHz mono PCM16 from
// Web Audio; float-format WAVs are accepted too because they cost nothing.
// Returns false on malformed/unsupported files.
bool read_pcm_wav(const std::string &path, std::vector<float> &samples, int &sample_rate)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    auto rd32 = [](std::FILE *f) -> std::uint32_t
    {
        unsigned char b[4];
        return std::fread(b, 1, 4, f) == 4 ? static_cast<std::uint32_t>(b[0]) |
                                                 (static_cast<std::uint32_t>(b[1]) << 8) |
                                                 (static_cast<std::uint32_t>(b[2]) << 16) |
                                                 (static_cast<std::uint32_t>(b[3]) << 24)
                                           : 0u;
    };
    auto rd16 = [](std::FILE *f) -> std::uint16_t
    {
        unsigned char b[2];
        return std::fread(b, 1, 2, f) == 2
                   ? static_cast<std::uint16_t>(b[0]) | (static_cast<std::uint16_t>(b[1]) << 8)
                   : 0u;
    };
    char tag[4];
    bool ok = std::fread(tag, 1, 4, file) == 4 && std::memcmp(tag, "RIFF", 4) == 0;
    if (ok)
    {
        rd32(file); // riff size
        ok = std::fread(tag, 1, 4, file) == 4 && std::memcmp(tag, "WAVE", 4) == 0;
    }
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::size_t data_bytes = 0;
    while (ok)
    {
        if (std::fread(tag, 1, 4, file) != 4)
        {
            ok = false;
            break;
        }
        const std::uint32_t chunk = rd32(file);
        const long next = std::ftell(file) + static_cast<long>(chunk) + (chunk & 1u);
        if (std::memcmp(tag, "fmt ", 4) == 0 && chunk >= 16)
        {
            format = rd16(file);
            channels = rd16(file);
            rate = rd32(file);
            rd32(file); // byte rate
            rd16(file); // block align
            bits = rd16(file);
        }
        else if (std::memcmp(tag, "data", 4) == 0)
        {
            data_bytes = chunk;
        }
        if (std::fseek(file, next, SEEK_SET) != 0)
            break;
        if (format && channels && rate && data_bytes)
            break;
    }
    // 1 = PCM16, 3 = IEEE float 32.
    if (!ok || !data_bytes || !format || !rate || (format != 1 && format != 3) ||
        (bits != 16 && bits != 32) || channels < 1 || channels > 2 ||
        (format == 1 && bits != 16) || (format == 3 && bits != 32) ||
        data_bytes > 256u * 1024u * 1024u)
    {
        std::fclose(file);
        return false;
    }
    const std::size_t bytes_per_frame = static_cast<std::size_t>(channels) * bits / 8u;
    const std::size_t frames = data_bytes / bytes_per_frame;
    std::vector<unsigned char> raw(data_bytes);
    if (std::fseek(file, 0, SEEK_END) < 0)
    {
        std::fclose(file);
        return false;
    }
    // Re-walk to the data chunk offset (fmt parsing above left the cursor past it
    // only when data came first; the loop above stops right after reading sizes,
    // so locate data again deterministically).
    std::fseek(file, 12, SEEK_SET);
    long data_at = -1;
    char tag2[4];
    while (std::fread(tag2, 1, 4, file) == 4)
    {
        const std::uint32_t chunk = rd32(file);
        if (std::memcmp(tag2, "data", 4) == 0)
        {
            data_at = std::ftell(file);
            break;
        }
        if (std::fseek(file, static_cast<long>(chunk) + static_cast<long>(chunk & 1u), SEEK_CUR) != 0)
            break;
    }
    if (data_at < 0 || std::fseek(file, data_at, SEEK_SET) != 0 ||
        std::fread(raw.data(), 1, data_bytes, file) != data_bytes)
    {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i)
    {
        float value = 0.0f;
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            const std::size_t at = (i * channels + ch) * bits / 8u;
            float one = 0.0f;
            if (format == 1)
            {
                const auto pcm = static_cast<std::int16_t>(raw[at] |
                                                            (static_cast<std::uint16_t>(raw[at + 1])
                                                             << 8));
                one = static_cast<float>(pcm) / 32768.0f;
            }
            else
            {
                std::uint32_t u = raw[at] | (static_cast<std::uint32_t>(raw[at + 1]) << 8) |
                                 (static_cast<std::uint32_t>(raw[at + 2]) << 16) |
                                 (static_cast<std::uint32_t>(raw[at + 3]) << 24);
                std::memcpy(&one, &u, sizeof(one));
            }
            value += one;
        }
        value /= static_cast<float>(channels);
        samples[i] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }
    sample_rate = static_cast<int>(rate);
    return !samples.empty();
}

// Saves every artifact the run produced (score.abc, midi, events) next to the
// output name, matching the yue2 adapter so http_server serves them for
// download and the Music Studio UI can load them into the ABC editor.
std::string write_result_artifacts(const audiocpp_result *result, const std::string &output_path)
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
        else if (name == "semantic" || name == "events")
            extension = ".json";
        else if (kind == AUDIOCPP_ARTIFACT_MIDI)
            extension = ".mid";
        const std::string path = base + "-" + name + extension;
        std::FILE *file = std::fopen(path.c_str(), "wb");
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

bool ensure_engine(const std::string &root, const std::string &weight_type,
                   const std::string &backend,
                   const std::vector<std::pair<std::string, std::string>> &engine_options,
                   void (*progress)(const char *))
{
    if (g_model && g_session && g_model_root == root)
        return true;
    release_engine();
    const audiocpp_model_config model_config = {"sheetsage2", nullptr, nullptr, nullptr};
    if (audiocpp_registry_create(nullptr, &g_registry) != AUDIOCPP_OK ||
        audiocpp_model_load(g_registry, root.c_str(), &model_config, nullptr, &g_model) !=
            AUDIOCPP_OK)
    {
        stage_error("model_load");
        log_error("model load failed");
        release_engine();
        return false;
    }
    stage_log("model_load ok");
    g_options = audiocpp_options_create();
    if (!g_options)
    {
        log_error("session options creation failed");
        release_engine();
        return false;
    }
    // weight_type comes from the Cover settings; the pinned GGUF ships fp32
    // and q4_0/q4_k keep the MERT encoder inside the PS5 base 12 GiB heap.
    // q4_0 is the validated PS5 default (build #37 E2E: 16/16 notes correct):
    // fp32 stores + vk buffers overshoot the ~11.7 GiB direct pool.
    if (weight_type.empty())
        audiocpp_options_set(g_options, "sheetsage2.weight_type", "q4_0");
    else if (audiocpp_options_set(g_options, "sheetsage2.weight_type", weight_type.c_str()) !=
             AUDIOCPP_OK)
        log_error("sheetsage2.weight_type rejected");
    // PS5's libc heap (flexible memory) is a few hundred MiB; audio.cpp's default
    // graph arena (1536 MiB, x8 for the encoder context) only fits on a PC. 96 MiB
    // is the validated floor that still builds the decoder graph (build #30+).
    audiocpp_options_set(g_options, "sheetsage2.decoder_graph_arena_mb", "96");
    // Store contexts default to 1024 MiB each (x4 stores = 4 GiB) — combined with
    // the arenas that exhausts the direct pool mid-run (sampler death log, build
    // #37: pool free 971 MiB, largest 846 MiB). 640 fits the q4_0 stores.
    audiocpp_options_set(g_options, "sheetsage2.weight_context_mb", "640");
    for (const auto &option : engine_options)
    {
        char note[160];
        std::snprintf(note, sizeof(note), "engine option: %s=%s", option.first.c_str(),
                      option.second.c_str());
        stage_log(note);
        if (audiocpp_options_set(g_options, option.first.c_str(), option.second.c_str()) !=
            AUDIOCPP_OK)
            stage_log("engine option REJECTED");
    }
    if (progress)
        progress("Loading SheetSage2");
    // Vulkan + q4_0 + 640 MiB stores is the validated PS5 default (probe
    // 18.10: 62 s vs 376 s CPU, identical 16/16-note transcription). The old
    // fp32/1024 defaults overshot the direct pool with the vk buffers added
    // on top; audio_options backend=cpu keeps the slow-but-safe fallback.
    const char *backend_name = backend == "cpu" ? "cpu" : "vulkan";
    const audiocpp_backend_config backend_config = {backend_name, 0, 8};
    if (audiocpp_session_create(g_model, "midi", "offline", &backend_config, g_options,
                                &g_session) != AUDIOCPP_OK)
    {
        stage_error("session_create");
        log_error("Vulkan session creation failed");
        release_engine();
        return false;
    }
    g_model_root = root;
    prospero_runtime::set_model_resident(true);
    return true;
}

// audio_options keys the Cover flow understands. input_audio is consumed by
// this adapter (never forwarded to the engine); max_tokens is forwarded.
struct CoverSettings
{
    std::string input_audio;
    std::string weight_type;
    std::string backend;
    std::string max_tokens;
    std::vector<std::pair<std::string, std::string>> engine_options;
};

CoverSettings parse_cover_options(const std::string &blob)
{
    CoverSettings out;
    std::size_t pos = 0;
    while (pos < blob.size())
    {
        const std::size_t eol = blob.find('\n', pos);
        const std::string line =
            blob.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? blob.size() : eol + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (value.empty() || value.size() > 1900)
            continue;
        if (key == "input_audio")
            out.input_audio = value;
        else if (key == "weight_type")
            out.weight_type = value;
        else if (key == "backend")
            out.backend = value;
        else if (key == "max_tokens")
            out.max_tokens = value;
        else if (key.compare(0, 11, "sheetsage2.") == 0)
            out.engine_options.emplace_back(key, value); // forwarded verbatim to audio.cpp
    }
    return out;
}
} // namespace

extern "C" void ps5_sheetsage_release()
{
    release_engine();
}

// The SheetSage weight stores are 1-3 GiB host buffers; the trace shows them
// landing in libc's 284 MiB flexible heap even though the direct pool reports
// 11 GiB free. The build-35 probes proved the exact posix_memalign(64, 1 GiB)
// succeeds microseconds before session_run and fails inside it, and that
// free() of probe blocks never returns segments to the kernel (the tiling
// refusal in heap_unmap_locked) — so allocation probes poison the very pool
// they measure. This build measures without touching: a SIGABRT handler fires
// at the GGML_ASSERT to record heap state, total pool free, largest contiguous
// block, and flexible memory at the death moment.
struct HeapStatsProbe
{
    void *range_base;
    std::size_t range_bytes;
    std::size_t mapped_bytes;
    std::size_t peak_bytes;
    unsigned segments;
    unsigned padding;
    unsigned long long libc_fallbacks;
    unsigned arenas;
};
extern "C" void ps5_heap_stats(HeapStatsProbe *) noexcept;

extern "C" int ps5_sheetsage_transcribe(const char *root, const char *prompt, char *response,
                                        std::size_t response_capacity,
                                        std::uint64_t *elapsed_microseconds,
                                        void (*progress)(const char *))
{
    (void)prompt;
    if (!root || !*root || !response || !response_capacity)
        return 1;
    const auto settings = prospero_settings::get();
    const CoverSettings cover = parse_cover_options(settings.audio_options);
    {
        char line[640];
        std::snprintf(line, sizeof(line), "enter root=%.128s input=%.256s wt=%.16s be=%.8s", root,
                      cover.input_audio.c_str(), cover.weight_type.c_str(),
                      cover.backend.c_str());
        stage_log(line);
    }
    if (cover.input_audio.empty() || cover.input_audio.find("..") != std::string::npos ||
        cover.input_audio.rfind("/download0/ProsperoAI/work/audio/", 0) != 0)
    {
        stage_log("guard rejected input_audio");
        std::snprintf(response, response_capacity,
                      "Cover needs audio_options 'input_audio=/download0/ProsperoAI/work/"
                      "audio/<file>.wav'");
        return 1;
    }
    // Memory gate: SheetSage needs ~2.7 GiB of weights loaded into the direct
    // pool FIRST, then ggml-vulkan wants one ~3.4 GiB buffer for the encoder
    // graph (4.0 GiB gallocr reservation). Since the RADV arena allocator
    // backs big buffers in 2 MiB pieces, contiguity no longer matters — only
    // the pool's total free memory does. Below ~7.5 GiB total the title
    // restarts automatically (the UI reacts to NEEDS_RESTART) instead of dying
    // 30 s deep in graph build.
    if (prospero_memory::free_total() < (7500ull << 20))
    {
        const unsigned long long mb =
            static_cast<unsigned long long>(prospero_memory::free_total() >> 20);
        char line[128];
        std::snprintf(line, sizeof(line), "memory gate: pool free %llu MiB", mb);
        stage_log(line);
        std::snprintf(response, response_capacity,
                      "NEEDS_RESTART GPU memory low: only %llu MiB free and SheetSage needs "
                      "~7.5 GiB. Restart the title and retry.", mb);
        return 1;
    }
    const std::string model_root(root);
    std::string output = cover.input_audio;
    const std::size_t dot = output.rfind('.');
    if (dot != std::string::npos)
        output = output.substr(0, dot);
    output += "-cover.abc";
    audiocpp_request *request = nullptr;
    audiocpp_result *result = nullptr;
    const std::uint64_t start = sceKernelGetProcessTime();
    log_memory("before request");
    int result_code = 0;
    do
    {
        std::vector<float> samples;
        int sample_rate = 0;
        if (!read_pcm_wav(cover.input_audio, samples, sample_rate))
        {
            stage_log("wav read failed");
            std::snprintf(response, response_capacity,
                          "Cover input unreadable (need 16-bit PCM WAV): %s",
                          cover.input_audio.c_str());
            result_code = 2;
            break;
        }
        {
            char line[128];
            std::snprintf(line, sizeof(line), "wav ok: %zu samples @ %d Hz", samples.size(),
                          sample_rate);
            stage_log(line);
        }
        if (!ensure_engine(model_root, cover.weight_type, cover.backend, cover.engine_options,
                           progress))
        {
            stage_error("engine load failed");
            std::snprintf(response, response_capacity, "Could not load SheetSage2 engine: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        stage_log("engine + session ok");
        request = audiocpp_request_create();
        if (!request ||
            audiocpp_request_set_audio(request, samples.data(), samples.size(), sample_rate,
                                       1) != AUDIOCPP_OK ||
            (!cover.max_tokens.empty() &&
             audiocpp_request_set_option(request, "max_tokens", cover.max_tokens.c_str()) !=
                 AUDIOCPP_OK))
        {
            stage_error("request setup failed");
            log_error("request setup failed");
            result_code = 3;
            break;
        }
        char stage[128];
        std::snprintf(stage, sizeof(stage), "Transcribing %.0f s of audio with ggml Vulkan",
                      static_cast<double>(samples.size()) / sample_rate);
        if (progress)
            progress(stage);
        {
            char line[64];
            std::snprintf(line, sizeof(line), "request ok, session_run begin (%.1fs audio)",
                          static_cast<double>(samples.size()) / sample_rate);
            stage_log(line);
        }
        {
            HeapStatsProbe stats{};
            ps5_heap_stats(&stats);
            char line[256];
            std::snprintf(line, sizeof(line),
                          "pre-run heap: mapped=%zu MiB peak=%zu MiB seg=%u fb=%llu arenas=%u | "
                          "pool free=%zu MiB largest=%zu MiB flexible=%zu MiB",
                          stats.mapped_bytes >> 20, stats.peak_bytes >> 20, stats.segments,
                          stats.libc_fallbacks, stats.arenas, prospero_memory::free_total() >> 20,
                          prospero_memory::largest_free_block() >> 20,
                          prospero_memory::flexible_free() >> 20);
            stage_log(line);
        }
        if (audiocpp_session_run(g_session, request, &result) != AUDIOCPP_OK)
        {
            stage_error("session_run");
            log_error("Vulkan transcription failed");
            std::snprintf(response, response_capacity, "SheetSage2 transcription failed: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        const std::string artifacts = write_result_artifacts(result, output);
        if (artifacts.empty())
        {
            stage_log("run ok but zero artifacts");
            std::snprintf(response, response_capacity,
                          "SheetSage2 returned no score artifact (audio too short or silent?)");
            result_code = 4;
            break;
        }
        std::snprintf(response, response_capacity, "Cover transcribed locally. Artifacts: %s",
                      artifacts.c_str());
    } while (false);
    audiocpp_result_free(result);
    audiocpp_request_free(request);
    // Same allocator hygiene as the YuE2 path: free the session between runs
    // so the MERT encoder graph cannot strand the PS5 heap before YuE2 loads.
    release_engine();
    log_memory("after release");
    if (elapsed_microseconds)
        *elapsed_microseconds = sceKernelGetProcessTime() - start;
    return result_code;
}
