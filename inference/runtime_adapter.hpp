// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gpt_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace prospero::inference
{

// Stable application-level modality IDs. Model catalogs and backend libraries must
// translate to these instead of sharing backend-specific enum values.
enum class Modality : std::uint8_t
{
    Text = 0,
    Image = 1,
    Audio = 2,
    Speech = 3,
};

struct Model
{
    std::string id;
    std::string name;
    std::string root;
    Modality modality = Modality::Text;
    unsigned backend_index = 0;
};

struct GenerationRequest
{
    const Model &model;
    const gpt_runtime_message_t *messages;
    unsigned message_count;
    const gpt_runtime_settings_t &settings;
    char *output;
    std::size_t output_capacity;
    gpt_runtime_stats_t *stats;
    gpt_runtime_progress_fn progress;
    const gpt_runtime_image_options_t *image_options = nullptr;
};

struct Adapter
{
    Modality modality;
    const char *name;
    int (*generate)(const GenerationRequest &request);
    void (*release)();
};

const Adapter *find_adapter(Modality modality);
// Enlarge an existing picture with Real-ESRGAN, no catalog model involved.
int upscale_image(const char *source_path, unsigned factor, char *output,
                  std::size_t output_capacity);
const char *modality_name(Modality modality);
bool is_media(Modality modality);

} // namespace prospero::inference
