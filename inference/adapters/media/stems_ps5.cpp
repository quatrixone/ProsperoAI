#include "runtime_residency.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// HTDemucs stem separation (vocals/drums/bass/other) through audio.cpp on Vulkan.
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
    std::snprintf(line, sizeof(line), "[prosperoai:stems] %s: %.420s\n", stage,
                  audiocpp_last_error());
    sceKernelDebugOutText(0, line);
}

// Persistent stage trail beside the uploads (retail has no readable stdout).
void stage_log(const char *what)
{
    std::FILE *file = std::fopen("/download0/ProsperoAI/work/audio/stems-stage.log", "a");
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

// Reads a little-endian PCM16/IEEE-float WAV (1 or 2 channels) into
// INTERLEAVED float [-1, 1]. Unlike the Cover reader this keeps stereo —
// Demucs separates L/R independently and a mono downmix would ruin it.
// Mono input is duplicated to stereo so callers can always assume 2 channels.
bool read_pcm_wav_stereo(const std::string &path, std::vector<float> &samples, int &sample_rate)
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
    long data_at = -1;
    while (ok)
    {
        if (std::fread(tag, 1, 4, file) != 4)
        {
            ok = false;
            break;
        }
        const std::uint32_t chunk = rd32(file);
        const long body_start = std::ftell(file);
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
            data_at = body_start;
        }
        const long next = body_start + static_cast<long>(chunk) + (chunk & 1u);
        if (std::fseek(file, next, SEEK_SET) != 0)
            break;
    }
    // 1 = PCM16, 3 = IEEE float 32. The scan loop always ends at EOF with
    // ok=false, so the parsed fields are the only meaningful gate.
    if (!data_bytes || !format || !rate || (format != 1 && format != 3) ||
        (bits != 16 && bits != 32) || channels < 1 || channels > 2 ||
        (format == 1 && bits != 16) || (format == 3 && bits != 32) ||
        data_bytes > 256u * 1024u * 1024u || data_at < 0)
    {
        std::fclose(file);
        return false;
    }
    const std::size_t bytes_per_frame = static_cast<std::size_t>(channels) * bits / 8u;
    const std::size_t frames = data_bytes / bytes_per_frame;
    std::vector<unsigned char> raw(data_bytes);
    if (std::fseek(file, data_at, SEEK_SET) != 0 ||
        std::fread(raw.data(), 1, data_bytes, file) != data_bytes)
    {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    samples.resize(frames * 2u);
    for (std::size_t i = 0; i < frames; ++i)
    {
        float l = 0.0f, r = 0.0f;
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
            if (!std::isfinite(one))
                one = 0.0f;
            one = std::clamp(one, -1.0f, 1.0f);
            if (ch == 0)
                l = one;
            else
                r = one;
        }
        if (channels == 1)
            r = l;
        samples[i * 2u] = l;
        samples[i * 2u + 1u] = r;
    }
    sample_rate = static_cast<int>(rate);
    return !samples.empty();
}

// Writes interleaved float stereo as 16-bit PCM WAV.
bool write_pcm_wav_stereo(const std::string &path, const float *interleaved, std::size_t frames,
                          int sample_rate)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(frames * 2u * 2u);
    const std::uint32_t riff = 36u + data_bytes;
    auto w32 = [&file](std::uint32_t v)
    {
        unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
        std::fwrite(b, 1, 4, file);
    };
    auto w16 = [&file](std::uint16_t v)
    {
        unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
        std::fwrite(b, 1, 2, file);
    };
    std::fwrite("RIFF", 1, 4, file);
    w32(riff);
    std::fwrite("WAVEfmt ", 1, 8, file);
    w32(16);
    w16(1); // PCM
    w16(2); // stereo
    w32(static_cast<std::uint32_t>(sample_rate));
    w32(static_cast<std::uint32_t>(sample_rate) * 4u);
    w16(4);
    w16(16);
    std::fwrite("data", 1, 4, file);
    w32(data_bytes);
    // PCM16 in streaming chunks so a long stem never needs a second full buffer.
    unsigned char chunk[65536];
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t take = std::min<std::size_t>(frames - done, 8192);
        for (std::size_t i = 0; i < take * 2u; ++i)
        {
            const float v = std::clamp(interleaved[(done * 2u) + i], -1.0f, 1.0f);
            const auto pcm = static_cast<std::int16_t>(v >= 0 ? v * 32767.0f : v * 32768.0f);
            chunk[i * 2u] = static_cast<unsigned char>(pcm & 0xff);
            chunk[i * 2u + 1u] = static_cast<unsigned char>((pcm >> 8) & 0xff);
        }
        if (std::fwrite(chunk, 1, take * 4u, file) != take * 4u)
        {
            std::fclose(file);
            return false;
        }
        done += take;
    }
    return std::fclose(file) == 0;
}

// Minimal sidecar so the library UI can group stems under the parent track
// (same shape the browser flow's /api/music/meta writes).
void write_stem_meta(const std::string &stem_path, const std::string &parent_name)
{
    std::string json_path = stem_path;
    const std::size_t dot = json_path.rfind('.');
    if (dot != std::string::npos)
        json_path = json_path.substr(0, dot);
    json_path += ".track.json";
    std::FILE *file = std::fopen(json_path.c_str(), "wb");
    if (!file)
        return;
    std::fprintf(file, "{\"stem_of\":\"%s\",\"engine\":\"htdemucs-vulkan\"}", parent_name.c_str());
    std::fclose(file);
}

bool ensure_engine(const std::string &root, const std::string &weight_type,
                   const std::string &backend,
                   const std::vector<std::pair<std::string, std::string>> &engine_options,
                   void (*progress)(const char *))
{
    if (g_model && g_session && g_model_root == root)
        return true;
    release_engine();
    const audiocpp_model_config model_config = {"htdemucs", nullptr, nullptr, nullptr};
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
    if (!weight_type.empty() &&
        audiocpp_options_set(g_options, "htdemucs.weight_type", weight_type.c_str()) != AUDIOCPP_OK)
        log_error("htdemucs.weight_type rejected");
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
        progress("Loading HTDemucs");
    const char *backend_name = backend == "cpu" ? "cpu" : "vulkan";
    const audiocpp_backend_config backend_config = {backend_name, 0, 8};
    if (audiocpp_session_create(g_model, "sep", "offline", &backend_config, g_options,
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

struct StemsSettings
{
    std::string input_audio;
    std::string weight_type;
    std::string backend;
    std::vector<std::pair<std::string, std::string>> engine_options;
};

StemsSettings parse_stems_options(const std::string &blob)
{
    StemsSettings out;
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
        else if (key.compare(0, 9, "htdemucs.") == 0)
            out.engine_options.emplace_back(key, value);
    }
    return out;
}

// Sanitize a stem id from the engine into a filename-safe token.
std::string safe_stem_id(const char *id)
{
    std::string name = id && *id ? id : "stem";
    for (char &c : name)
        if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' ||
              c == '_'))
            c = '_';
    if (name.size() > 40)
        name = name.substr(0, 40);
    return name;
}
} // namespace

extern "C" void ps5_stems_release()
{
    release_engine();
}

// audio_options: input_audio=/download0/.../X.wav [+ backend=cpu]
//                [+ weight_type=...] [+ htdemucs.<opt>=...]
// Writes <base>-<stem>.wav for every named output plus <base>-instrumental.wav
// (= mixture minus vocals) and a .track.json sidecar each.
extern "C" int ps5_stems_separate(const char *root, const char *prompt, char *response,
                                  std::size_t response_capacity,
                                  std::uint64_t *elapsed_microseconds,
                                  void (*progress)(const char *))
{
    (void)prompt;
    if (!root || !*root || !response || !response_capacity)
        return 1;
    const auto settings = prospero_settings::get();
    const StemsSettings cfg = parse_stems_options(settings.audio_options);
    {
        char line[640];
        std::snprintf(line, sizeof(line), "enter root=%.128s input=%.256s be=%.8s", root,
                      cfg.input_audio.c_str(), cfg.backend.c_str());
        stage_log(line);
    }
    if (cfg.input_audio.empty() || cfg.input_audio.find("..") != std::string::npos ||
        cfg.input_audio.rfind("/download0/ProsperoAI/work/audio/", 0) != 0)
    {
        stage_log("guard rejected input_audio");
        std::snprintf(response, response_capacity,
                      "Stems needs audio_options 'input_audio=/download0/ProsperoAI/work/"
                      "audio/<file>.wav'");
        return 1;
    }
    std::string base = cfg.input_audio;
    const std::size_t dot = base.rfind('.');
    const std::size_t slash = base.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base = base.substr(0, dot);
    const std::string parent_name =
        cfg.input_audio.substr(cfg.input_audio.find_last_of('/') + 1);
    audiocpp_request *request = nullptr;
    audiocpp_result *result = nullptr;
    const std::uint64_t start = sceKernelGetProcessTime();
    int result_code = 0;
    do
    {
        std::vector<float> samples;
        int sample_rate = 0;
        if (!read_pcm_wav_stereo(cfg.input_audio, samples, sample_rate))
        {
            stage_log("wav read failed");
            std::snprintf(response, response_capacity,
                          "Stems input unreadable (need 16-bit PCM WAV): %s",
                          cfg.input_audio.c_str());
            result_code = 2;
            break;
        }
        {
            char line[128];
            std::snprintf(line, sizeof(line), "wav ok: %zu frames interleaved @ %d Hz",
                          samples.size() / 2u, sample_rate);
            stage_log(line);
        }
        // HTDemucs is trained at 44.1 kHz and prepare() rejects anything else.
        // Band-limited Hann-windowed sinc (16 taps) — 48k material must not be
        // naive-truncated; aliasing here would be audible in every stem.
        if (sample_rate != 44100)
        {
            const std::size_t frames = samples.size() / 2u;
            const double ratio = 44100.0 / static_cast<double>(sample_rate);
            const std::size_t out_frames =
                std::max<std::size_t>(1u, static_cast<std::size_t>(frames * ratio));
            std::vector<float> resampled(out_frames * 2u, 0.0f);
            constexpr int kTaps = 16;
            const double pi = 3.14159265358979323846;
            const double cutoff = ratio < 1.0 ? ratio : 1.0;
            for (std::size_t i = 0; i < out_frames; ++i)
            {
                const double src = (static_cast<double>(i) + 0.5) / ratio - 0.5;
                const long center = static_cast<long>(std::floor(src + 0.5));
                for (int ch = 0; ch < 2; ++ch)
                {
                    double acc = 0.0, norm = 0.0;
                    for (int t = 0; t < kTaps; ++t)
                    {
                        const long idx = center - (kTaps / 2 - 1) + t;
                        const double x = src - static_cast<double>(idx);
                        if (idx < 0 || static_cast<std::size_t>(idx) >= frames ||
                            std::fabs(x) > kTaps / 2.0)
                            continue;
                        const double cx = pi * cutoff * x;
                        const double sinc =
                            std::fabs(cx) < 1e-9 ? cutoff : std::sin(cx) / cx * cutoff;
                        const double w = 0.5 + 0.5 * std::cos(pi * x / (kTaps / 2.0));
                        acc += static_cast<double>(samples[static_cast<std::size_t>(idx) * 2u +
                                                           static_cast<std::size_t>(ch)]) *
                               sinc * w;
                        norm += sinc * w;
                    }
                    resampled[i * 2u + static_cast<std::size_t>(ch)] =
                        norm != 0.0 ? static_cast<float>(acc / norm) : 0.0f;
                }
            }
            samples.swap(resampled);
            sample_rate = 44100;
            char line[96];
            std::snprintf(line, sizeof(line), "resampled %zu frames to 44100 Hz", frames);
            stage_log(line);
        }
        if (!ensure_engine(std::string(root), cfg.weight_type, cfg.backend, cfg.engine_options,
                           progress))
        {
            stage_error("engine load failed");
            std::snprintf(response, response_capacity, "Could not load HTDemucs engine: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        stage_log("engine + session ok");
        request = audiocpp_request_create();
        if (!request ||
            audiocpp_request_set_audio(request, samples.data(), samples.size() / 2u, sample_rate,
                                       2) != AUDIOCPP_OK)
        {
            stage_error("request setup failed");
            log_error("request setup failed");
            result_code = 3;
            break;
        }
        char stage[128];
        std::snprintf(stage, sizeof(stage), "Separating %.0f s of audio with HTDemucs on ggml "
                      "Vulkan", static_cast<double>(samples.size() / 2u) / sample_rate);
        if (progress)
            progress(stage);
        if (audiocpp_session_run(g_session, request, &result) != AUDIOCPP_OK)
        {
            stage_error("session_run");
            log_error("Vulkan separation failed");
            std::snprintf(response, response_capacity, "HTDemucs separation failed: %s",
                          audiocpp_last_error());
            result_code = 0;
            break;
        }
        const std::size_t stem_count = audiocpp_result_named_audio_count(result);
        {
            char line[64];
            std::snprintf(line, sizeof(line), "run ok: %zu named stems", stem_count);
            stage_log(line);
        }
        std::string written;
        const float *vocals = nullptr;
        std::size_t vocals_frames = 0;
        int vocals_rate = 0, vocals_channels = 0;
        for (std::size_t i = 0; i < stem_count; ++i)
        {
            const char *id = nullptr;
            const float *pcm = nullptr;
            std::size_t frames = 0;
            int rate = 0, channels = 0;
            if (audiocpp_result_named_audio(result, i, &id, &pcm, &frames, &rate, &channels) !=
                    AUDIOCPP_OK ||
                !pcm || !frames || channels != 2)
            {
                char line[128];
                std::snprintf(line, sizeof(line), "stem %zu skipped (id=%s ch=%d frames=%zu)", i,
                              id ? id : "?", channels, frames);
                stage_log(line);
                continue;
            }
            const std::string id_safe = safe_stem_id(id);
            if (id_safe == "vocals")
            {
                vocals = pcm;
                vocals_frames = frames;
                vocals_rate = rate;
                vocals_channels = channels;
            }
            const std::string path = base + "-" + id_safe + ".wav";
            if (!write_pcm_wav_stereo(path, pcm, frames, rate))
            {
                stage_log(("stem write failed: " + path).c_str());
                continue;
            }
            write_stem_meta(path, parent_name);
            if (!written.empty())
                written += ", ";
            written += path;
        }
        // Karaoke track: instrumental = mixture - vocals (exact complement,
        // same convention as the browser flow so the UI needs no new branch).
        if (vocals && written.find("-vocals.wav") != std::string::npos)
        {
            std::vector<float> minus(samples.size());
            const std::size_t frames =
                std::min<std::size_t>(samples.size() / 2u, vocals_frames);
            for (std::size_t i = 0; i < frames * 2u; ++i)
                minus[i] = std::clamp(samples[i] - vocals[i], -1.0f, 1.0f);
            const std::string path = base + "-instrumental.wav";
            if (write_pcm_wav_stereo(path, minus.data(), frames, vocals_rate))
            {
                write_stem_meta(path, parent_name);
                written += ", " + path;
            }
            else
                stage_log(("stem write failed: " + path).c_str());
        }
        if (written.empty())
        {
            stage_log("run ok but zero stems written");
            std::snprintf(response, response_capacity,
                          "HTDemucs returned no usable stems (audio too short or silent?)");
            result_code = 4;
            break;
        }
        std::snprintf(response, response_capacity, "Stems written: %s", written.c_str());
    } while (false);
    audiocpp_result_free(result);
    audiocpp_request_free(request);
    // Same allocator hygiene as Cover/YuE2: never keep the graph resident,
    // the next engine in the queue needs the whole pool.
    release_engine();
    if (elapsed_microseconds)
        *elapsed_microseconds = sceKernelGetProcessTime() - start;
    return result_code;
}
