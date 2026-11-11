// SPDX-License-Identifier: GPL-3.0-or-later
// Every curated image preset must resolve to files it actually installs: the
// adapter used to look for three hardcoded filenames, so eight of ten presets
// could not be loaded on the console.
#include "image_checkpoint_layout.hpp"

#include <cassert>
#include <cstring>
#include <string>

namespace
{

prospero_image_layout::CheckpointLayout layout_for(const char *id)
{
    const std::string root = std::string("/models/") + id;
    return prospero_image_layout::checkpoint_layout(root.c_str());
}

bool declares(const char *id, const char *relative)
{
    for (const auto &preset : prospero_model_download::presets)
        if (std::strcmp(preset.id, id) == 0)
            for (std::size_t i = 0; i < preset.count; ++i)
                if (std::strcmp(preset.files[i].path, relative) == 0)
                    return true;
    return false;
}

void expect_checkpoint(const char *id, const char *relative)
{
    const auto layout = layout_for(id);
    assert(layout.found);
    assert(declares(id, relative));
    assert(layout.model == std::string("/models/") + id + "/" + relative);
}

} // namespace

int main()
{
    // Single-file checkpoints, with and without a bundled VAE.
    expect_checkpoint("sdxs-512-q8-0", "sdxs-512-tinySDdistilled_Q8_0.gguf");
    assert(layout_for("sdxs-512-q8-0").vae[0] == '\0');
    assert(!layout_for("sdxs-512-q8-0").sdxl);

    expect_checkpoint("sd15-inpainting-q8-0", "stable-diffusion-v1-5-inpainting-Q8_0.gguf");
    assert(!layout_for("sd15-inpainting-q8-0").sdxl);

    expect_checkpoint("sdxl-base-q8-0", "stable-diffusion-xl-base-1.0-Q8_0.gguf");
    assert(layout_for("sdxl-base-q8-0").sdxl);
    assert(layout_for("sdxl-base-q8-0").vae ==
           std::string("/models/sdxl-base-q8-0/vae/sdxl_vae_fp16_fix.safetensors"));
    assert(layout_for("sdxl-base-q8-0").control_net[0] == '\0');

    expect_checkpoint("raehoshi-illustxl-1-1-q8-0", "raehoshiIllustXL_v110.safetensors");
    expect_checkpoint("ilustmix-v11-1-q8-0", "ilustmix_v111.safetensors");
    expect_checkpoint("sdxl-inpainting-q8-0", "stable-diffusion-xl-inpainting-1.0-Q8_0.gguf");

    // ControlNet is a third file beside the checkpoint and its VAE.
    expect_checkpoint("sdxl-canny-q8-0", "stable-diffusion-xl-base-1.0-Q8_0.gguf");
    assert(layout_for("sdxl-canny-q8-0").control_net ==
           std::string("/models/sdxl-canny-q8-0/controlnet/xinsir-canny-sdxl-v2.safetensors"));

    // FLUX.2 Klein keeps its diffusion model, text encoder and VAE apart.
    const auto flux2 = layout_for("flux2-klein-4b-q8-0");
    assert(flux2.found && flux2.flux2 && !flux2.sdxl);
    assert(flux2.model ==
           std::string("/models/flux2-klein-4b-q8-0/diffusion_models/flux-2-klein-4b-Q8_0.gguf"));
    assert(flux2.llm ==
           std::string(
               "/models/flux2-klein-4b-q8-0/text_encoders/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));
    assert(flux2.vae == std::string("/models/flux2-klein-4b-q8-0/vae/flux2-vae.safetensors"));

    // Realistic Vision ships a top-level checkpoint with a flat vae.safetensors.
    expect_checkpoint("realistic-vision-v51-fp16", "Realistic_Vision_V5.1_fp16-no-ema.safetensors");
    assert(layout_for("realistic-vision-v51-fp16").vae ==
           std::string("/models/realistic-vision-v51-fp16/vae.safetensors"));
    assert(!layout_for("realistic-vision-v51-fp16").sdxl);

    // sd-turbo is a diffusers folder: no checkpoint, so the loader keeps the folder.
    assert(!layout_for("sd-turbo-fp16").found);

    // Text, audio and unknown folders are not image checkpoints.
    assert(!layout_for("minicpm5-2b-q8-0.gguf").found);
    assert(!layout_for("not-a-preset").found);
    assert(!prospero_image_layout::checkpoint_layout("").found);
    assert(!prospero_image_layout::checkpoint_layout(nullptr).found);

    // Every installed image preset must resolve, so none can silently stay unloadable.
    for (const auto &preset : prospero_model_download::presets)
    {
        if (preset.kind != 1)
            continue;
        const auto layout = layout_for(preset.id);
        const bool diffusers = std::strcmp(preset.id, "sd-turbo-fp16") == 0;
        assert(layout.found != diffusers);
    }
    return 0;
}
