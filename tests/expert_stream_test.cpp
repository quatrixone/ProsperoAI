// SPDX-License-Identifier: GPL-3.0-or-later
// Runs MUL_MAT_ID through the scheduler with the expert weights not resident:
// the weights live in the streaming buffer type and their bytes arrive from the
// cache through the expert-copy hook. Compared against resident weights.
#include "expert_stream.hpp"
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-cpu.h"
#include "ggml-vulkan.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace prospero_streaming;

namespace
{
constexpr int columns = 256, rows = 64, experts = 16, selected = 4;

std::string write_weights(const char *directory, ggml_type type, std::vector<std::uint8_t> &packed,
                          std::uint64_t header)
{
    std::vector<float> values(std::size_t(columns) * rows * experts);
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = std::sin(float(i) * .011f) * .6f + std::cos(float(i) * .003f) * .3f;
    packed.resize(std::size_t(ggml_row_size(type, columns)) * rows * experts);
    assert(ggml_quantize_chunk(type, values.data(), packed.data(), 0, rows * experts, columns,
                               nullptr) == packed.size());
    std::string path = std::string(directory) + "/expert-stream-fixture-XXXXXX";
    const int fd = mkstemp(path.data());
    assert(fd >= 0);
    std::vector<std::uint8_t> padding(header, 0);
    assert(write(fd, padding.data(), padding.size()) == static_cast<ssize_t>(padding.size()));
    assert(write(fd, packed.data(), packed.size()) == static_cast<ssize_t>(packed.size()));
    close(fd);
    return path;
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    constexpr std::uint64_t header = 128;
    const auto type = GGML_TYPE_Q4_K;
    std::vector<std::uint8_t> packed;
    const auto path = write_weights(argv[1], type, packed, header);
    const auto expert_bytes = packed.size() / experts;

    // The Vulkan device offloads MUL_MAT_ID only from batch 32 by default, so a
    // single-token decode would run routed experts on the CPU backend and read
    // weights that are not resident. Streaming requires offload at batch 1.
    setenv("GGML_OP_OFFLOAD_MIN_BATCH", "1", 1);
    auto *vulkan = ggml_backend_vk_init(0);
    auto *cpu = ggml_backend_cpu_init();
    assert(vulkan && cpu);

    // Four of sixteen experts stay resident, so most tokens must fetch.
    ExpertCache cache(path.c_str(), 4 * expert_bytes + 64, 2);
    const ExpertExtent extent{header, static_cast<std::uint64_t>(ggml_row_size(type, columns)),
                              rows, experts};
    unsigned resolved = 0;
    ExpertStream stream(
        cache,
        [&](const std::string &name, StreamedTensor &out)
        {
            if (name != "streamed_experts")
                return false;
            out.extent = extent;
            out.slots = 4;
            ++resolved;
            return true;
        },
        argc > 2);
    stream.install();

    // Room for both graphs: one graph's worth of context silently aliased them.
    // Weights live for the whole run; the graph is rebuilt for every pass, as
    // llama.cpp does. Reusing one prebuilt graph does not work: the scheduler
    // rewrites node sources to its own copies, so a second pass sees no inputs
    // to fill and computes from stale memory.
    auto *weights_ctx = ggml_init({ggml_tensor_overhead() * 4, nullptr, true});
    assert(weights_ctx);
    auto *streamed = ggml_new_tensor_3d(weights_ctx, type, columns, rows, experts);
    ggml_set_name(streamed, "streamed_experts");
    auto *resident = ggml_new_tensor_3d(weights_ctx, type, columns, rows, experts);
    ggml_set_name(resident, "resident_experts");

    auto *streamed_buffer =
        ggml_backend_buft_alloc_buffer(stream.buffer_type(), ggml_nbytes(streamed) + 64);
    assert(streamed_buffer);
    ggml_backend_buffer_set_usage(streamed_buffer, GGML_BACKEND_BUFFER_USAGE_WEIGHTS);
    auto allocator = ggml_tallocr_new(streamed_buffer);
    assert(ggml_tallocr_alloc(&allocator, streamed) == GGML_STATUS_SUCCESS);
    assert(resolved == 1);

    auto *resident_buffer = ggml_backend_alloc_buffer(cpu, ggml_nbytes(resident) + 1024);
    assert(resident_buffer);
    ggml_backend_buffer_set_usage(resident_buffer, GGML_BACKEND_BUFFER_USAGE_WEIGHTS);
    auto resident_allocator = ggml_tallocr_new(resident_buffer);
    assert(ggml_tallocr_alloc(&resident_allocator, resident) == GGML_STATUS_SUCCESS);
    ggml_backend_tensor_set(resident, packed.data(), 0, packed.size());

    ggml_backend_t backends[] = {vulkan, cpu};
    ggml_backend_buffer_type_t bufts[] = {ggml_backend_get_default_buffer_type(vulkan),
                                          ggml_backend_get_default_buffer_type(cpu)};
    auto *sched = ggml_backend_sched_new(backends, bufts, 2, 64, false, true);
    assert(sched);

    std::array<float, columns> x{};
    for (int i = 0; i < columns; ++i)
        x[i] = std::sin(i * .21f);
    std::vector<float> a(std::size_t(rows) * selected), b(a.size());
    const std::array<std::array<std::int32_t, selected>, 6> routes{{{0, 1, 2, 3},
                                                                    {0, 1, 2, 3},
                                                                    {12, 13, 14, 15},
                                                                    {3, 7, 11, 15},
                                                                    {1, 2, 3, 4},
                                                                    {15, 0, 8, 4}}};

    // One MUL_MAT_ID per graph: the scheduler replaces the expert copy only when
    // the streamed weights are src[0] of the split's first node, which is the
    // shape of llama.cpp's MoE graphs.
    const auto run = [&](ggml_tensor *weights, std::vector<float> &out,
                         const std::array<std::int32_t, selected> &route)
    {
        auto *ctx = ggml_init(
            {ggml_tensor_overhead() * 8 + ggml_graph_overhead_custom(8, false), nullptr, true});
        assert(ctx);
        auto *input = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, columns, 1, 1);
        ggml_set_name(input, "input");
        ggml_set_input(input);
        auto *ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, selected, 1);
        ggml_set_name(ids, "ids");
        ggml_set_input(ids);
        auto *result = ggml_mul_mat_id(ctx, weights, input, ids);
        ggml_set_output(result);
        auto *graph = ggml_new_graph_custom(ctx, 8, false);
        ggml_build_forward_expand(graph, result);
        ggml_backend_sched_reset(sched);
        assert(ggml_backend_sched_alloc_graph(sched, graph));
        ggml_backend_tensor_set(input, x.data(), 0, sizeof(x));
        ggml_backend_tensor_set(ids, route.data(), 0, sizeof(route));
        try
        {
            assert(ggml_backend_sched_graph_compute(sched, graph) == GGML_STATUS_SUCCESS);
        }
        catch (...)
        {
            ggml_free(ctx);
            throw;
        }
        ggml_backend_tensor_get(result, out.data(), 0, out.size() * sizeof(float));
        ggml_free(ctx);
    };

    for (const auto &route : routes)
    {
        stream.begin_graph(1); // one shape throughout: residency may persist
        run(streamed, a, route);
        run(resident, b, route);
        // Comparing two silent zeros is not parity: require real output first.
        double route_energy = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            route_energy += std::abs(b[i]);
        assert(route_energy > 1e-3 * double(a.size()));
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            assert(std::isfinite(a[i]) && std::isfinite(b[i]));
            assert(std::abs(a[i] - b[i]) <= 1e-4f * std::max(1.0f, std::abs(b[i])));
        }
    }
    const auto served = stream.stats();
    const auto cached = cache.stats();
    assert(served.copies > 0 && served.experts > 0 &&
           served.bytes == served.experts * expert_bytes);
    // The second route repeats the first, so those experts must not be uploaded
    // again, and parity above proves the skipped ones were really still there.
    assert(argc > 2 ? served.skipped >= selected : served.skipped == 0);
    assert(served.skipped_bytes == served.skipped * expert_bytes);

    // A different graph key means the scheduler may have moved its copies: every
    // expert must be uploaded again, even the ones just sent.
    const auto before_invalidation = stream.stats();
    stream.begin_graph(2);
    run(streamed, a, routes.front());
    run(resident, b, routes.front());
    stream.drain();
    const auto after_invalidation = stream.stats();
    assert(cache.stats().pinned_slots == 0);
    assert(after_invalidation.experts >= before_invalidation.experts + selected);
    assert(after_invalidation.skipped == before_invalidation.skipped);
    double energy = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        energy += std::abs(b[i]);
    assert(energy > 1e-3 * double(a.size()));
    for (std::size_t i = 0; i < a.size(); ++i)
        assert(std::abs(a[i] - b[i]) <= 1e-4f * std::max(1.0f, std::abs(b[i])));
    // The cache may record no hits at all now: a repeated expert is skipped
    // before it is asked for, which is the point of the GPU-side record.
    assert(cached.misses > 0 && cached.hits + cached.misses <= served.experts + served.skipped);
    assert(cached.allocated_bytes <= 4 * expert_bytes + 64);
    stream.drain(); // in-flight copies hold their leases
    assert(cache.stats().pinned_slots == 0);

    const auto before_uninstall = stream.stats();

    // With the hook uninstalled the scheduler would read the fake address, so a
    // run without it must not be attempted; check the counter instead.
    ExpertStream::uninstall();
    assert(stream.stats().copies == before_uninstall.copies);

    std::printf("expert stream: routed parity through the scheduler; %llu experts uploaded, "
                "%llu skipped as already resident on the GPU, %llu copies, cache %zu bytes, "
                "%llu hits / %llu misses, events=%s, stalls=%llu\n",
                (unsigned long long)after_invalidation.experts,
                (unsigned long long)after_invalidation.skipped,
                (unsigned long long)after_invalidation.copies, cached.allocated_bytes,
                (unsigned long long)cached.hits, (unsigned long long)cached.misses,
                after_invalidation.events ? "yes" : "no",
                (unsigned long long)after_invalidation.stalls);
    ggml_backend_sched_free(sched);
    ggml_backend_buffer_free(streamed_buffer);
    ggml_backend_buffer_free(resident_buffer);
    ggml_free(weights_ctx);
    ggml_backend_free(vulkan);
    ggml_backend_free(cpu);
    std::remove(path.c_str());
    ggml_quantize_free();
}
