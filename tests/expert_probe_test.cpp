// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a synthetic MoE GGUF, then checks what the probe reports about it.
#include "expert_probe.hpp"
#include "ggml.h"
#include "gguf.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace prospero_streaming;

namespace {
constexpr int columns = 256, rows = 64, experts = 8, layers = 4, used = 2;

std::string write_fixture(const char *directory) {
    const std::string path = std::string(directory) + "/probe-fixture.gguf";
    auto *ctx = ggml_init({ggml_tensor_overhead() * (layers * 4 + 4) + 64u * 1024 * 1024, nullptr, false});
    assert(ctx);
    auto *gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "gemma4");
    gguf_set_val_u32(gguf, "gemma4.expert_count", experts);
    gguf_set_val_u32(gguf, "gemma4.expert_used_count", used);
    // A rank-2 tensor and a non-expert rank-3 tensor must not be mistaken for experts.
    auto *resident = ggml_new_tensor_2d(ctx, GGML_TYPE_Q4_K, columns, rows);
    ggml_set_name(resident, "token_embd.weight");
    gguf_add_tensor(gguf, resident);
    auto *attention = ggml_new_tensor_2d(ctx, GGML_TYPE_Q4_K, columns, rows);
    ggml_set_name(attention, "blk.0.attn_q.weight");
    gguf_add_tensor(gguf, attention);
    for (int layer = 0; layer < layers; ++layer) {
        // Gemma 4 keeps one scale per expert in a rank-1 tensor beside the matrices.
        auto *scale = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, experts);
        ggml_set_name(scale, (std::string("blk.") + std::to_string(layer) + ".ffn_down_exps.scale").c_str());
        std::memset(scale->data, layer + 1, ggml_nbytes(scale));
        gguf_add_tensor(gguf, scale);
        for (const char *suffix : {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}) {
            // Q6_K down projections make one tensor wider than the others.
            const auto type = std::strcmp(suffix, "ffn_down_exps") == 0 ? GGML_TYPE_Q6_K : GGML_TYPE_Q4_K;
            auto *tensor = ggml_new_tensor_3d(ctx, type, columns, rows, experts);
            ggml_set_name(tensor, (std::string("blk.") + std::to_string(layer) + "." + suffix + ".weight").c_str());
            std::memset(tensor->data, layer * 16 + suffix[4], ggml_nbytes(tensor));
            gguf_add_tensor(gguf, tensor);
        }
    }
    assert(gguf_write_to_file(gguf, path.c_str(), false));
    gguf_free(gguf);
    ggml_free(ctx);
    return path;
}
} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    const auto path = write_fixture(argv[1]);
    const auto gate_bytes = static_cast<std::uint64_t>(ggml_row_size(GGML_TYPE_Q4_K, columns)) * rows;
    const auto down_bytes = static_cast<std::uint64_t>(ggml_row_size(GGML_TYPE_Q6_K, columns)) * rows;

    ExpertProbeRequest request;
    request.path = path;
    request.slots = 4;
    request.tokens = 3;
    auto result = probe_expert_stream(request);
    assert(result.ok && result.error.empty());
    assert(result.architecture == "gemma4");
    assert(result.experts == experts && result.experts_used == used);   // taken from metadata
    assert(result.routed_layers == layers && result.routed_tensors == layers * 4); // matrices + scale
    assert(result.replayed_layers == layers && result.tokens == 3);
    assert(result.max_expert_bytes == down_bytes);                      // the widest tensor sizes a slot
    constexpr std::uint64_t scale_bytes = sizeof(float);                   // one scale per expert
    assert(result.per_token_bytes == (2 * gate_bytes + down_bytes + scale_bytes) * used * layers);
    assert(result.slots == 4 && result.allocated_bytes >= 4 * down_bytes);
    assert(result.reads == static_cast<std::uint64_t>(layers) * 4 * used * 3);
    assert(result.hits + result.misses == result.reads);
    assert(result.bytes_read >= result.misses * scale_bytes &&
           result.bytes_read <= result.misses * down_bytes); // one extent per miss
    assert(result.cold_bytes > 0 && result.cold_bytes <= result.per_token_bytes);
    assert(result.bytes_read == result.cold_bytes + result.warm_bytes);
    assert(result.file_bytes > result.per_token_bytes);

    // Routing is deterministic for a seed, and a different seed may differ.
    auto repeated = probe_expert_stream(request);
    assert(repeated.ok && repeated.hits == result.hits && repeated.bytes_read == result.bytes_read);

    // One slot per layer tensor set forces eviction; a wide budget removes it.
    request.slots = 1;
    auto narrow = probe_expert_stream(request);
    assert(narrow.ok && narrow.evictions > 0 && narrow.slots == 1);
    request.slots = 64;
    request.layers = 1;
    auto wide = probe_expert_stream(request);
    assert(wide.ok && wide.replayed_layers == 1 && wide.evictions == 0);
    assert(wide.per_token_bytes == (2 * gate_bytes + down_bytes + scale_bytes) * used);

    // Rejections and failures are reported, never thrown.
    request = ExpertProbeRequest{};
    request.path = path;
    request.slots = 0;
    assert(!probe_expert_stream(request).ok);
    request.slots = 8;
    request.experts_per_token = experts + 1;
    assert(!probe_expert_stream(request).ok);
    request.experts_per_token = 0;
    request.path = std::string(argv[1]) + "/missing.gguf";
    const auto missing = probe_expert_stream(request);
    assert(!missing.ok && !missing.error.empty());

    std::remove(path.c_str());
    std::cout << "expert probe: layout, determinism, budget, eviction and failures passed; "
              << "per-token=" << result.per_token_bytes << " bytes, read=" << result.bytes_read
              << " bytes, hits=" << result.hits << "\n";
}
