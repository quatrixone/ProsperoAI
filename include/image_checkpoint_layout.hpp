// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model_presets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace prospero_image_layout {

// The curated image presets install a checkpoint plus optional vae/ and controlnet/
// files, and every preset names them differently, so read the layout from the preset
// table instead of hardcoding filenames. A folder that is not a preset install (a
// diffusers directory, or a checkpoint dropped in by hand) leaves found false.
struct CheckpointLayout
{
    bool found;
    bool flux2;
    bool sdxl;
    char model[512];
    char llm[512];
    char vae[512];
    char control_net[512];
};

inline bool path_starts_with(const char *path, const char *prefix)
{
    return std::strncmp(path, prefix, std::strlen(prefix)) == 0;
}

inline bool path_has_suffix(const char *path, const char *suffix)
{
    const std::size_t length = std::strlen(path);
    const std::size_t wanted = std::strlen(suffix);
    return length >= wanted && std::strcmp(path + length - wanted, suffix) == 0;
}

inline CheckpointLayout checkpoint_layout(const char *root)
{
    CheckpointLayout layout{};
    if (!root || !*root)
        return layout;
    const char *separator = std::strrchr(root, '/');
    const std::string id = separator ? separator + 1 : root;
    const auto preset =
        std::find_if(std::begin(prospero_model_download::presets),
                     std::end(prospero_model_download::presets),
                     [&](const auto &entry) { return entry.kind == 1 && id == entry.id; });
    if (preset == std::end(prospero_model_download::presets))
        return layout;
    const auto resolve = [&](char (&target)[512], const char *relative)
    {
        return std::snprintf(target, sizeof(target), "%s/%s", root, relative) <
               static_cast<int>(sizeof(target));
    };
    for (std::size_t i = 0; i < preset->count; ++i)
    {
        const char *path = preset->files[i].path;
        if (path_starts_with(path, "licenses/"))
            continue;
        if (path_starts_with(path, "diffusion_models/"))
        {
            layout.flux2 = true;
            if (!resolve(layout.model, path))
                return {};
        }
        else if (path_starts_with(path, "text_encoders/"))
        {
            if (!resolve(layout.llm, path))
                return {};
        }
        else if (path_starts_with(path, "vae/") || std::strcmp(path, "vae.safetensors") == 0)
        {
            // SDXL models must decode their VAE off the Vulkan heap.
            // shortcut: SDXL is recognised by its bundled fp16-fix VAE, the only family
            // signal the preset table carries; give Preset an explicit family field if a
            // preset ever ships SDXL with a different VAE.
            if (std::strcmp(path, "vae/sdxl_vae_fp16_fix.safetensors") == 0)
                layout.sdxl = true;
            if (!resolve(layout.vae, path))
                return {};
        }
        else if (path_starts_with(path, "controlnet/"))
        {
            if (!resolve(layout.control_net, path))
                return {};
        }
        else if (!layout.model[0] && !std::strchr(path, '/') &&
                 (path_has_suffix(path, ".gguf") || path_has_suffix(path, ".safetensors")))
        {
            if (!resolve(layout.model, path))
                return {};
        }
    }
    // A diffusers preset (sd-turbo) lists only subdirectory files, so no checkpoint is
    // found and the loader is pointed at the folder as before.
    layout.found = layout.model[0] != '\0';
    return layout;
}

} // namespace prospero_image_layout
