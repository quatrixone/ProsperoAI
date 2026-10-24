// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_streaming.hpp"

namespace prospero::inference::airllm
{

bool apply_ps5_streaming_profile(sd_ctx_params_t &parameters, ModelProfile profile)
{
    if (profile != ModelProfile::ZImageTurbo)
        return false;
    parameters.params_backend = "disk";
    parameters.max_vram = "10.5";
    parameters.disable_prefetch = true;
    parameters.diffusion_conv_direct = true;
    parameters.vae_conv_direct = true;
    return true;
}

} // namespace prospero::inference::airllm
