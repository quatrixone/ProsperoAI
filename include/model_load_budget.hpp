// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace prospero_memory {
// Conservative admission for resident GGUF loading, before backend allocations.
// File bytes are an upper estimate of packed weights, not a guarantee of fit:
// KV and graph workspace still have their own post-load checks.
constexpr std::uint64_t model_load_reserve = 768ULL << 20;
inline bool resident_model_fits(std::uint64_t bytes, std::uint64_t free,
                                std::uint64_t total) {
    return bytes && total && free <= total && free > model_load_reserve &&
           bytes <= free - model_load_reserve;
}
}
