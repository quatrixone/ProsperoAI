// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "expert_cache.hpp"
#include "ggml-backend.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace prospero_streaming {

// Reports whether `device` will run MUL_MAT_ID itself for an expert tensor of
// this type and shape. If it will not, the scheduler computes routed experts on
// the CPU, which would read weights that are not resident: streaming must then
// be refused rather than attempted. A Vulkan device refuses MUL_MAT_ID when its
// shared memory cannot hold the row ids and the result tile.
bool device_runs_routed_experts(ggml_backend_dev_t device, int type, std::int64_t columns,
                                 std::int64_t rows, std::int64_t experts,
                                 std::int64_t experts_used, std::int64_t tokens,
                                 std::string *reason = nullptr);

// Where one routed expert tensor lives in the model file, and how many of its
// experts stay resident.
struct StreamedTensor {
    ExpertExtent extent;
    unsigned slots = 0;
};

// Serves routed expert weights to the ggml scheduler without keeping them
// resident. A tensor placed in this buffer type gets address space and nothing
// else: its bytes come from the cache when the scheduler asks for the experts a
// token actually uses, through the ggml_backend_set_expert_copy hook. A stray
// dereference of such a tensor faults on a recognizable address rather than
// reading something plausible.
class ExpertStream {
public:
    struct State;   // the buffer-type and hook callbacks need it
    // `resolve` is asked for every tensor allocated in this buffer type. It
    // returns false for a tensor that must not be streamed, which is then
    // rejected: a silent fallback would allocate the whole tensor.
    using Resolver = std::function<bool(const std::string &name, StreamedTensor &out)>;

    ExpertStream(ExpertCache &cache, Resolver resolve);
    ExpertStream(const ExpertStream &) = delete;
    ExpertStream &operator=(const ExpertStream &) = delete;
    ~ExpertStream();

    // The buffer type to allocate routed expert tensors in.
    ggml_backend_buffer_type_t buffer_type();

    // Installs this instance as the scheduler's expert source. One instance can
    // be installed at a time; destruction uninstalls it.
    void install();
    static void uninstall();

    struct Stats {
        std::uint64_t copies = 0, experts = 0, bytes = 0;  // scheduler requests served
        std::uint64_t rejected = 0;                        // requests for unknown tensors
    };
    Stats stats() const;

private:
    std::unique_ptr<State> state_;
};
} // namespace prospero_streaming
