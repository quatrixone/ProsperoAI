// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_layout.hpp"
#include "ggml.h"
#include "gguf.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace prospero_streaming {
namespace {
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (!a || !b || a > UINT64_MAX / b) throw std::invalid_argument("Overflowing expert extent");
    return a * b;
}

// GGUF keeps a MoE expert matrix as one rank-3 tensor with the experts last,
// for example blk.0.ffn_gate_exps.weight. Rank-4 names are not this.
bool routed_expert_name(const char *name, int &layer) {
    if (std::strncmp(name, "blk.", 4) != 0 || !std::strstr(name, "_exps")) return false;
    char *end = nullptr;
    const long parsed = std::strtol(name + 4, &end, 10);
    if (end == name + 4 || !end || *end != '.' || parsed < 0) return false;
    layer = static_cast<int>(parsed);
    return true;
}

struct GgufHandle {
    gguf_context *ctx = nullptr;
    ~GgufHandle() { if (ctx) gguf_free(ctx); }
};

std::uint64_t metadata_u64(const gguf_context *ctx, const std::string &key) {
    const auto id = gguf_find_key(ctx, key.c_str());
    if (id < 0) return 0;
    switch (gguf_get_kv_type(ctx, id)) {
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(ctx, id);
        case GGUF_TYPE_INT32: {
            const auto value = gguf_get_val_i32(ctx, id);
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
        case GGUF_TYPE_UINT64: return gguf_get_val_u64(ctx, id);
        default: return 0;
    }
}
} // namespace

std::uint64_t RoutedExpertLayout::per_token_bytes(unsigned experts_per_token) const {
    std::uint64_t total = 0;
    for (const auto &tensor : tensors) total += tensor.bytes_per_expert * experts_per_token;
    return total;
}

std::uint64_t RoutedExpertLayout::cache_bytes(unsigned slots) const {
    std::uint64_t total = 0;
    for (const auto &tensor : tensors)
        total += ((tensor.bytes_per_expert + 63) & ~std::uint64_t(63))
                 * std::min<std::uint64_t>(slots, tensor.extent.experts);
    return total;
}

RoutedExpertLayout read_routed_expert_layout(const std::string &path) {
    RoutedExpertLayout layout;
    GgufHandle gguf;
    gguf.ctx = gguf_init_from_file(path.c_str(), gguf_init_params{true, nullptr});
    if (!gguf.ctx) throw std::runtime_error("Cannot read GGUF metadata: " + path);
    const auto architecture_id = gguf_find_key(gguf.ctx, "general.architecture");
    if (architecture_id >= 0 && gguf_get_kv_type(gguf.ctx, architecture_id) == GGUF_TYPE_STRING)
        layout.architecture = gguf_get_val_str(gguf.ctx, architecture_id);
    layout.experts = static_cast<unsigned>(metadata_u64(gguf.ctx, layout.architecture + ".expert_count"));
    layout.experts_used = static_cast<unsigned>(metadata_u64(gguf.ctx, layout.architecture + ".expert_used_count"));

    const auto data_offset = static_cast<std::uint64_t>(gguf_get_data_offset(gguf.ctx));
    for (std::int64_t i = 0, n = gguf_get_n_tensors(gguf.ctx); i < n; ++i) {
        int layer = 0;
        const char *name = gguf_get_tensor_name(gguf.ctx, i);
        if (!routed_expert_name(name, layer)) continue;
        const auto *ne = gguf_get_tensor_ne(gguf.ctx, i);
        if (ne[3] != 1) continue;
        const auto type = gguf_get_tensor_type(gguf.ctx, i);
        const auto size = static_cast<std::uint64_t>(gguf_get_tensor_size(gguf.ctx, i));
        std::uint64_t row_bytes = 0, rows_per_expert = 0, experts = 0;
        bool expert_major = false;
        if (ne[2] >= 2) {
            // An expert-major matrix: rows of one expert are contiguous.
            experts = static_cast<std::uint64_t>(ne[2]);
            rows_per_expert = static_cast<std::uint64_t>(ne[1]);
            row_bytes = static_cast<std::uint64_t>(ggml_row_size(type, ne[0]));
            expert_major = true;
        } else if (ne[1] == 1 && layout.experts && ne[0] == static_cast<std::int64_t>(layout.experts)
                   && size % layout.experts == 0) {
            // A per-expert scalar vector, as Gemma 4's ffn_*_exps.scale tensors are.
            experts = layout.experts;
            rows_per_expert = 1;
            row_bytes = size / layout.experts;
        } else {
            continue;
        }
        const auto bytes_per_expert = multiply(row_bytes, rows_per_expert);
        if (multiply(bytes_per_expert, experts) != size)
            throw std::runtime_error(std::string("Uneven expert extent in tensor ") + name);
        layout.tensors.push_back(RoutedExpertTensor{
            name, layer, static_cast<int>(type),
            ExpertExtent{data_offset + gguf_get_tensor_offset(gguf.ctx, i), row_bytes,
                         rows_per_expert, experts},
            bytes_per_expert, expert_major});
        layout.max_expert_bytes = std::max(layout.max_expert_bytes, bytes_per_expert);
    }
    if (layout.tensors.empty()) throw std::runtime_error("No routed expert tensors in this GGUF");
    std::stable_sort(layout.tensors.begin(), layout.tensors.end(),
                     [](const RoutedExpertTensor &a, const RoutedExpertTensor &b) {
                         return a.layer < b.layer;
                     });
    if (!layout.experts) layout.experts = static_cast<unsigned>(layout.tensors.front().extent.experts);
    int previous = -1;
    for (const auto &tensor : layout.tensors)
        if (tensor.layer != previous) {
            ++layout.layers;
            previous = tensor.layer;
        }
    struct stat info{};
    if (stat(path.c_str(), &info) == 0) layout.file_bytes = static_cast<std::uint64_t>(info.st_size);
    return layout;
}
} // namespace prospero_streaming
