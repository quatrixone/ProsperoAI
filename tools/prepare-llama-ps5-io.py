#!/usr/bin/env python3
"""Apply guarded, repeatable PS5-only hooks to a llama.cpp source tree."""
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[1]


def replace(path, before, after):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise SystemExit(f"{path}: PS5 I/O hook no longer matches pinned llama.cpp")
    path.write_text(text.replace(before, after))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / '.deps/llama.cpp')
parser.add_argument('--ggml-source', type=Path, default=root / 'build/ggml-upstream')
args = parser.parse_args()
source_root = args.source
ggml_source = args.ggml_source
source = source_root / 'src'
replace(source / "llama-mmap.cpp", '#include "llama-mmap.h"',
        '#include "llama-mmap.h"\n#ifdef PROSPERO_PS5_MODEL_IO\n#include "ps5-model-io.hpp"\n#endif')
replace(source / "llama-mmap.cpp", '            std::size_t ret = std::fread(ptr, to_read, 1, fp);',
        '''#ifdef PROSPERO_PS5_MODEL_IO
            if (to_read == len && prospero_ps5_read_parallel(fp, ptr, to_read)) return;
#endif
            std::size_t ret = std::fread(ptr, to_read, 1, fp);''')
loader = source / 'llama-model-loader.cpp'
direct_size = ('LLAMA_DIRECT_IO_BUFFER_SIZE' if 'LLAMA_DIRECT_IO_BUFFER_SIZE' in loader.read_text()
               else '64 * 1024 * 1024')
replace(loader,
        f'const size_t buffer_size = alignment != 1 ? {direct_size} + 2 * alignment : 1 * 1024 * 1024;',
        f'''const size_t buffer_size = alignment != 1 ? {direct_size} + 2 * alignment :
#ifdef PROSPERO_PS5_MODEL_IO
        (std::getenv("PROSPERO_MODEL_IO_SERIAL") ? 1 : 8) * 1024 * 1024; // 32 MiB ring in parallel mode
#else
        1 * 1024 * 1024;
#endif''')

# Fragmentation hunt (20.10): log every >=64 MiB device allocation so a YuE2
# run names its biggest contiguous demand; the gate threshold can then match
# the real requirement instead of a guess.
buffers = ggml_source / 'src/ggml-vulkan/ggml-vulkan-buffers.cpp'
replace(buffers,
        '    vk::MemoryRequirements mem_req = device->device.getBufferMemoryRequirements(buf->buffer);',
        '''    vk::MemoryRequirements mem_req = device->device.getBufferMemoryRequirements(buf->buffer);
    if (mem_req.size >= (64ull << 20))
        GGML_LOG_INFO("ps5 vkalloc: %llu MiB (import=%d)\\n",
                      (unsigned long long)(mem_req.size >> 20), int(import_ptr != nullptr));''')

# prospero-ps5-small-alloc-slab: sub-allocate small ggml-vulkan device buffers
# inside ggml-owned slabs so the PS5 direct-memory pool stops fragmenting.
# Anchors validated against ggml commit d7cb574130e (21.10).
types_h = ggml_source / 'src/ggml-vulkan/ggml-vulkan-types.h'

SLAB_CODE = '''// ---- prospero-ps5-small-alloc-slab ------------------------------------------
// The PS5 direct-memory pool is fragmented by a stream of small
// vkAllocateMemory calls: every freed segment lingers as a kernel-level
// island, so after a single model load/unload cycle the largest contiguous
// block drops from 11.4 GiB to ~5.7 GiB and starves the contiguous needs of
// the audio adapters (measured 21.10). Sub-allocating small buffers inside
// ggml-owned slabs keeps the driver facing a few large, stable segments;
// large allocations keep their dedicated memory as before.
#include <algorithm>
#include <map>

struct vk_slab;

struct vk_slab_pool {
    std::mutex mutex;
    std::vector<vk_slab *> slabs;
    std::size_t total = 0;
    static constexpr std::size_t slab_size = 256ull << 20;
    static constexpr std::size_t max_alloc = 0;  // disabled: slabs pinned 256 MiB host maps and starved the app heap
    static constexpr std::size_t max_total = 3ull << 30;
};

struct vk_slab {
    vk::DeviceMemory memory;
    void * mapped = nullptr;   // whole-slab mapping, host-visible types only
    std::size_t size = 0;
    std::uint32_t memory_type = 0;
    vk::MemoryPropertyFlags props {};
    vk_slab_pool * pool = nullptr;
    std::vector<vk_slab_region> regions;   // free ranges, sorted by offset, coalesced
};

static std::mutex g_slab_pools_mutex;
static std::map<VkDevice, vk_slab_pool *> g_slab_pools;

static vk_slab_pool * vk_slab_pool_for(VkDevice device) {
    std::lock_guard<std::mutex> guard(g_slab_pools_mutex);
    vk_slab_pool * & pool = g_slab_pools[device];
    if (pool == nullptr)
        pool = new vk_slab_pool();
    return pool;
}

static std::size_t vk_slab_align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static void vk_slab_sort_regions(vk_slab * slab) {
    std::sort(slab->regions.begin(), slab->regions.end(),
              [](const vk_slab_region & a, const vk_slab_region & b) { return a.offset < b.offset; });
}

// Carve a region out of a slab of the given memory type, creating a new slab
// when none can serve it. Returns false when the pool declines (size, type or
// capacity) and the caller falls back to a dedicated allocation.
static bool vk_slab_acquire(vk_slab_pool * pool, vk::Device device, std::uint32_t memory_type,
                            vk::MemoryPropertyFlags props, std::size_t alloc_size,
                            std::size_t alignment, const vk::MemoryAllocateFlagsInfo & flags_info,
                            vk_slab * & slab_out, std::size_t & offset_out) {
    std::lock_guard<std::mutex> guard(pool->mutex);
    if (alloc_size == 0 || alloc_size > vk_slab_pool::max_alloc)
        return false;
    alignment = vk_slab_align_up(alignment < 64 ? 64 : alignment, 64);
    for (vk_slab * slab : pool->slabs) {
        if (slab->memory_type != memory_type)
            continue;
        for (std::size_t i = 0; i < slab->regions.size(); ++i) {
            const vk_slab_region region = slab->regions[i];
            const std::size_t base = vk_slab_align_up(region.offset, alignment);
            if (base + alloc_size > region.offset + region.size)
                continue;
            if (base == region.offset && base + alloc_size == region.offset + region.size)
                slab->regions.erase(slab->regions.begin() + i);
            else if (base == region.offset)
                slab->regions[i] = { base + alloc_size, region.size - alloc_size };
            else {
                slab->regions[i] = { region.offset, base - region.offset };
                if (base + alloc_size < region.offset + region.size)
                    slab->regions.push_back({ base + alloc_size, region.offset + region.size - base - alloc_size });
            }
            vk_slab_sort_regions(slab);
            slab_out = slab;
            offset_out = base;
            return true;
        }
    }
    if (pool->total + vk_slab_pool::slab_size > vk_slab_pool::max_total)
        return false;
    try {
        vk_slab * slab = new vk_slab();
        slab->memory = device.allocateMemory({ vk_slab_pool::slab_size, memory_type, &flags_info });
        slab->size = vk_slab_pool::slab_size;
        slab->memory_type = memory_type;
        slab->props = props;
        slab->pool = pool;
        if (props & vk::MemoryPropertyFlagBits::eHostVisible)
            slab->mapped = device.mapMemory(slab->memory, 0, VK_WHOLE_SIZE);
        pool->slabs.push_back(slab);
        pool->total += slab->size;
        GGML_LOG_INFO("ps5 slab: new %llu MiB slab (memory type %u)\\n",
                      (unsigned long long)(slab->size >> 20), (unsigned) memory_type);
        const std::size_t consumed = vk_slab_align_up(alloc_size, 64);
        if (consumed < slab->size)
            slab->regions.push_back({ consumed, slab->size - consumed });
        slab_out = slab;
        offset_out = 0;
        return true;
    } catch (const vk::SystemError & e) {
        GGML_LOG_WARN("ps5 slab: slab creation failed (%s), using dedicated allocation\\n", e.what());
        return false;
    }
}

void vk_slab_release(void * owner, std::size_t offset, std::size_t size) {
    if (owner == nullptr)
        return;
    vk_slab * slab = static_cast<vk_slab *>(owner);
    std::lock_guard<std::mutex> guard(slab->pool->mutex);
    slab->regions.push_back({ offset, size });
    vk_slab_sort_regions(slab);
    std::vector<vk_slab_region> merged;
    for (const vk_slab_region & region : slab->regions) {
        if (!merged.empty() && merged.back().offset + merged.back().size == region.offset)
            merged.back().size += region.size;
        else
            merged.push_back(region);
    }
    slab->regions.swap(merged);
}

static bool ps5_slab_try_alloc(vk_device & device, vk_buffer & buf, std::size_t alloc_size,
                               std::size_t alignment, std::uint32_t memory_type,
                               vk::MemoryPropertyFlags props,
                               const vk::MemoryAllocateFlagsInfo & flags_info) {
    if (buf->slab_owner != nullptr || alloc_size > vk_slab_pool::max_alloc)
        return false;
    // Non-coherent host-visible memory needs per-buffer flush ranges that a
    // slab cannot express honestly; the PS5 types are coherent, but keep the
    // guard for future drivers.
    if ((props & vk::MemoryPropertyFlagBits::eHostVisible) &&
        !(props & vk::MemoryPropertyFlagBits::eHostCoherent))
        return false;
    vk_slab * slab = nullptr;
    std::size_t offset = 0;
    if (!vk_slab_acquire(vk_slab_pool_for(device->device), device->device, memory_type, props,
                         alloc_size, alignment, flags_info, slab, offset))
        return false;
    buf->device_memory = slab->memory;
    buf->slab_owner = slab;
    buf->slab_offset = offset;
    return true;
}

static vk_buffer ggml_vk_create_buffer(vk_device& device, size_t size, const std::initializer_list<vk::MemoryPropertyFlags> & req_flags_list,'''

# A1: insert the slab engine right before ggml_vk_create_buffer.
replace(buffers,
 'static vk_buffer ggml_vk_create_buffer(vk_device& device, size_t size, const std::initializer_list<vk::MemoryPropertyFlags> & req_flags_list,',
 SLAB_CODE)

# A2: route the per-buffer allocateMemory through the slab first.
replace(buffers,
 '                    buf->device_memory = device->device.allocateMemory({ mem_req.size, *mtype_it, &mem_flags_info });',
 '''                    buf->memory_property_flags = mem_props.memoryTypes[*mtype_it].propertyFlags;
                    if (!ps5_slab_try_alloc(device, buf, mem_req.size, mem_req.alignment,
                                            *mtype_it, buf->memory_property_flags, mem_flags_info))
                        buf->device_memory = device->device.allocateMemory({ mem_req.size, *mtype_it, &mem_flags_info });''')

# A3: host-visible mapping comes from the slab when sub-allocated.
replace(buffers,
 '''        if (buf->memory_property_flags & vk::MemoryPropertyFlagBits::eHostVisible) {
            buf->ptr = device->device.mapMemory(buf->device_memory, 0, VK_WHOLE_SIZE);
        }''',
 '''        if (buf->memory_property_flags & vk::MemoryPropertyFlagBits::eHostVisible) {
            buf->ptr = buf->slab_owner
                ? static_cast<char *>(static_cast<vk_slab *>(buf->slab_owner)->mapped) + buf->slab_offset
                : device->device.mapMemory(buf->device_memory, 0, VK_WHOLE_SIZE);
        }''')

# A4: bind at the slab offset (0 for dedicated memory, so this is universal).
replace(buffers,
 '    device->device.bindBufferMemory(buf->buffer, buf->device_memory, 0);',
 '    device->device.bindBufferMemory(buf->buffer, buf->device_memory, buf->slab_offset);')

# A5: host_malloc failure path must release the slab region, not the memory.
replace(buffers,
 '''        device->device.freeMemory(buf->device_memory);
        device->device.destroyBuffer(buf->buffer);''',
 '''        if (buf->slab_owner) {
            vk_slab_release(buf->slab_owner, buf->slab_offset, buf->size);
            buf->slab_owner = nullptr;
            buf->device_memory = VK_NULL_HANDLE;
        } else {
            device->device.freeMemory(buf->device_memory);
        }
        device->device.destroyBuffer(buf->buffer);
        buf->buffer = VK_NULL_HANDLE;''')

# B1: release hook visible to the buffer destructor.
replace(types_h,
 'struct vk_buffer_struct;',
 '''struct vk_slab_region { std::size_t offset; std::size_t size; };
void vk_slab_release(void * slab, std::size_t offset, std::size_t size);  // prospero-ps5-small-alloc-slab

struct vk_buffer_struct;''')

# B2: buffer fields that record the slab carve.
replace(types_h,
 '    vk::DeviceAddress bda_addr {};',
 '''    vk::DeviceAddress bda_addr {};
    // prospero-ps5-small-alloc-slab: non-null when device_memory is a shared slab
    void * slab_owner = nullptr;
    std::size_t slab_offset = 0;''')

# B3: the destructor returns slab carves instead of freeing shared memory.
replace(types_h,
 '''        device->buffer_destroy_count.fetch_add(1, std::memory_order_release);
        device->device.freeMemory(device_memory);
        device->device.destroyBuffer(buffer);''',
 '''        device->buffer_destroy_count.fetch_add(1, std::memory_order_release);
        if (slab_owner != nullptr) {
            vk_slab_release(slab_owner, slab_offset, size);
            slab_owner = nullptr;
        } else {
            device->device.freeMemory(device_memory);
        }
        device->device.destroyBuffer(buffer);''')


# PS5 titles do not expose libc's stderr in klog. Route each timestamp result
# through ggml's registered log callback so profiling remains observable.
vulkan = ggml_source / 'src/ggml-vulkan/ggml-vulkan.cpp'
text = vulkan.read_text()
marker = 'GGML_LOG_INFO("Vulkan Timings:\\n")'
if marker not in text:
    # Current upstream moved this method outside vk_perf_logger's class.
    # Keep the adapter compatible with both that layout and the older Prism pin.
    signature = 'void vk_perf_logger::print_timings(bool force) {'
    if signature in text:
        begin = text.index(signature)
        end = text.index('\nstd::string vk_perf_logger::get_node_fusion_name', begin)
    else:
        signature = '    void print_timings(bool force = false) {'
        if signature not in text:
            raise SystemExit(f"{vulkan}: unsupported Vulkan timing logger layout")
        begin = text.index(signature)
        end = text.index('\n    std::string get_node_fusion_name', begin)
    body = '''SIGNATURE
        if (timings.empty()) return;
        print_count++;
        if ((print_count % vk_perf_logger_frequency) != 0 && !force) return;
        print_count = 0;
        uint64_t total_all_op_times = 0;
        GGML_LOG_INFO("Vulkan Timings:\\n");
        for (const auto & t : timings) {
            uint64_t total_op_times = 0;
            for (const auto & time : t.second) total_op_times += time;
            std::ostringstream line;
            line << t.first << ": " << t.second.size() << " x "
                 << (total_op_times / t.second.size() / 1000.0) << " us = "
                 << (total_op_times / 1000.0) << " us";
            auto it = flops.find(t.first);
            if (it != flops.end() && it->second.size() == t.second.size()) {
                uint64_t total_op_flops = 0;
                for (const auto & elem : it->second) total_op_flops += elem;
                line << " (" << (double(total_op_flops) / 1.0e9) /
                    (double(total_op_times) / 1.0e9) << " GFLOPS/s)";
            }
            total_all_op_times += total_op_times;
            GGML_LOG_INFO("%s\\n", line.str().c_str());
        }
        GGML_LOG_INFO("Total time: %.3f us.\\n", total_all_op_times / 1000.0);
        timings.clear();
        flops.clear();
    }
'''.replace('SIGNATURE', signature)
    vulkan.write_text(text[:begin] + body + text[end:])

# Oberon's RDNA2 UMA benefits from computing four adjacent standard-quant
# output rows per subgroup. A console A/B on Mistral 7B Q4_0 measured
# 65.51 versus 62.93 decode tokens/s (+4.1%). Keep this PS5-only rather than
# changing the generic upstream AMD heuristic.
replace(vulkan, '''        if (device->architecture == AMD_GCN) {
            rm_stdq = 2;
            rm_kq = 4;
            rm_stdq_int = 4;
        }
''', '''        if (device->architecture == AMD_GCN) {
            rm_stdq = 2;
            rm_kq = 4;
            rm_stdq_int = 4;
        } else if (device->architecture == AMD_RDNA2 && device->uma) {
            rm_stdq = 2;
        }
''')

# Oberon's RDNA2 UMA also prefers the larger hybrid-reduction mat-vec workgroup for the
# narrower projections, which upstream enables only for NVIDIA and Intel. A console A/B on
# Mistral 7B Q4_0 measured 66.49 versus 65.43 decode tokens/s (+1.6%) with the limit at
# m <= 8192; letting the 14336-row FFN rows use it as well regressed to 62.13 tokens/s.
if '/* prospero-ps5-vulkan-profile */' not in text:
    replace(vulkan, """    if (b_type == GGML_TYPE_Q8_1) {
        if (ctx->device->vendor_id == VK_VENDOR_ID_INTEL) {
            dmmv_wg = DMMV_WG_SIZE_SUBGROUP;
        }
        return ctx->device->pipeline_dequant_mul_mat_vec_q8_1_f32[dmmv_wg][a_type][num_cols-1];""",
        """    if (ctx->device->vendor_id == VK_VENDOR_ID_AMD &&
        ctx->device->architecture == vk_device_architecture::AMD_RDNA2 && ctx->device->uma &&
        a_type != GGML_TYPE_Q6_K && m <= 8192 && k >= 1024) {
        dmmv_wg = DMMV_WG_SIZE_LARGE;
    }

    if (b_type == GGML_TYPE_Q8_1) {
        if (ctx->device->vendor_id == VK_VENDOR_ID_INTEL) {
            dmmv_wg = DMMV_WG_SIZE_SUBGROUP;
        }
        return ctx->device->pipeline_dequant_mul_mat_vec_q8_1_f32[dmmv_wg][a_type][num_cols-1];""")
