// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "expert_cache.hpp"
#include "ggml.h"
#include "ggml-backend.h"

namespace prospero_streaming {
// Baseline transfer: retain the source lease until the GPU has consumed it and
// wait for earlier work before overwriting the destination. A future pipelined
// implementation must preserve both lifetimes with backend events.
inline void upload_expert(ExpertCache &cache, const ExpertExtent &extent,
                          std::uint64_t expert, ggml_backend_t backend,
                          ggml_tensor *destination, std::size_t destination_offset = 0,
                          const std::atomic<bool> *cancel = nullptr) {
    if (!backend || !destination || !destination->buffer)
        throw std::invalid_argument("Expert upload requires an allocated destination");
    auto lease = cache.acquire(extent, expert, cancel);
    const auto bytes = ggml_nbytes(destination);
    if (destination_offset > bytes || lease.size() > bytes - destination_offset)
        throw std::invalid_argument("Expert upload exceeds destination tensor");
    ggml_backend_synchronize(backend);
    try {
        ggml_backend_tensor_set_async(backend, destination, lease.data(),
                                      destination_offset, lease.size());
        ggml_backend_synchronize(backend);
    } catch (...) {
        ggml_backend_synchronize(backend);
        throw;
    }
}
} // namespace prospero_streaming
