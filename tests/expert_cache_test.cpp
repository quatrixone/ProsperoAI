// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_cache.hpp"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace prospero_streaming;

template<class E, class F> void rejects(F f) {
    bool caught = false;
    try { f(); } catch (const E &) { caught = true; }
    assert(caught);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    std::string path = std::string(argv[1]) + "/expert-cache-fixture-XXXXXX";
    int fd = mkstemp(path.data());
    assert(fd >= 0);
    constexpr std::size_t header = 96, expert_bytes = 512, experts = 8;
    constexpr std::size_t scale_bytes = 4, matrix_end = header + expert_bytes * experts;
    std::vector<std::uint8_t> file(matrix_end + scale_bytes * experts);
    for (std::size_t i = 0; i < file.size(); ++i) file[i] = (i * 71 + i / 101) % 251;
    assert(write(fd, file.data(), file.size()) == static_cast<ssize_t>(file.size()));
    assert(lseek(fd, 17, SEEK_SET) == 17);

    const ExpertExtent matrix{header, 128, 4, experts};          // 512 bytes per expert
    const ExpertExtent scale{matrix_end, scale_bytes, 1, experts}; // 4 bytes per expert
    const auto *matrix_data = file.data() + header;
    const auto *scale_data = file.data() + matrix_end;

    // A slot is exactly one expert wide, so a narrow tensor costs almost nothing
    // and the budget covers the pools it was asked for, not the widest tensor.
    ExpertCache cache(fd, 2 * expert_bytes + 4 * 64);
    const auto matrix_pool = cache.add_extent(matrix, 2);
    const auto scale_pool = cache.add_extent(scale, 4);
    assert(matrix_pool == 0 && scale_pool == 1 && cache.pool_count() == 2);
    assert(cache.bytes_per_expert(matrix_pool) == expert_bytes);
    assert(cache.bytes_per_expert(scale_pool) == scale_bytes);
    assert(cache.stats().allocated_bytes == 2 * expert_bytes + 4 * 64);
    assert(cache.stats().slots == 6 && cache.stats().pools == 2);
    rejects<CacheBudgetExceeded>([&] { cache.add_extent(matrix, 1); });  // budget is spent
    rejects<std::invalid_argument>([&] { cache.add_extent(matrix, 0); });
    rejects<std::invalid_argument>([&] { cache.bytes_per_expert(7); });

    auto first = cache.acquire(matrix_pool, 3);
    const auto *original = first.data();
    assert(reinterpret_cast<std::uintptr_t>(original) % 64 == 0);
    assert(first.size() == expert_bytes);
    assert(!std::memcmp(original, matrix_data + 3 * expert_bytes, expert_bytes));
    assert(lseek(fd, 0, SEEK_CUR) == 17); // pread must not move the loader's cursor
    auto same = cache.acquire(matrix_pool, 3);
    assert(same.data() == original);

    // Pools are independent: reading every scale expert evicts no matrix slot.
    for (std::uint64_t expert = 0; expert < experts; ++expert) {
        auto lease = cache.acquire(scale_pool, expert);
        assert(lease.size() == scale_bytes);
        assert(!std::memcmp(lease.data(), scale_data + expert * scale_bytes, scale_bytes));
    }
    assert(!std::memcmp(same.data(), matrix_data + 3 * expert_bytes, expert_bytes));

    auto second = cache.acquire(matrix_pool, 0);
    rejects<CacheBusy>([&] { cache.acquire(matrix_pool, 7); });
    first = {};
    rejects<CacheBusy>([&] { cache.acquire(matrix_pool, 7); }); // `same` still pins expert 3
    second = {};
    auto seventh = cache.acquire(matrix_pool, 7);               // evicts expert 0, the oldest
    assert(same.data() == original);
    assert(cache.stats().pinned_slots == 2);
    same = {};
    seventh = {};
    assert(cache.stats().pinned_slots == 0);

    // Concurrent clients may share a lease, but a pinned slot is never evicted.
    std::vector<std::thread> clients;
    for (int i = 0; i < 8; ++i) clients.emplace_back([&] {
        for (int j = 0; j < 100; ++j) {
            auto lease = cache.acquire(matrix_pool, 7);
            assert(!std::memcmp(lease.data(), matrix_data + 7 * expert_bytes, expert_bytes));
        }
    });
    for (auto &client : clients) client.join();
    const auto busy = cache.stats();
    // One hit for the repeated expert 3, then 800 from the threads. The scale
    // sweep of eight experts through four slots is eight misses and four
    // evictions; expert 7 evicting expert 0 in the matrix pool is the fifth.
    assert(busy.hits == 801 && busy.evictions == 5);
    assert(busy.bytes_read == 3 * expert_bytes + experts * scale_bytes);

    rejects<std::invalid_argument>([&] { cache.acquire(matrix_pool, experts); });
    rejects<std::invalid_argument>([&] { cache.acquire(9, 0); });
    rejects<std::invalid_argument>([&] { cache.add_extent({header, 0, 4, experts}, 1); });
    rejects<std::invalid_argument>([&] { cache.add_extent({header, 1 << 20, 4, experts}, 1); });
    rejects<std::invalid_argument>([&] {
        cache.add_extent({std::numeric_limits<std::uint64_t>::max(), 128, 4, experts}, 1);
    });
    rejects<std::invalid_argument>([&] {
        cache.add_extent({0, std::numeric_limits<std::uint64_t>::max(), 4, experts}, 1);
    });
    std::atomic<bool> cancelled{true};
    rejects<ReadCancelled>([&] { cache.acquire(matrix_pool, 7, &cancelled); }); // hits also cancel
    rejects<ReadCancelled>([&] { cache.acquire(matrix_pool, 1, &cancelled); });
    assert(cache.stats().bytes_read == busy.bytes_read);

    // Failed reads are never published as valid hits, including partial EOF.
    {
        ExpertCache damaged(fd, expert_bytes);
        const auto pool = damaged.add_extent(matrix, 1);
        assert(ftruncate(fd, header + 5 * expert_bytes + 17) == 0);
        rejects<std::runtime_error>([&] { damaged.acquire(pool, 5); });
        assert(damaged.stats().bytes_read == 17 && damaged.stats().pinned_slots == 0);
        assert(pwrite(fd, file.data(), file.size(), 0) == static_cast<ssize_t>(file.size()));
        auto retried = damaged.acquire(pool, 5);
        assert(damaged.stats().hits == 0 && damaged.stats().misses == 2);
        assert(!std::memcmp(retried.data(), matrix_data + 5 * expert_bytes, expert_bytes));
    }

    // A path-constructed cache owns its descriptor, so it needs no dup(), which
    // PS5 rejects. A lease keeps its bytes after that cache and the file are gone.
    ExpertCache::Lease survivor;
    {
        ExpertCache owned(path.c_str(), expert_bytes);
        survivor = owned.acquire(owned.add_extent(matrix, 1), 2);
        close(fd);
        unlink(path.c_str());
    }
    assert(!std::memcmp(survivor.data(), matrix_data + 2 * expert_bytes, expert_bytes));
    rejects<std::invalid_argument>([&] { ExpertCache(-1, expert_bytes); });
    rejects<std::system_error>([&] {
        ExpertCache((std::string(argv[1]) + "/absent.gguf").c_str(), expert_bytes);
    });
    // Parallel reads must return the same bytes, and a cancelled or failed range
    // must not publish a slot. Workers are rejected outside 1..32.
    rejects<std::invalid_argument>([&] { ExpertCache(path.c_str(), expert_bytes, 0); });
    rejects<std::invalid_argument>([&] { ExpertCache(path.c_str(), expert_bytes, 33); });
    {
        const std::string parallel_path = std::string(argv[1]) + "/expert-cache-parallel";
        const int parallel_fd = open(parallel_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        assert(parallel_fd >= 0);
        constexpr std::size_t wide_rows = 8192;                    // 1 MiB per expert
        std::vector<std::uint8_t> wide(header + 128 * wide_rows * experts);
        for (std::size_t i = 0; i < wide.size(); ++i) wide[i] = (i * 37 + i / 97) % 253;
        assert(write(parallel_fd, wide.data(), wide.size()) == static_cast<ssize_t>(wide.size()));
        const ExpertExtent extent{header, 128, wide_rows, experts};
        const auto expert_width = 128 * wide_rows;
        for (const unsigned workers : {1u, 2u, 4u, 8u}) {
            ExpertCache parallel(parallel_fd, 2 * expert_width, workers);
            assert(parallel.stats().read_workers == workers);
            const auto pool = parallel.add_extent(extent, 2);
            for (std::uint64_t expert : {5u, 2u, 5u}) {
                auto lease = parallel.acquire(pool, expert);
                assert(lease.size() == expert_width);
                assert(!std::memcmp(lease.data(), wide.data() + header + expert * expert_width,
                                    expert_width));
            }
            assert(parallel.stats().hits == 1 && parallel.stats().bytes_read == 2 * expert_width);
            std::atomic<bool> stop{true};
            rejects<ReadCancelled>([&] { parallel.acquire(pool, 7, &stop); });
            assert(parallel.stats().pinned_slots == 0);
            auto after_cancel = parallel.acquire(pool, 7);        // the cancelled slot is refilled
            assert(!std::memcmp(after_cancel.data(), wide.data() + header + 7 * expert_width,
                                expert_width));
        }
        close(parallel_fd);
        unlink(parallel_path.c_str());
    }
    std::cout << "expert cache: pools, exact slot width, budget, pins, eviction, cancellation, "
                 "EOF, concurrency, parallel reads and lifetime passed\n";
}
