// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "stable-diffusion.h"

namespace prospero::inference::airllm
{

enum class ModelProfile
{
    Standard,
    ZImageTurbo,
};

// Select disk backed parameter staging only for models that exceed ordinary
// PS5 memory residency. The shared SD runner performs the graph staging.
bool apply_ps5_streaming_profile(sd_ctx_params_t &parameters, ModelProfile profile);

} // namespace prospero::inference::airllm
