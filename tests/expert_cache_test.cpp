// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_cache.hpp"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <system_error>
#include <string>
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
    std::vector<std::uint8_t> file(header + expert_bytes * experts);
    for (std::size_t i = 0; i < file.size(); ++i) file[i] = (i * 71 + i / 101) % 251;
    assert(write(fd, file.data(), file.size()) == static_cast<ssize_t>(file.size()));
    assert(lseek(fd, 17, SEEK_SET) == 17);
    const ExpertExtent extent{header, 128, 4, experts};
    ExpertCache cache(fd, 2 * expert_bytes + 31, expert_bytes);
    assert(cache.stats().allocated_bytes == 2 * expert_bytes);
    auto first = cache.acquire(extent, 3);
    const auto *original = first.data();
    assert(reinterpret_cast<std::uintptr_t>(original) % 64 == 0);
    assert(first.size() == expert_bytes);
    assert(!std::memcmp(original, file.data() + header + 3 * expert_bytes, expert_bytes));
    assert(lseek(fd, 0, SEEK_CUR) == 17); // pread must not move loader's cursor
    auto same = cache.acquire(extent, 3);
    assert(same.data() == original);
    auto second = cache.acquire(extent, 0);
    rejects<CacheBusy>([&] { cache.acquire(extent, 7); });
    first = {};
    rejects<CacheBusy>([&] { cache.acquire(extent, 7); }); // second lease still pins expert 3
    second = {};
    auto seventh = cache.acquire(extent, 7);
    assert(same.data() == original);
    assert(!std::memcmp(same.data(), file.data() + header + 3 * expert_bytes, expert_bytes));
    assert(cache.stats().pinned_slots == 2);
    same = {};
    seventh = {};
    assert(cache.stats().pinned_slots == 0);

    // Concurrent clients may share a lease, but a pinned slot is never evicted.
    std::vector<std::thread> clients;
    for (int i = 0; i < 8; ++i) clients.emplace_back([&] {
        for (int j = 0; j < 100; ++j) {
            auto lease = cache.acquire(extent, 7);
            assert(!std::memcmp(lease.data(), file.data() + header + 7 * expert_bytes, expert_bytes));
        }
    });
    for (auto &client : clients) client.join();
    assert(cache.stats().bytes_read == 3 * expert_bytes);
    assert(cache.stats().hits == 801);
    assert(cache.stats().evictions == 1);

    rejects<std::invalid_argument>([&] { cache.acquire(extent, experts); });
    rejects<std::invalid_argument>([&] { cache.acquire({header, 0, 4, experts}, 0); });
    rejects<std::invalid_argument>([&] { cache.acquire({header, 1024, 4, experts}, 0); });
    rejects<std::invalid_argument>([&] {
        cache.acquire({std::numeric_limits<std::uint64_t>::max(), 128, 4, experts}, 0);
    });
    rejects<std::invalid_argument>([&] {
        cache.acquire({0, std::numeric_limits<std::uint64_t>::max(), 4, experts}, 0);
    });
    std::atomic<bool> cancelled{true};
    rejects<ReadCancelled>([&] { cache.acquire(extent, 7, &cancelled); }); // hits also cancel
    rejects<ReadCancelled>([&] { cache.acquire(extent, 1, &cancelled); });
    assert(cache.stats().bytes_read == 3 * expert_bytes);

    // Failed reads are never published as valid hits, including partial EOF.
    ExpertCache damaged(fd, expert_bytes, expert_bytes);
    assert(ftruncate(fd, header + 5 * expert_bytes + 17) == 0);
    rejects<std::runtime_error>([&] { damaged.acquire(extent, 5); });
    assert(damaged.stats().bytes_read == 17 && damaged.stats().pinned_slots == 0);
    assert(pwrite(fd, file.data(), file.size(), 0) == static_cast<ssize_t>(file.size()));
    const std::string reopened = path;
    auto retried = damaged.acquire(extent, 5);
    assert(damaged.stats().hits == 0 && damaged.stats().misses == 2);
    assert(!std::memcmp(retried.data(), file.data() + header + 5 * expert_bytes, expert_bytes));

    // A path-constructed cache owns its descriptor, so it needs no dup(), which
    // PS5 rejects. A lease keeps its bytes after that cache and the file are gone.
    ExpertCache::Lease survivor;
    {
        ExpertCache owned(reopened.c_str(), expert_bytes, expert_bytes);
        ExpertCache borrowed(fd, expert_bytes, expert_bytes);
        survivor = owned.acquire(extent, 2);
        assert(borrowed.acquire(extent, 2).data() != nullptr);
        close(fd);
        unlink(reopened.c_str());
    }
    rejects<std::invalid_argument>([&] { ExpertCache(-1, expert_bytes, expert_bytes); });
    rejects<std::system_error>([&] {
        ExpertCache((std::string(argv[1]) + "/absent.gguf").c_str(), expert_bytes, expert_bytes);
    });
    assert(!std::memcmp(survivor.data(), file.data() + header + 2 * expert_bytes, expert_bytes));
    std::cout << "expert cache: ranges, pins, eviction, cancellation, EOF, concurrency and lifetime passed\n";
}
