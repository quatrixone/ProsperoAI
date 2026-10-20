// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_cache.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace prospero_streaming {
namespace {
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (!a || !b || a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::invalid_argument("Invalid or overflowing expert extent");
    return a * b;
}
void check_cancel(const std::atomic<bool> *cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw ReadCancelled();
}
}

struct ExpertCache::State {
    struct Slot {
        std::uint64_t offset = 0, age = 0;
        std::size_t size = 0, pins = 0;
        bool valid = false;
    };
    int fd = -1;
    bool owns_fd = false;
    std::uint64_t file_size = 0, clock = 0;
    std::size_t stride = 0, max_expert = 0;
    std::uint8_t *payload = nullptr;
    std::vector<Slot> slots;
    Stats stats;
    mutable std::mutex mutex;
    ~State() {
        if (owns_fd && fd >= 0) close(fd);
        std::free(payload);
    }
};

ExpertCache::ExpertCache(const char *path, std::size_t budget, std::size_t max_expert)
    : state_(std::make_shared<State>()) {
    auto &s = *state_;
    allocate(s, budget, max_expert);
    s.fd = open(path, O_RDONLY);
    if (s.fd < 0) throw std::system_error(errno, std::generic_category(), "Open model file");
    s.owns_fd = true;
    measure(s);
}

ExpertCache::ExpertCache(int fd, std::size_t budget, std::size_t max_expert)
    : state_(std::make_shared<State>()) {
    auto &s = *state_;
    allocate(s, budget, max_expert);
    if (fd < 0) throw std::invalid_argument("Expert cache requires an open model file");
    s.fd = fd;
    measure(s);
}

void ExpertCache::measure(State &s) {
    struct stat st{};
    if (fstat(s.fd, &st)) throw std::system_error(errno, std::generic_category(), "Stat model file");
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        throw std::invalid_argument("Expert cache requires a regular model file");
    s.file_size = static_cast<std::uint64_t>(st.st_size);
}

void ExpertCache::allocate(State &s, std::size_t budget, std::size_t max_expert) {
    constexpr std::size_t alignment = 64;
    if (!max_expert || max_expert > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        throw std::invalid_argument("Invalid expert cache slot size");
    s.max_expert = max_expert;
    s.stride = (max_expert + alignment - 1) & ~(alignment - 1);
    const auto count = budget / s.stride;
    if (!count) throw std::invalid_argument("Expert cache budget cannot hold one aligned slot");
    s.slots.resize(count);
    void *data = nullptr;
    const auto bytes = count * s.stride; // bounded by budget
    const int result = posix_memalign(&data, alignment, bytes);
    if (result) throw std::system_error(result, std::generic_category(), "Allocate expert cache");
    s.payload = static_cast<std::uint8_t *>(data);
    s.stats.allocated_bytes = bytes;
    s.stats.slots = count;
}

ExpertCache::Lease::Lease(std::shared_ptr<State> state, std::size_t slot, std::size_t size)
    : state_(std::move(state)), slot_(slot), size_(size) {}
ExpertCache::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_), size_(other.size_) {
    other.size_ = 0;
}
ExpertCache::Lease &ExpertCache::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        size_ = other.size_;
        other.size_ = 0;
    }
    return *this;
}
ExpertCache::Lease::~Lease() { release(); }
void ExpertCache::Lease::release() noexcept {
    if (state_) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            --state_->slots[slot_].pins;
        }
        state_.reset();
    }
    size_ = 0;
}
const std::uint8_t *ExpertCache::Lease::data() const noexcept {
    return state_ ? state_->payload + slot_ * state_->stride : nullptr;
}

ExpertCache::Lease ExpertCache::acquire(const ExpertExtent &extent, std::uint64_t expert,
                                      const std::atomic<bool> *cancel) {
    check_cancel(cancel);
    auto &s = *state_;
    const auto bytes = multiply(extent.row_bytes, extent.rows_per_expert);
    const auto tensor_bytes = multiply(bytes, extent.experts);
    if (expert >= extent.experts || bytes > s.max_expert ||
        extent.offset > s.file_size || tensor_bytes > s.file_size - extent.offset)
        throw std::invalid_argument("Expert range exceeds tensor, file or cache slot");
    const auto offset = extent.offset + expert * bytes; // total extent checked above
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset)
        throw std::invalid_argument("Expert range exceeds positional I/O offsets");

    // Serialize cache mutation and file I/O. Held leases can be consumed in
    // parallel; their bytes cannot be overwritten. No worker threads or waits
    // for pinned slots are created by this baseline implementation.
    std::lock_guard<std::mutex> lock(s.mutex);
    check_cancel(cancel);
    std::size_t victim = s.slots.size();
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        auto &slot = s.slots[i];
        if (slot.valid && slot.offset == offset && slot.size == bytes) {
            ++slot.pins;
            slot.age = ++s.clock;
            ++s.stats.hits;
            return Lease(state_, i, bytes);
        }
        if (!slot.pins && (victim == s.slots.size() ||
            (s.slots[victim].valid && (!slot.valid || slot.age < s.slots[victim].age))))
            victim = i;
    }
    if (victim == s.slots.size()) throw CacheBusy();
    auto &slot = s.slots[victim];
    ++s.stats.misses;
    if (slot.valid) ++s.stats.evictions;
    slot.valid = false; // partial/failed reads must never become cache hits
    auto *data = s.payload + victim * s.stride;
    std::size_t done = 0;
    while (done < bytes) {
        check_cancel(cancel);
        const auto count = std::min<std::size_t>(bytes - done, 1024 * 1024);
        const auto n = pread(s.fd, data + done, count, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::system_error(errno, std::generic_category(), "Read expert weights");
        if (!n) throw std::runtime_error("Unexpected EOF reading expert weights");
        done += static_cast<std::size_t>(n);
        s.stats.bytes_read += static_cast<std::size_t>(n);
    }
    check_cancel(cancel);
    slot.offset = offset;
    slot.size = bytes;
    slot.age = ++s.clock;
    slot.pins = 1;
    slot.valid = true;
    return Lease(state_, victim, bytes);
}

ExpertCache::Stats ExpertCache::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto result = state_->stats;
    for (const auto &slot : state_->slots) result.pinned_slots += slot.pins != 0;
    return result;
}
} // namespace prospero_streaming
