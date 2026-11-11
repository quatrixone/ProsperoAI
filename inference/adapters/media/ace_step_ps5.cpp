#include "runtime_residency.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// ACE-Step 1.5 music generation through audio.cpp and the PS5 Vulkan backend.
#include "audiocpp.h"
#include "../../../include/generated_output_path.hpp"
#include "../../../include/direct_memory.hpp"
#include "../../../include/runtime_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

bool write_wav(const char *path, const float *samples, std::size_t frames,
               int sample_rate, int channels)
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
        const auto pcm = static_cast<std::int16_t>(value < 0 ? value * 32768.0f
                                                             : value * 32767.0f);
        write_u16(file, static_cast<std::uint16_t>(pcm));
    }
    const bool complete = std::ferror(file) == 0;
    return std::fclose(file) == 0 && complete;
}

void log_error(const char *stage)
{
    char line[512];
    std::snprintf(line, sizeof(line), "[prosperoai:ace-step] %s: %.420s\n",
                  stage, audiocpp_last_error());
    sceKernelDebugOutText(0, line);
}

void log_memory(const char *stage)
{
    char line[160];
    std::snprintf(line, sizeof(line), "[prosperoai:ace-step] %s: largest free block %llu MiB, total free %llu MiB, flexible free %llu MiB\n",
                  stage, static_cast<unsigned long long>(prospero_memory::largest_free_block() >> 20),
                  static_cast<unsigned long long>(prospero_memory::free_total() >> 20),
                  static_cast<unsigned long long>(prospero_memory::flexible_free() >> 20));
    sceKernelDebugOutText(0, line);
}

bool ensure_engine(const std::string &root, void (*progress)(const char *))
{
    if (g_model && g_session && g_model_root == root)
        return true;
    release_engine();
    const audiocpp_model_config model_config = {"ace_step", nullptr, nullptr, nullptr};
    if (audiocpp_registry_create(nullptr, &g_registry) != AUDIOCPP_OK ||
        audiocpp_model_load(g_registry, root.c_str(), &model_config, nullptr, &g_model) != AUDIOCPP_OK)
    {
        log_error("model load failed");
        release_engine();
        return false;
    }
    g_options = audiocpp_options_create();
    if (!g_options || audiocpp_options_set(g_options, "ace_step.mem_saver", "true") != AUDIOCPP_OK)
    {
        log_error("session options creation failed");
        release_engine();
        return false;
    }
    if (progress)
        progress("Loading ACE-Step 1.5 on Vulkan");
    const audiocpp_backend_config backend_config = {"vulkan", 0, 4};
    if (audiocpp_session_create(g_model, "gen", "offline", &backend_config,
                                g_options, &g_session) != AUDIOCPP_OK)
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

extern "C" void ps5_ace_step_release()
{
    release_engine();
}

extern "C" int ps5_ace_step_generate(const char *root, const char *prompt,
                                      char *response, std::size_t response_capacity,
                                      std::uint64_t *elapsed_microseconds,
                                      void (*progress)(const char *))
{
    if (!root || !*root || !prompt || !*prompt || !response || !response_capacity)
        return 1;
    char output[160];
    if (!prospero_generated_output_path(output, sizeof(output), "audio", "ace-step-1.5", "wav"))
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
            result_code = 2;
            break;
        }
        request = audiocpp_request_create();
        char duration[16];
        char seed[16];
        const auto settings = prospero_settings::get();
        std::snprintf(duration, sizeof(duration), "%u",
                      settings.audio_duration_sec ? settings.audio_duration_sec : 8u);
        std::snprintf(seed, sizeof(seed), "%u", settings.audio_seed);
        if (!request || audiocpp_request_set_text(request, prompt, nullptr) != AUDIOCPP_OK ||
            audiocpp_request_set_option(request, "duration_sec", duration) != AUDIOCPP_OK ||
            audiocpp_request_set_option(request, "seed", seed) != AUDIOCPP_OK ||
            (!settings.audio_lyrics.empty() &&
             audiocpp_request_set_option(request, "lyrics", settings.audio_lyrics.c_str()) !=
                 AUDIOCPP_OK))
        {
            log_error("request setup failed");
            result_code = 3;
            break;
        }
        if (progress)
            progress("Generating music with ggml Vulkan");
        if (audiocpp_session_run(g_session, request, &result) != AUDIOCPP_OK)
        {
            log_error("Vulkan generation failed");
            result_code = 4;
            break;
        }
        const float *samples = nullptr;
        std::size_t frames = 0;
        int sample_rate = 0, channels = 0;
        if (audiocpp_result_audio(result, &samples, &frames, &sample_rate, &channels) != AUDIOCPP_OK ||
            !write_wav(output, samples, frames, sample_rate, channels))
        {
            log_error("WAV output failed");
            std::remove(output);
            result_code = 5;
            break;
        }
        std::snprintf(response, response_capacity, "Music generated locally at %s", output);
    } while (false);
    audiocpp_result_free(result);
    audiocpp_request_free(request);
    // The VAE graph may retain a very large Vulkan reservation after decoding.
    // Drop the result and session before another request so the allocator cannot
    // strand the 1+ GiB workspace needed by a subsequent decode on the PS5 heap.
    release_engine();
    log_memory("after release");
    if (elapsed_microseconds)
        *elapsed_microseconds = sceKernelGetProcessTime() - start;
    return result_code;
}
