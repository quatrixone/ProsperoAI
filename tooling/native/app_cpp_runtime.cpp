#define PS5_WRAP_MALLOC
/*
 * The application's allocator: a heap for the interface and the OpenGL runtime,
 * and the fallbacks the model runtimes need for their large buffers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <atomic>
#include <sys/mman.h>

extern "C"
{
    void *malloc(std::size_t size);
    void free(void *address);
    int posix_memalign(void **address, std::size_t alignment, std::size_t size);
    void __real_free(void *address);
    int __real_posix_memalign(void **address, std::size_t alignment, std::size_t size);
#ifdef PS5_WRAP_MALLOC
    void *__real_malloc(std::size_t size);
    void *__real_calloc(std::size_t count, std::size_t size);
    void *__real_realloc(void *address, std::size_t size);
    std::size_t __real_malloc_usable_size(const void *address);
#endif
    std::int64_t sceKernelGetDirectMemorySize(void);
    std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t,
                                               int, std::int64_t *);
    std::int32_t sceKernelMapDirectMemory(void **, std::size_t, int, int, std::int64_t,
                                          std::size_t);
    std::int32_t sceKernelMapNamedFlexibleMemory(void **, std::size_t, int, int, const char *);
    std::int32_t sceKernelMunmap(void *, std::size_t);
    std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
    void *scePthreadSelf(void);
    void *sceLibcMspaceCreate(const char *name, void *base, std::size_t size, unsigned flags);
    void *sceLibcMspaceMalloc(void *mspace, std::size_t size);
    void *sceLibcMspaceCalloc(void *mspace, std::size_t count, std::size_t size);
    void *sceLibcMspaceRealloc(void *mspace, void *address, std::size_t size);
    void sceLibcMspaceFree(void *mspace, void *address);
    int sceLibcMspacePosixMemalign(void *mspace, void **address, std::size_t alignment,
                                   std::size_t size);
    std::size_t sceLibcMspaceMallocUsableSize(const void *address);
}

namespace
{
constexpr std::size_t direct_alignment = 0x4000;
// SheetSage2 wants 1 GiB + 3 GiB weight stores plus graph pools inside this one
// arena; 4 GiB was sized for the YuE2 buffers alone. The kernel refuses the
// whole arena if the pool cannot hold it, and every caller then fails the same
// clean way it did before, so growing it is the safe direction.
constexpr std::size_t direct_arena_size = 6ULL * 1024 * 1024 * 1024;
constexpr std::size_t small_mapped_allocation_limit = 2ULL * 1024 * 1024;
constexpr std::size_t mapped_allocation_slots = 256;
constexpr std::uint64_t direct_allocation_magic = 0x505335414c4c4f43ULL;

struct alignas(std::max_align_t) DirectAllocation
{
    std::uint64_t magic;
    std::size_t previous_used;
    DirectAllocation *previous;
    std::size_t requested_size;
    bool released;
};

struct MappedAllocation
{
    void *mapping;
    void *address;
    std::size_t mapped_size;
    std::size_t requested_size;
};

// ponytail: the generator uses one model-loading/inference thread; add a lock
// only if concurrent allocation becomes measurable.
void *direct_arena{};
std::int64_t direct_arena_physical{-1};
std::size_t direct_arena_used{};
DirectAllocation *direct_tail{};
MappedAllocation mapped_allocations[mapped_allocation_slots]{};
std::atomic_flag direct_arena_lock = ATOMIC_FLAG_INIT;
std::atomic<bool> direct_fallback_enabled{false};

struct DirectArenaLock
{
    DirectArenaLock() noexcept
    {
        while (direct_arena_lock.test_and_set(std::memory_order_acquire))
            __asm__ volatile("pause");
    }
    ~DirectArenaLock()
    {
        direct_arena_lock.clear(std::memory_order_release);
    }
};

[[nodiscard]] void *allocate_direct(std::size_t size, std::size_t alignment) noexcept;

[[nodiscard]] void *allocate_mapped(std::size_t size, std::size_t alignment, bool cpu_only) noexcept
{
    if (size > direct_arena_size || alignment > direct_alignment ||
        size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        return nullptr;

    DirectArenaLock lock;
    MappedAllocation *slot = nullptr;
    for (auto &allocation : mapped_allocations)
    {
        if (allocation.mapping == nullptr)
        {
            slot = &allocation;
            break;
        }
    }
    if (!slot)
        return nullptr;

    const std::size_t required = size + alignment - 1;
    const std::size_t mapped_size = (required + direct_alignment - 1) & ~(direct_alignment - 1);
    void *mapping =
        cpu_only ? mmap(reinterpret_cast<void *>(0x600000000ULL), mapped_size, 0x03, 0x1002, -1, 0)
                 : nullptr;
    if (cpu_only)
    {
        if (mapping == MAP_FAILED || (reinterpret_cast<std::uintptr_t>(mapping) >> 32) == 2u)
        {
            if (mapping != MAP_FAILED)
                sceKernelMunmap(mapping, mapped_size);
            return nullptr;
        }
    }
    else if (sceKernelMapNamedFlexibleMemory(&mapping, mapped_size, 0x03, 0, "ProsperoAI small") !=
             0)
    {
        return nullptr;
    }
    const auto aligned = (reinterpret_cast<std::uintptr_t>(mapping) + alignment - 1) &
                         ~(static_cast<std::uintptr_t>(alignment) - 1);
    *slot = {mapping, reinterpret_cast<void *>(aligned), mapped_size, size};
    return slot->address;
}

// The thread that draws the interface. Its allocations never enter the direct
// arena: that arena is released in the order it was filled and belongs to the
// model being run, and a font or a picture kept by the interface would pin it.
std::atomic<void *> interface_thread{nullptr};

bool on_interface_thread() noexcept
{
    void *thread = interface_thread.load(std::memory_order_acquire);
    return thread != nullptr && thread == scePthreadSelf();
}

[[nodiscard]] void *allocate_fallback(std::size_t size, std::size_t alignment) noexcept
{
    const bool use_direct =
        direct_fallback_enabled.load(std::memory_order_acquire) && !on_interface_thread();
    if (!use_direct || size <= small_mapped_allocation_limit)
    {
        if (void *address = allocate_mapped(size, alignment, !use_direct))
            return address;
    }
    return use_direct ? allocate_direct(size, alignment) : nullptr;
}

bool release_mapped(void *address) noexcept
{
    DirectArenaLock lock;
    for (auto &allocation : mapped_allocations)
    {
        if (allocation.address != address)
            continue;
        sceKernelMunmap(allocation.mapping, allocation.mapped_size);
        allocation = {};
        return true;
    }
    return false;
}

[[nodiscard]] void *allocate_direct(std::size_t size, std::size_t alignment) noexcept
{
    DirectArenaLock lock;
    if (alignment < alignof(DirectAllocation))
        alignment = alignof(DirectAllocation);
    if (alignment > direct_alignment ||
        size > std::numeric_limits<std::size_t>::max() - sizeof(DirectAllocation) - (alignment - 1))
        return nullptr;

    if (direct_arena == nullptr)
    {
        const std::int64_t total = sceKernelGetDirectMemorySize();
        std::int64_t physical = -1;
        if (total <= 0 ||
            sceKernelAllocateDirectMemory(0, total, direct_arena_size, direct_alignment, 12,
                                          &physical) != 0 ||
            sceKernelMapDirectMemory(&direct_arena, direct_arena_size, 0x33, 0, physical,
                                     direct_alignment) != 0)
        {
            if (physical >= 0)
                sceKernelReleaseDirectMemory(physical, direct_arena_size);
            direct_arena = nullptr;
            return nullptr;
        }
        direct_arena_physical = physical;
    }

    const std::size_t previous_used = direct_arena_used;
    const std::size_t offset =
        (previous_used + sizeof(DirectAllocation) + alignment - 1) & ~(alignment - 1);
    if (offset > direct_arena_size || size > direct_arena_size - offset)
        return nullptr;
    auto *allocation = reinterpret_cast<DirectAllocation *>(
        static_cast<unsigned char *>(direct_arena) + offset - sizeof(DirectAllocation));
    *allocation = {direct_allocation_magic, previous_used, direct_tail, size, false};
    direct_tail = allocation;
    direct_arena_used = offset + size;
    return static_cast<unsigned char *>(direct_arena) + offset;
}

bool release_direct(void *address) noexcept
{
    DirectArenaLock lock;
    if (direct_arena == nullptr)
        return false;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(direct_arena);
    if (value < begin || value >= begin + direct_arena_size)
        return false;

    auto *allocation = reinterpret_cast<DirectAllocation *>(static_cast<unsigned char *>(address) -
                                                            sizeof(DirectAllocation));
    if (allocation->magic != direct_allocation_magic)
        return true;
    allocation->released = true;
    while (direct_tail != nullptr && direct_tail->released)
    {
        DirectAllocation *previous = direct_tail->previous;
        direct_arena_used = direct_tail->previous_used;
        direct_tail->magic = 0;
        direct_tail = previous;
    }
    return true;
}

[[nodiscard]] void *allocate(std::size_t size) noexcept
{
    size = size == 0 ? 1 : size;
    if (void *address = malloc(size))
        return address;
    return allocate_fallback(size, alignof(std::max_align_t));
}

bool owned_size(const void *address, std::size_t *size) noexcept
{
    DirectArenaLock lock;
    for (const auto &allocation : mapped_allocations)
        if (allocation.address == address && address)
        {
            *size = allocation.requested_size;
            return true;
        }
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(direct_arena);
    if (!direct_arena || value < begin + sizeof(DirectAllocation) ||
        value >= begin + direct_arena_size)
        return false;
    const auto *allocation = reinterpret_cast<const DirectAllocation *>(
        static_cast<const unsigned char *>(address) - sizeof(DirectAllocation));
    if (allocation->magic != direct_allocation_magic)
        return false;
    *size = allocation->requested_size;
    return true;
}

[[nodiscard]] void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept
{
    void *address = nullptr;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if ((alignment & (alignment - 1)) != 0)
        return nullptr;
    size = size == 0 ? 1 : size;
    if (posix_memalign(&address, alignment, size) == 0)
        return address;
    return allocate_fallback(size, alignment);
}

void release(void *address) noexcept
{
    if (address != nullptr && !release_mapped(address) && !release_direct(address))
        free(address);
}

[[noreturn]] void allocation_failure() noexcept
{
    __builtin_trap();
}

// ---- The interface heap ------------------------------------------------------
// The OpenGL runtime needs a heap of its own (ps5-opengl docs/consumer-build.md):
// Mesa's shader compiler alone makes thousands of small allocations, more than
// the title's libc heap serves, and the fallback above keeps one slot per
// allocation because it exists for a few large buffers. So the malloc family
// first asks one fixed region handed to the system's mspace allocator, as
// ps5-homebrew-ui's app_heap.c does. It is mapped once, away from the address
// range the inference scratch must have, and never unmapped. Requests larger
// than the limits below, and whatever the region cannot serve, take the paths
// this file had before, so the model runtimes keep the memory they had.
constexpr std::size_t interface_heap_size = 192ULL * 1024 * 1024;
constexpr std::size_t interface_heap_reduced = 128ULL * 1024 * 1024; // from flexible memory
// The interface's own thread may keep large blocks here (font atlases, pictures).
constexpr std::size_t interface_heap_largest = 64ULL * 1024 * 1024;
// Other threads keep to what was always a small, CPU-only allocation.
constexpr std::size_t interface_heap_largest_elsewhere = small_mapped_allocation_limit;
constexpr std::uintptr_t interface_heap_hint = 0x600000000ULL;
std::atomic<int> interface_heap_state{0}; // 0 not made, 1 being made, 2 ready, -1 unavailable
void *interface_heap_base{};
std::size_t interface_heap_bytes{};
void *interface_heap{};

bool outside_scratch_range(const void *address) noexcept
{
    return (reinterpret_cast<std::uintptr_t>(address) >> 32) != 2u;
}

bool interface_heap_ready() noexcept
{
    const int state = interface_heap_state.load(std::memory_order_acquire);
    if (state == 2)
        return true;
    if (state != 0)
        return false; // unavailable, or being made: that thread allocates the old way meanwhile
    int expected = 0;
    if (!interface_heap_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 2;

    // Direct memory mapped for the CPU only, as the OpenGL SDK's own heap does when
    // it is larger than flexible memory comfortably holds.
    void *base = reinterpret_cast<void *>(interface_heap_hint);
    std::size_t bytes = interface_heap_size;
    std::int64_t physical = -1;
    const std::size_t alignment = 2ULL * 1024 * 1024;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment, 12,
                                      &physical) != 0 ||
        sceKernelMapDirectMemory(&base, bytes, 0x03, 0, physical, alignment) != 0 ||
        base == nullptr || !outside_scratch_range(base))
    {
        if (physical >= 0)
        {
            if (base != nullptr && base != reinterpret_cast<void *>(interface_heap_hint))
                sceKernelMunmap(base, bytes);
            sceKernelReleaseDirectMemory(physical, bytes);
        }
        bytes = interface_heap_reduced;
        base = mmap(reinterpret_cast<void *>(interface_heap_hint), bytes, 0x03, 0x1002, -1, 0);
        if (base != MAP_FAILED && !outside_scratch_range(base))
        {
            sceKernelMunmap(base, bytes);
            base = MAP_FAILED;
        }
    }
    void *space =
        base == MAP_FAILED ? nullptr : sceLibcMspaceCreate("ProsperoAI interface", base, bytes, 0);
    if (space == nullptr)
    {
        interface_heap_state.store(-1, std::memory_order_release);
        return false;
    }
    interface_heap_base = base;
    interface_heap_bytes = bytes;
    interface_heap = space;
    interface_heap_state.store(2, std::memory_order_release);
    return true;
}

bool interface_heap_owns(const void *address) noexcept
{
    if (interface_heap_state.load(std::memory_order_acquire) != 2)
        return false;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(interface_heap_base);
    return value >= base && value - base < interface_heap_bytes;
}

bool interface_heap_takes(std::size_t size) noexcept
{
    return size <= (on_interface_thread() ? interface_heap_largest
                                          : interface_heap_largest_elsewhere) &&
           interface_heap_ready();
}
} // namespace

extern "C" __attribute__((visibility("hidden"))) void ps5SetInterfaceThread() noexcept
{
    interface_thread.store(scePthreadSelf(), std::memory_order_release);
}

extern "C" __attribute__((visibility("hidden"))) int
__wrap_posix_memalign(void **address, std::size_t alignment, std::size_t size)
{
    if (alignment <= direct_alignment && interface_heap_takes(size) &&
        sceLibcMspacePosixMemalign(interface_heap, address, alignment, size == 0 ? 1 : size) == 0)
        return 0;
    const int result = __real_posix_memalign(address, alignment, size);
    if (result != 12)
        return result;
    size = size == 0 ? 1 : size;
    *address = allocate_fallback(size, alignment);
    return *address == nullptr ? 12 : 0;
}

#ifdef PS5_WRAP_MALLOC
extern "C" __attribute__((visibility("hidden"))) void *__wrap_malloc(std::size_t size)
{
    size = size == 0 ? 1 : size;
    if (interface_heap_takes(size))
        if (void *address = sceLibcMspaceMalloc(interface_heap, size))
            return address;
    if (void *address = __real_malloc(size))
        return address;
    return allocate_fallback(size, alignof(std::max_align_t));
}

extern "C" __attribute__((visibility("hidden"))) void *__wrap_calloc(std::size_t count,
                                                                     std::size_t size)
{
    if ((count == 0 || size <= std::numeric_limits<std::size_t>::max() / count) &&
        interface_heap_takes(count * size))
        if (void *address =
                sceLibcMspaceCalloc(interface_heap, count == 0 ? 1 : count, size == 0 ? 1 : size))
            return address;
    if (void *address = __real_calloc(count, size))
        return address;
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)
        return nullptr;
    const std::size_t bytes = count * size;
    const std::size_t allocation_size = bytes == 0 ? 1 : bytes;
    void *address = allocate_fallback(allocation_size, alignof(std::max_align_t));
    if (address != nullptr)
        std::memset(address, 0, bytes == 0 ? 1 : bytes);
    return address;
}
extern "C" __attribute__((visibility("hidden"))) void *__wrap_realloc(void *address,
                                                                      std::size_t size)
{
    if (!address)
        return __wrap_malloc(size);
    if (interface_heap_owns(address))
    {
        if (!size)
        {
            sceLibcMspaceFree(interface_heap, address);
            return nullptr;
        }
        if (interface_heap_takes(size))
            if (void *resized = sceLibcMspaceRealloc(interface_heap, address, size))
                return resized;
        // It grew past what this heap takes (or holds): it moves to where such blocks go.
        void *moved = __wrap_malloc(size);
        if (!moved)
            return nullptr; // realloc failure retains the caller's original allocation.
        const std::size_t kept = sceLibcMspaceMallocUsableSize(address);
        std::memcpy(moved, address, kept < size ? kept : size);
        sceLibcMspaceFree(interface_heap, address);
        return moved;
    }
    std::size_t previous = 0;
    if (!owned_size(address, &previous))
        return __real_realloc(address, size);
    if (!size)
    {
        if (!release_mapped(address))
            release_direct(address);
        return nullptr;
    }
    void *next = __wrap_malloc(size);
    if (!next)
        return nullptr; // realloc failure retains the caller's original allocation.
    std::memcpy(next, address, previous < size ? previous : size);
    if (!release_mapped(address))
        release_direct(address);
    return next;
}

extern "C" __attribute__((visibility("hidden"))) std::size_t
__wrap_malloc_usable_size(const void *address)
{
    if (address && interface_heap_owns(address))
        return sceLibcMspaceMallocUsableSize(address);
    std::size_t size = 0;
    return !address || owned_size(address, &size) ? size : __real_malloc_usable_size(address);
}
#endif

extern "C" __attribute__((visibility("hidden"))) void __wrap_free(void *address)
{
    if (address != nullptr && interface_heap_owns(address))
    {
        sceLibcMspaceFree(interface_heap, address);
        return;
    }
    if (address != nullptr && (release_mapped(address) || release_direct(address)))
        return;
    __real_free(address);
}

extern "C" __attribute__((noinline, visibility("hidden"))) bool
ps5ObserveOwnedAllocation(const void *address) noexcept
{
    __asm__ volatile("" : : "r"(address) : "memory");
    return address != nullptr;
}

extern "C" __attribute__((visibility("hidden"))) void ps5SetDirectFallback(int enabled) noexcept
{
    direct_fallback_enabled.store(enabled != 0, std::memory_order_release);
}

extern "C" __attribute__((visibility("hidden"))) bool
ps5SdIsDirectArenaRange(const void *address, std::size_t size) noexcept
{
    if (direct_arena == nullptr)
        return false;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(direct_arena);
    return value >= begin && value <= begin + direct_arena_size &&
           size <= begin + direct_arena_size - value;
}

extern "C" __attribute__((visibility("hidden"))) bool ps5SdReleaseDirectArenaIfEmpty() noexcept
{
    DirectArenaLock lock;
    if (direct_arena == nullptr)
        return true;
    if (direct_arena_used != 0 || direct_tail != nullptr)
        return false;
    const int unmap_result = munmap(direct_arena, direct_arena_size);
    const int release_result =
        sceKernelReleaseDirectMemory(direct_arena_physical, direct_arena_size);
    direct_arena = nullptr;
    direct_arena_physical = -1;
    return unmap_result == 0 && release_result == 0;
}

void *operator new(std::size_t size)
{
    if (void *address = allocate(size))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    if (void *address = allocate_aligned(size, static_cast<std::size_t>(alignment)))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *address) noexcept
{
    release(address);
}
void operator delete[](void *address) noexcept
{
    release(address);
}
void operator delete(void *address, std::size_t) noexcept
{
    release(address);
}
void operator delete[](void *address, std::size_t) noexcept
{
    release(address);
}
void operator delete(void *address, std::align_val_t) noexcept
{
    release(address);
}
void operator delete[](void *address, std::align_val_t) noexcept
{
    release(address);
}
void operator delete(void *address, std::size_t, std::align_val_t) noexcept
{
    release(address);
}
void operator delete[](void *address, std::size_t, std::align_val_t) noexcept
{
    release(address);
}
void operator delete(void *address, const std::nothrow_t &) noexcept
{
    release(address);
}
void operator delete[](void *address, const std::nothrow_t &) noexcept
{
    release(address);
}
void operator delete(void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    release(address);
}
void operator delete[](void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    release(address);
}
