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
// A per-expert scalar vector is row_bytes = bytes per expert, rows_per_expert 1.
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
    CacheBusy() : std::runtime_error("All slots for this expert tensor are in use") {}
};
struct CacheBudgetExceeded : std::runtime_error {
    CacheBudgetExceeded() : std::runtime_error("Expert cache budget cannot hold these slots") {}
};

// One pool of slots per registered expert tensor, each slot exactly one expert
// wide. Per-tensor pools are what keep a layer's experts resident while the
// other layers are read, and they waste nothing on narrow tensors; a single
// pool of uniformly wide slots does both badly.
class ExpertCache {
    struct State;
public:
    struct Stats {
        std::uint64_t hits = 0, misses = 0, evictions = 0, bytes_read = 0;
        std::size_t allocated_bytes = 0, slots = 0, pinned_slots = 0, pools = 0;
        unsigned read_workers = 1;
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
        const std::uint8_t *data() const noexcept { return data_; }
        std::size_t size() const noexcept { return size_; }
        explicit operator bool() const noexcept { return bool(state_); }
    private:
        friend class ExpertCache;
        Lease(std::shared_ptr<State> state, std::size_t pool, std::size_t slot,
              const std::uint8_t *data, std::size_t size);
        void release() noexcept;
        std::shared_ptr<State> state_;
        std::size_t pool_ = 0, slot_ = 0, size_ = 0;
        const std::uint8_t *data_ = nullptr;
    };

    // Opens the model itself and owns that descriptor. The source must remain
    // immutable while this cache exists. budget_bytes caps the sum of all pools;
    // no read or eviction can grow it after the pools are registered. PS5 has no
    // usable dup(): the clean-room libc issues syscall 41 directly and the kernel
    // rejects it, so nothing here duplicates a descriptor.
    // read_workers splits one expert's read across that many persistent threads;
    // 1 reads it on the calling thread.
    ExpertCache(const char *path, std::size_t budget_bytes, unsigned read_workers = 1);
    // Borrows fd, which must stay open until this cache is destroyed.
    ExpertCache(int fd, std::size_t budget_bytes, unsigned read_workers = 1);
    ExpertCache(const ExpertCache &) = delete;
    ExpertCache &operator=(const ExpertCache &) = delete;

    // Reserves `slots` resident experts for one tensor and returns its handle.
    // Throws CacheBudgetExceeded when the budget cannot cover the request.
    std::size_t add_extent(const ExpertExtent &extent, unsigned slots);
    std::size_t pool_count() const;
    std::uint64_t bytes_per_expert(std::size_t pool) const;

    Lease acquire(std::size_t pool, std::uint64_t expert,
                  const std::atomic<bool> *cancel = nullptr);
    Stats stats() const;
private:
    static void start_readers(State &state, unsigned read_workers);
    static void open_source(State &state, int fd, bool owned);
    std::shared_ptr<State> state_;
};
} // namespace prospero_streaming
