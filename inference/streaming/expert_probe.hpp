// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prospero_streaming {

// Replays a routed-expert access pattern over a GGUF through the bounded cache
// and reports what the reads cost. This measures file I/O and cache behaviour
// only: it loads no model, allocates no backend buffers and runs no inference.
struct ExpertProbeRequest {
    std::string path;                // GGUF to read
    unsigned slots = 8;              // cache slots, each max_expert_bytes wide
    unsigned tokens = 2;             // routing steps to replay
    unsigned experts_per_token = 0;  // 0 takes the file's expert_used_count
    unsigned layers = 0;             // 0 takes every routed layer
    std::uint64_t seed = 1;          // routing is deterministic for a seed
};

struct ExpertProbeResult {
    bool ok = false;
    std::string error;
    // File layout
    std::string architecture;
    unsigned routed_layers = 0, routed_tensors = 0, experts = 0, experts_used = 0;
    std::uint64_t file_bytes = 0, max_expert_bytes = 0, per_token_bytes = 0;
    // Replay
    unsigned replayed_layers = 0, tokens = 0;
    std::size_t slots = 0, allocated_bytes = 0;
    std::uint64_t reads = 0, hits = 0, misses = 0, evictions = 0, bytes_read = 0;
    // First token reads nothing warm; later tokens can reuse pinned-free slots.
    double cold_ms = 0, warm_ms = 0;
    std::uint64_t cold_bytes = 0, warm_bytes = 0;
};

// Never throws: a rejected request or a read failure comes back in `error`.
ExpertProbeResult probe_expert_stream(const ExpertProbeRequest &request);

} // namespace prospero_streaming
