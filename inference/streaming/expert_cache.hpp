// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace prospero_streaming {

// Contiguous quantized rows; row_bytes must come from the model's quantization
// layout. For fused gate/up weights rows_per_expert includes both projections.
struct ExpertExtent {
    std::uint64_t offset;
    std::uint64_t row_bytes;
    std::uint64_t rows_per_expert;
    std::uint64_t experts;
};

struct ReadCancelled : std::runtime_error {
    ReadCancelled() : std::runtime_error("Expert read cancelled") {}
};
struct CacheBusy : std::runtime_error {
    CacheBusy() : std::runtime_error("All expert cache slots are in use") {}
};

class ExpertCache {
    struct State;
public:
    struct Stats {
        std::uint64_t hits = 0, misses = 0, evictions = 0, bytes_read = 0;
        std::size_t allocated_bytes = 0, slots = 0, pinned_slots = 0;
    };

    // A lease pins its slot and keeps the file/cache alive. A caller using its
    // bytes for an asynchronous transfer must retain it until transfer completion.
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();
        const std::uint8_t *data() const noexcept;
        std::size_t size() const noexcept { return size_; }
        explicit operator bool() const noexcept { return bool(state_); }
    private:
        friend class ExpertCache;
        Lease(std::shared_ptr<State> state, std::size_t slot, std::size_t size);
        void release() noexcept;
        std::shared_ptr<State> state_;
        std::size_t slot_ = 0, size_ = 0;
    };

    // Duplicates fd. The source must remain immutable while this cache exists.
    // budget_bytes covers aligned payload slots; small bookkeeping is separate.
    // No allocation or eviction can increase the payload budget after construction.
    ExpertCache(int fd, std::size_t budget_bytes, std::size_t max_expert_bytes);
    ExpertCache(const ExpertCache &) = delete;
    ExpertCache &operator=(const ExpertCache &) = delete;
    Lease acquire(const ExpertExtent &extent, std::uint64_t expert,
                  const std::atomic<bool> *cancel = nullptr);
    Stats stats() const;
private:
    std::shared_ptr<State> state_;
};
} // namespace prospero_streaming
