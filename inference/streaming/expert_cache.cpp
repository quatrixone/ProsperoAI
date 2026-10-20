// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_cache.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace prospero_streaming {
namespace {
constexpr std::size_t alignment = 64;

std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (!a || !b || a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::invalid_argument("Invalid or overflowing expert extent");
    return a * b;
}
void check_cancel(const std::atomic<bool> *cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw ReadCancelled();
}
}

// A fixed set of reader threads. One expert's extent is split into contiguous
// ranges so several positional reads are in flight at once, as the model loader
// already does; the caller waits for all of them, so a failed range can never
// leave a slot half filled and published. Ranges still to hand out and ranges
// still running are counted separately: one number for both deadlocks.
class ReadPool {
public:
    explicit ReadPool(unsigned workers) : workers_(workers) {
        for (unsigned i = 0; i + 1 < workers_; ++i)
            threads_.emplace_back([this] { serve(); });
    }
    ~ReadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_.notify_all();
        for (auto &thread : threads_) thread.join();
    }
    unsigned workers() const { return workers_; }

    // Reads [offset, offset + bytes) into data. Returns the bytes read, and sets
    // `failure` to the first error any range hit.
    std::size_t read(int fd, std::uint8_t *data, std::size_t bytes, std::uint64_t offset,
                     const std::atomic<bool> *cancel, std::exception_ptr &failure) {
        constexpr std::size_t min_range = 256 * 1024;
        const std::size_t ranges = std::max<std::size_t>(1,
            std::min<std::size_t>(workers_, bytes / min_range));
        const std::size_t span = (bytes + ranges - 1) / ranges;
        Job job{fd, data, bytes, offset, span, cancel};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = job;
            total_ = ranges;
            next_ = 1;              // range 0 belongs to the calling thread
            outstanding_ = ranges;
            done_ = 0;
            failure_ = nullptr;
        }
        if (ranges > 1) work_.notify_all();
        execute(job, 0);
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [this] { return outstanding_ == 0; });
        total_ = next_ = 0;         // nothing more of this job may be handed out
        failure = failure_;
        return done_;
    }
private:
    struct Job {
        int fd = -1;
        std::uint8_t *data = nullptr;
        std::size_t bytes = 0;
        std::uint64_t offset = 0;
        std::size_t span = 0;
        const std::atomic<bool> *cancel = nullptr;
    };

    void serve() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            work_.wait(lock, [this] { return stopping_ || next_ < total_; });
            if (stopping_) return;
            const std::size_t range = next_++;
            const Job job = job_;
            lock.unlock();
            execute(job, range);
            lock.lock();
        }
    }

    void execute(const Job &job, std::size_t range) {
        std::size_t done = 0;
        std::exception_ptr failure;
        const std::size_t start = range * job.span;
        const std::size_t count = start >= job.bytes ? 0 : std::min(job.span, job.bytes - start);
        try {
            while (done < count) {
                if (job.cancel && job.cancel->load(std::memory_order_relaxed)) throw ReadCancelled();
                const auto chunk = std::min<std::size_t>(count - done, 1024 * 1024);
                const auto n = pread(job.fd, job.data + start + done, chunk,
                                     static_cast<off_t>(job.offset + start + done));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) throw std::system_error(errno, std::generic_category(), "Read expert weights");
                if (!n) throw std::runtime_error("Unexpected EOF reading expert weights");
                done += static_cast<std::size_t>(n);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_ += done;
        if (failure && !failure_) failure_ = failure;
        if (--outstanding_ == 0) finished_.notify_all();
    }

    const unsigned workers_;
    std::vector<std::thread> threads_;
    mutable std::mutex mutex_;
    std::condition_variable work_, finished_;
    Job job_;
    std::size_t total_ = 0, next_ = 0, outstanding_ = 0, done_ = 0;
    std::exception_ptr failure_ = nullptr;
    bool stopping_ = false;
};

struct ExpertCache::State {
    struct Slot {
        std::uint64_t offset = 0, age = 0;
        std::size_t pins = 0;
        bool valid = false;
    };
    struct Pool {
        ExpertExtent extent{};
        std::uint64_t bytes_per_expert = 0;
        std::size_t stride = 0;
        std::uint8_t *payload = nullptr;
        std::vector<Slot> slots;
        ~Pool() { std::free(payload); }
    };
    int fd = -1;
    bool owns_fd = false;
    std::uint64_t file_size = 0, clock = 0;
    std::size_t budget = 0, used = 0;
    std::vector<std::unique_ptr<Pool>> pools;
    std::unique_ptr<ReadPool> readers;
    Stats stats;
    mutable std::mutex mutex;
    ~State() {
        if (owns_fd && fd >= 0) close(fd);
    }
};

void ExpertCache::start_readers(State &s, unsigned read_workers) {
    if (!read_workers || read_workers > 32)
        throw std::invalid_argument("read_workers must be 1..32");
    s.readers = std::make_unique<ReadPool>(read_workers);
    s.stats.read_workers = read_workers;
}

void ExpertCache::open_source(State &s, int fd, bool owned) {
    s.fd = fd;
    s.owns_fd = owned;
    struct stat st{};
    if (fstat(s.fd, &st)) throw std::system_error(errno, std::generic_category(), "Stat model file");
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        throw std::invalid_argument("Expert cache requires a regular model file");
    s.file_size = static_cast<std::uint64_t>(st.st_size);
}

ExpertCache::ExpertCache(const char *path, std::size_t budget, unsigned read_workers)
    : state_(std::make_shared<State>()) {
    state_->budget = budget;
    start_readers(*state_, read_workers);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Open model file");
    open_source(*state_, fd, true);
}

ExpertCache::ExpertCache(int fd, std::size_t budget, unsigned read_workers)
    : state_(std::make_shared<State>()) {
    state_->budget = budget;
    start_readers(*state_, read_workers);
    if (fd < 0) throw std::invalid_argument("Expert cache requires an open model file");
    open_source(*state_, fd, false);
}

std::size_t ExpertCache::add_extent(const ExpertExtent &extent, unsigned slots) {
    if (!slots) throw std::invalid_argument("An expert tensor needs at least one slot");
    const auto bytes_per_expert = multiply(extent.row_bytes, extent.rows_per_expert);
    const auto tensor_bytes = multiply(bytes_per_expert, extent.experts);
    auto &s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (extent.offset > s.file_size || tensor_bytes > s.file_size - extent.offset)
        throw std::invalid_argument("Expert tensor extends past the end of the model file");
    if (bytes_per_expert > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        throw std::invalid_argument("Invalid expert cache slot size");
    const auto resident = std::min<std::uint64_t>(slots, extent.experts);
    const std::size_t stride = (static_cast<std::size_t>(bytes_per_expert) + alignment - 1)
                               & ~(alignment - 1);
    if (stride > std::numeric_limits<std::size_t>::max() / resident)
        throw CacheBudgetExceeded();
    const std::size_t bytes = stride * static_cast<std::size_t>(resident);
    if (bytes > s.budget - std::min(s.budget, s.used)) throw CacheBudgetExceeded();

    auto pool = std::make_unique<State::Pool>();
    pool->extent = extent;
    pool->bytes_per_expert = bytes_per_expert;
    pool->stride = stride;
    void *data = nullptr;
    const int result = posix_memalign(&data, alignment, bytes);
    if (result) throw std::system_error(result, std::generic_category(), "Allocate expert cache");
    pool->payload = static_cast<std::uint8_t *>(data);
    pool->slots.resize(static_cast<std::size_t>(resident));
    s.pools.push_back(std::move(pool));
    s.used += bytes;
    s.stats.allocated_bytes += bytes;
    s.stats.slots += static_cast<std::size_t>(resident);
    s.stats.pools = s.pools.size();
    return s.pools.size() - 1;
}

std::size_t ExpertCache::pool_count() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->pools.size();
}

std::uint64_t ExpertCache::bytes_per_expert(std::size_t pool) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (pool >= state_->pools.size()) throw std::invalid_argument("Unknown expert tensor");
    return state_->pools[pool]->bytes_per_expert;
}

ExpertCache::Lease::Lease(std::shared_ptr<State> state, std::size_t pool, std::size_t slot,
                          const std::uint8_t *data, std::size_t size)
    : state_(std::move(state)), pool_(pool), slot_(slot), size_(size), data_(data) {}
ExpertCache::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), pool_(other.pool_), slot_(other.slot_),
      size_(other.size_), data_(other.data_) {
    other.size_ = 0;
    other.data_ = nullptr;
}
ExpertCache::Lease &ExpertCache::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        pool_ = other.pool_;
        slot_ = other.slot_;
        size_ = other.size_;
        data_ = other.data_;
        other.size_ = 0;
        other.data_ = nullptr;
    }
    return *this;
}
ExpertCache::Lease::~Lease() { release(); }
void ExpertCache::Lease::release() noexcept {
    if (state_) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            --state_->pools[pool_]->slots[slot_].pins;
        }
        state_.reset();
    }
    size_ = 0;
    data_ = nullptr;
}

ExpertCache::Lease ExpertCache::acquire(std::size_t pool_index, std::uint64_t expert,
                                        const std::atomic<bool> *cancel) {
    check_cancel(cancel);
    auto &s = *state_;

    // Serialize cache mutation and file I/O. Held leases can be consumed in
    // parallel; their bytes cannot be overwritten. No worker threads or waits
    // for pinned slots are created by this baseline implementation.
    std::lock_guard<std::mutex> lock(s.mutex);
    check_cancel(cancel);
    if (pool_index >= s.pools.size()) throw std::invalid_argument("Unknown expert tensor");
    auto &pool = *s.pools[pool_index];
    if (expert >= pool.extent.experts) throw std::invalid_argument("Expert index outside the tensor");
    const auto bytes = static_cast<std::size_t>(pool.bytes_per_expert);
    const auto offset = pool.extent.offset + expert * pool.bytes_per_expert; // checked on registration
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - bytes)
        throw std::invalid_argument("Expert range exceeds positional I/O offsets");

    std::size_t victim = pool.slots.size();
    for (std::size_t i = 0; i < pool.slots.size(); ++i) {
        auto &slot = pool.slots[i];
        if (slot.valid && slot.offset == offset) {
            ++slot.pins;
            slot.age = ++s.clock;
            ++s.stats.hits;
            return Lease(state_, pool_index, i, pool.payload + i * pool.stride, bytes);
        }
        if (!slot.pins && (victim == pool.slots.size() ||
            (pool.slots[victim].valid && (!slot.valid || slot.age < pool.slots[victim].age))))
            victim = i;
    }
    if (victim == pool.slots.size()) throw CacheBusy();
    auto &slot = pool.slots[victim];
    ++s.stats.misses;
    if (slot.valid) ++s.stats.evictions;
    slot.valid = false; // partial/failed reads must never become cache hits
    auto *data = pool.payload + victim * pool.stride;
    std::exception_ptr failure;
    const auto done = s.readers->read(s.fd, data, bytes, offset, cancel, failure);
    s.stats.bytes_read += done;
    if (failure) std::rethrow_exception(failure);
    if (done != bytes) throw std::runtime_error("Short read of expert weights");
    check_cancel(cancel);
    slot.offset = offset;
    slot.age = ++s.clock;
    slot.pins = 1;
    slot.valid = true;
    return Lease(state_, pool_index, victim, data, bytes);
}

ExpertCache::Stats ExpertCache::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto result = state_->stats;
    for (const auto &pool : state_->pools)
        for (const auto &slot : pool->slots) result.pinned_slots += slot.pins != 0;
    return result;
}
} // namespace prospero_streaming
