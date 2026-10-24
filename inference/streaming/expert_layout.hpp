// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "expert_cache.hpp"
#include <string>
#include <vector>

namespace prospero_streaming {

// One routed expert tensor of a GGUF: where its experts are and how wide one is.
struct RoutedExpertTensor {
    std::string name;
    int layer = 0;
    int type = 0;                  // ggml_type of the stored weights
    ExpertExtent extent{};
    std::uint64_t bytes_per_expert = 0;
    // An expert-major matrix (rank 3) is what the scheduler streams per token.
    // A per-expert scalar vector (rank 1, a few bytes per expert) is counted in
    // the read budget but is far too small to stream: it stays resident.
    bool expert_major = false;
};

struct RoutedExpertLayout {
    std::string architecture;
    unsigned experts = 0, experts_used = 0, layers = 0;
    std::uint64_t file_bytes = 0, max_expert_bytes = 0;
    std::vector<RoutedExpertTensor> tensors;   // sorted by layer, then file order

    // Sum of one expert's bytes in every tensor, times the experts a token uses:
    // what an uncached token costs in reads.
    std::uint64_t per_token_bytes(unsigned experts_per_token) const;
    // Resident bytes for `slots` experts of every tensor, slots aligned as the
    // cache aligns them.
    std::uint64_t cache_bytes(unsigned slots) const;
};

// Reads the GGUF's metadata and tensor extents. Throws on a file it cannot use.
RoutedExpertLayout read_routed_expert_layout(const std::string &path);

} // namespace prospero_streaming
