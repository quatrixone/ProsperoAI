#pragma once
// PS5 direct-memory accounting. sceKernelAvailableDirectMemorySize reports only the
// largest contiguous free block; after models load and unload the pool is split, so
// that figure can be half of what is actually free. Vulkan buffers on the console are
// at most 128 MiB each, so the total is what decides whether a model fits.
#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
int sceKernelDirectMemoryQuery(std::int64_t offset, int flags, void *info, std::size_t info_size);
int sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t *, std::size_t *);
int sceKernelAvailableFlexibleMemorySize(std::size_t *);
}

namespace prospero_memory {

// Pool size minus every allocated range.
inline std::uint64_t free_total()
{
    struct { std::int64_t start, end; int type; } info{};
    constexpr int kFindNext = 1;
    const std::int64_t pool = sceKernelGetDirectMemorySize();
    if (pool <= 0)
        return 0;
    std::uint64_t used = 0;
    for (std::int64_t offset = 0; offset < pool;) {
        if (sceKernelDirectMemoryQuery(offset, kFindNext, &info, sizeof(info)) != 0 || info.end <= offset)
            break;
        used += static_cast<std::uint64_t>(info.end - std::max(info.start, offset));
        offset = info.end;
    }
    return used < static_cast<std::uint64_t>(pool) ? static_cast<std::uint64_t>(pool) - used : 0;
}

inline std::uint64_t largest_free_block()
{
    std::int64_t start = 0;
    std::size_t bytes = 0;
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x4000, &start, &bytes);
    return bytes;
}

// Flexible (CPU heap) memory left in the process budget.
inline std::uint64_t flexible_free()
{
    std::size_t bytes = 0;
    return sceKernelAvailableFlexibleMemorySize(&bytes) == 0 ? bytes : 0;
}

// Hole-map probe (20.10, fragmentation hunt): every ALLOCATED range in the
// direct pool, MiB-rounded. The diff between a clean boot and a
// model-load/release cycle names the allocations that outlive release and
// split the largest free block.
struct BlockInfo
{
    std::int64_t start_mb;
    std::int64_t size_mb;
    int type;
};
inline int walk_blocks(BlockInfo *out, int max_blocks, std::int64_t *pool_mib)
{
    struct { std::int64_t start, end; int type; } info{};
    constexpr int kFindNext = 1;
    const std::int64_t pool = sceKernelGetDirectMemorySize();
    if (pool_mib)
        *pool_mib = pool >> 20;
    if (pool <= 0)
        return 0;
    int n = 0;
    for (std::int64_t offset = 0; offset < pool && n < max_blocks;)
    {
        if (sceKernelDirectMemoryQuery(offset, kFindNext, &info, sizeof(info)) != 0 ||
            info.end <= offset)
            break;
        out[n++] = {info.start >> 20, (info.end - std::max(info.start, offset)) >> 20, info.type};
        offset = info.end;
    }
    return n;
}

} // namespace prospero_memory
