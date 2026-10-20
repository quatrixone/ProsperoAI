// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_upload.hpp"
#include "ggml-alloc.h"
#include "ggml-cpu.h"
#include "ggml-vulkan.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace prospero_streaming;

// Compare routed matmul against identical fully resident quantized weights.
// Only selected experts are read/uploaded, using two CPU cache slots across
// routing changes. Destination allocation is accounted separately.
void check(ggml_backend_t backend, ggml_type type, int rows, const char *directory) {
    constexpr int columns = 256, experts = 8, selected = 2;
    const auto row_bytes = ggml_row_size(type, columns);
    const auto expert_bytes = row_bytes * rows;
    std::vector<float> weights(columns * rows * experts);
    for (std::size_t i = 0; i < weights.size(); ++i)
        weights[i] = std::sin(float(i) * .013f) * .7f + std::cos(float(i) * .007f) * .2f;
    std::vector<std::uint8_t> packed(expert_bytes * experts);
    assert(ggml_quantize_chunk(type, weights.data(), packed.data(), 0, rows * experts, columns, nullptr)
           == packed.size());
    std::string path = std::string(directory) + "/expert-upload-fixture-XXXXXX";
    const int fd = mkstemp(path.data());
    assert(fd >= 0);
    unlink(path.c_str());
    std::array<std::uint8_t, 64> header{};
    assert(write(fd, header.data(), header.size()) == static_cast<ssize_t>(header.size()));
    assert(write(fd, packed.data(), packed.size()) == static_cast<ssize_t>(packed.size()));
    const ExpertExtent extent{header.size(), row_bytes, static_cast<std::uint64_t>(rows), experts};
    ExpertCache cache(fd, expert_bytes * 2, expert_bytes);
    close(fd);

    auto *ctx = ggml_init({ggml_tensor_overhead() * 16 + ggml_graph_overhead_custom(16, false), nullptr, true});
    assert(ctx);
    auto *streamed = ggml_new_tensor_3d(ctx, type, columns, rows, experts);
    auto *resident = ggml_new_tensor_3d(ctx, type, columns, rows, experts);
    auto *input = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, columns, 1, 1);
    auto *ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, selected, 1);
    auto *actual = ggml_mul_mat_id(ctx, streamed, input, ids);
    auto *expected = ggml_mul_mat_id(ctx, resident, input, ids);
    assert(ggml_backend_supports_op(backend, actual));
    auto *graph = ggml_new_graph_custom(ctx, 16, false);
    ggml_build_forward_expand(graph, actual);
    ggml_build_forward_expand(graph, expected);
    auto *buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
    assert(buffer);
    ggml_backend_buffer_clear(buffer, 0);
    ggml_backend_tensor_set(resident, packed.data(), 0, packed.size());
    std::array<float, columns> x{};
    for (int i = 0; i < columns; ++i) x[i] = std::sin(i * .17f);
    ggml_backend_tensor_set(input, x.data(), 0, sizeof(x));

    const std::array<std::array<std::int32_t, selected>, 5> routes{{{3, 0}, {3, 0}, {7, 1}, {1, 4}, {7, 3}}};
    std::vector<float> a(rows * selected), b(rows * selected);
    for (const auto &route : routes) {
        ggml_backend_tensor_set(ids, route.data(), 0, sizeof(route));
        for (auto id : route)
            upload_expert(cache, extent, id, backend, streamed, id * expert_bytes);
        assert(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS);
        ggml_backend_tensor_get(actual, a.data(), 0, a.size() * sizeof(float));
        ggml_backend_tensor_get(expected, b.data(), 0, b.size() * sizeof(float));
        for (std::size_t i = 0; i < a.size(); ++i) {
            assert(std::isfinite(a[i]) && std::isfinite(b[i]));
            assert(std::abs(a[i] - b[i]) <= 1e-5f * std::max(1.0f, std::abs(b[i])));
        }
    }
    auto stats = cache.stats();
    assert(stats.pinned_slots == 0 && stats.allocated_bytes <= 2 * expert_bytes);
    assert(stats.hits >= 3 && stats.evictions > 0);
    assert(stats.bytes_read == stats.misses * expert_bytes);
    bool rejected = false;
    try { upload_expert(cache, extent, 0, backend, streamed, packed.size()); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected && cache.stats().pinned_slots == 0);
    std::atomic<bool> cancel{true};
    rejected = false;
    try { upload_expert(cache, extent, 0, backend, streamed, 0, &cancel); }
    catch (const ReadCancelled &) { rejected = true; }
    assert(rejected && cache.stats().pinned_slots == 0);
    std::cout << ggml_backend_name(backend) << " " << ggml_type_name(type)
              << " rows=" << rows << ": routed parity passed; cache=" << stats.allocated_bytes
              << " bytes, graph buffers=" << ggml_backend_buffer_get_size(buffer) << " bytes\n";
    ggml_backend_buffer_free(buffer);
    ggml_free(ctx);
}

int main(int argc, char **argv) {
    assert(argc == 3); // backend and fixture directory
    ggml_backend_t backend = std::strcmp(argv[1], "vulkan") == 0
        ? ggml_backend_vk_init(0) : ggml_backend_cpu_init();
    assert(backend);
    for (auto type : {GGML_TYPE_Q4_K, GGML_TYPE_Q6_K, GGML_TYPE_Q4_0})
        for (int rows : {32, 64}) check(backend, type, rows, argv[2]);
    ggml_backend_free(backend);
    ggml_quantize_free();
}
