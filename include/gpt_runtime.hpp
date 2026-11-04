// ProsperoAI model-runtime boundary.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

struct gpt_runtime_message_t
{
    const char *role;
    const char *content;
};

struct gpt_runtime_settings_t
{
    unsigned response_style;
    unsigned max_output_tokens;
    float temperature = 0;
    const char* model_id = nullptr; // Select atomically with Vulkan generation.
    unsigned context_tokens = 0; // Zero uses the model-load configuration.
    const char* grammar = nullptr; // Optional GBNF constraint (Vulkan only).
    unsigned context_length = 0;   // 0 = Auto (see prospero_kv::choose_auto)
    const char* kv_cache = nullptr; // nullptr/"auto", "f16", "q8_0", "q4_0"
    bool thinking = false;
    // Optional benchmark overrides. Zero keeps the app/file default. Changing either
    // recreates the llama context, so production callers should leave them zero.
    unsigned batch_length = 0;
    unsigned ubatch_length = 0;
};

enum : std::uint32_t
{
    GPT_IMAGE_CAP_TEXT_TO_IMAGE = 1u << 0,
    GPT_IMAGE_CAP_IMAGE_TO_IMAGE = 1u << 1,
    GPT_IMAGE_CAP_INPAINTING = 1u << 2,
    GPT_IMAGE_CAP_CONTROLNET = 1u << 3,
};

struct gpt_runtime_image_defaults_t
{
    unsigned width, height, steps;
    float cfg_scale;
    const char *sampler, *scheduler;
};

struct gpt_runtime_image_options_t
{
    const char *workflow = nullptr;
    unsigned width = 0, height = 0, steps = 0;
    std::int64_t seed = -1;
    float cfg_scale = 0;
    float strength = -1;
    float control_strength = -1;
    bool canny = false;
    unsigned upscale = 0;  // 0 off, else the Real-ESRGAN factor (2 or 4)
    const char *negative_prompt = nullptr;
    const char *sampler = nullptr;
    const char *scheduler = nullptr;
    const char *image_path = nullptr;
    const char *mask_path = nullptr;
    const char *control_image_path = nullptr;
    const char *image_edit_strategy = nullptr;
};

struct gpt_runtime_stats_t
{
    unsigned prompt_tokens;
    unsigned generated_tokens;
    unsigned reused_tokens;
    std::uint64_t prefill_microseconds;
    std::uint64_t elapsed_microseconds;
    bool output_limit_reached = false;
};

using gpt_runtime_progress_fn = void (*)(const char *text);

// Snapshot of actual backend residency; empty buffers when no model is loaded.
bool gpt_runtime_loaded_model(char *id, std::size_t id_capacity, char *name, std::size_t name_capacity);
bool gpt_runtime_available();
// Number of active model generations across text, image, audio, and speech.
unsigned gpt_runtime_generations_running();
const char *gpt_runtime_name();
const char *gpt_runtime_backend();
const char *gpt_runtime_purpose();
unsigned gpt_runtime_model_count();
unsigned gpt_runtime_selected_model();
const char *gpt_runtime_model_id(unsigned index);
const char *gpt_runtime_model_name(unsigned index);
const char *gpt_runtime_model_purpose(unsigned index);
bool gpt_runtime_model_image_metadata(unsigned index, std::uint32_t *capabilities,
                                      gpt_runtime_image_defaults_t *defaults,
                                      const char **edit_strategy);
bool gpt_runtime_select_model(unsigned index);
// Delete an installed model when it is not selected and inference is idle.
bool gpt_runtime_delete_model(const char *id);
// Release the selected runtime after it has been idle for timeout_seconds. A later
// generation lazily prepares that same model again. Safe to call from the UI loop.
void gpt_runtime_check_idle_unload(unsigned timeout_seconds);
void gpt_runtime_check_configured_idle_unload();
void gpt_runtime_set_idle_unload_timeout(unsigned timeout_seconds);
void gpt_runtime_refresh_models();
int gpt_runtime_prepare();
bool gpt_runtime_context_full();
// Reported total Vulkan device memory in bytes, or 0 when the backend is not available
// (host tests, or called before the Vulkan backend has registered its device).
std::uint64_t gpt_runtime_vulkan_heap_bytes();
// Device-wide Vulkan free/total memory snapshot; false means the snapshot is unavailable.
bool gpt_runtime_vulkan_memory(std::uint64_t *free_bytes, std::uint64_t *total_bytes);
// Enlarge an existing picture with Real-ESRGAN; factor is 2 or 4. On success `output`
// holds the new file's path, otherwise a message for the caller. Returns 0 on success.
int gpt_runtime_upscale_image(const char *source_path, unsigned factor, char *output,
                              std::size_t output_capacity);
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned message_count,
                         const gpt_runtime_settings_t &settings, char *output,
                         std::size_t output_capacity, gpt_runtime_stats_t *stats,
                         gpt_runtime_progress_fn progress,
                         const gpt_runtime_image_options_t *image_options = nullptr);
