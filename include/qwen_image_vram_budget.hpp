// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "inference_config.hpp"

#include <cstdio>
#include <string>

namespace prospero_sd {
inline std::string qwen_image_max_vram(const char *root) {
    const unsigned budget_mib = prospero::read_inference_setting(
        root, "vram_budget_mb.txt", 0, 1024, 16 * 1024);
    // stable-diffusion.cpp interprets a negative budget as free VRAM minus that GiB reserve.
    if (budget_mib == 0) return "-2";

    char budget_gib[24];
    std::snprintf(budget_gib, sizeof(budget_gib), "%.6f",
                  static_cast<double>(budget_mib) / 1024.0);
    return budget_gib;
}
}
