// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_adapter.hpp"
#include "runtime_settings.hpp"
#include "chatterbox_languages.hpp"

#include "runtime_vulkan.hpp"
#include <string>

extern "C"
{
int ps5_sd_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                    gpt_runtime_progress_fn);
int ps5_stable_audio_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                              gpt_runtime_progress_fn);
int ps5_kokoro_tts_generate(const char *, const char *, const char *, char *, std::size_t, std::uint64_t *,
                            gpt_runtime_progress_fn);
int ps5_chatterbox_tts_generate(const char *, const char *, const char *, const char *, char *, std::size_t,
                                std::uint64_t *, gpt_runtime_progress_fn);
int ps5_ace_step_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                          gpt_runtime_progress_fn);
int ps5_yue2_generate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                      gpt_runtime_progress_fn);
int ps5_sheetsage_transcribe(const char *, const char *, char *, std::size_t, std::uint64_t *,
                             gpt_runtime_progress_fn);
int ps5_stems_separate(const char *, const char *, char *, std::size_t, std::uint64_t *,
                       gpt_runtime_progress_fn);
void ps5_media_stop();
void ps5_sd_shutdown();
void ps5_stable_audio_release();
void ps5_ace_step_release();
void ps5_yue2_release();
void ps5_sheetsage_release();
void ps5_stems_release();
void ps5_chatterbox_release();
}

namespace prospero::inference
{
namespace
{
const char *last_user_prompt(const GenerationRequest &request)
{
    for (unsigned i = request.message_count; i > 0; --i)
    {
        const auto &message = request.messages[i - 1];
        if (message.role && message.content && std::string(message.role) == "user")
            return message.content;
    }
    return nullptr;
}

int generate_text(const GenerationRequest &request)
{
    auto settings = request.settings;
    const std::string actual =
        prospero_vulkan::gpt_runtime_model_id(request.model.backend_index);
    settings.model_id = actual.c_str();
    return prospero_vulkan::gpt_runtime_generate(
        request.messages, request.message_count, settings, request.output,
        request.output_capacity, request.stats, request.progress);
}

int generate_media(const GenerationRequest &request)
{
    const char *prompt = last_user_prompt(request);
    if (!prompt || !*prompt)
        return 1;

    std::uint64_t elapsed = 0;
    int result = 1;
    switch (request.model.modality)
    {
    case Modality::Image:
        result = ps5_sd_generate(request.model.root.c_str(), prompt, request.output,
                                 request.output_capacity, &elapsed, request.progress);
        break;
    case Modality::Audio:
        if (request.model.id.find("ace-step-1.5") != std::string::npos)
            result = ps5_ace_step_generate(request.model.root.c_str(), prompt,
                                           request.output, request.output_capacity,
                                           &elapsed, request.progress);
        else if (request.model.id.find("yue2") != std::string::npos)
            result = ps5_yue2_generate(request.model.root.c_str(), prompt,
                                       request.output, request.output_capacity,
                                       &elapsed, request.progress);
        else if (request.model.id.find("sheetsage") != std::string::npos)
            result = ps5_sheetsage_transcribe(request.model.root.c_str(), prompt,
                                              request.output, request.output_capacity,
                                              &elapsed, request.progress);
        else if (request.model.id.find("htdemucs") != std::string::npos)
            result = ps5_stems_separate(request.model.root.c_str(), prompt,
                                        request.output, request.output_capacity,
                                        &elapsed, request.progress);
        else
            result = ps5_stable_audio_generate(request.model.root.c_str(), prompt,
                                               request.output, request.output_capacity,
                                               &elapsed, request.progress);
        break;
    case Modality::Speech:
    {
        const auto runtime = prospero_settings::get();
        if (request.model.id.find("chatterbox") != std::string::npos)
        {
            const auto selected = prospero_voice::runtime_voice(true, runtime.chatterbox_voice);
            const char *voice = selected ? selected->id.c_str() : "cemo";
            const char *language =
                prospero_language::chatterbox_language(runtime.chatterbox_language).code;
            result = ps5_chatterbox_tts_generate(request.model.root.c_str(), voice, language, prompt,
                request.output, request.output_capacity, &elapsed, request.progress);
            break;
        }
        const auto selected = prospero_voice::runtime_voice(false, runtime.kokoro_voice);
        const char *voice = selected ? selected->id.c_str() : "af_heart";
        result = ps5_kokoro_tts_generate(request.model.root.c_str(), voice, prompt, request.output,
                                         request.output_capacity, &elapsed, request.progress);
        break;
    }
    case Modality::Text:
        return 1;
    }

    if (request.stats)
    {
        *request.stats = {};
        request.stats->generated_tokens = result == 0 ? 1 : 0;
        request.stats->elapsed_microseconds = elapsed;
    }
    return result;
}

void release_text()
{
    prospero_vulkan::release_model_memory();
}

void release_media()
{
    ps5_stable_audio_release();
    ps5_ace_step_release();
    ps5_yue2_release();
    ps5_sheetsage_release();
    ps5_stems_release();
    ps5_chatterbox_release();
    ps5_media_stop();
    ps5_sd_shutdown();
}

const Adapter adapters[] = {
    {Modality::Text, "llama.cpp / Vulkan", generate_text, release_text},
    {Modality::Image, "Stable Diffusion / Vulkan", generate_media, release_media},
    {Modality::Audio, "Stable Audio / Vulkan", generate_media, release_media},
    {Modality::Speech, "Kokoro / Vulkan", generate_media, release_media},
};
} // namespace

const Adapter *find_adapter(Modality modality)
{
    for (const Adapter &adapter : adapters)
        if (adapter.modality == modality)
            return &adapter;
    return nullptr;
}

const char *modality_name(Modality modality)
{
    switch (modality)
    {
    case Modality::Text:
        return "text-to-text";
    case Modality::Image:
        return "text-to-image";
    case Modality::Audio:
        return "text-to-audio";
    case Modality::Speech:
        return "text-to-speech";
    }
    return "unknown";
}

bool is_media(Modality modality)
{
    return modality == Modality::Image || modality == Modality::Audio ||
           modality == Modality::Speech;
}

} // namespace prospero::inference
