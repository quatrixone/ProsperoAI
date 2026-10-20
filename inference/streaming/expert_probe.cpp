// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_probe.hpp"
#include "expert_cache.hpp"
#include "ggml.h"
#include "gguf.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace prospero_streaming {
namespace {

struct RoutedTensor {
    ExpertExtent extent;
    std::uint64_t bytes_per_expert;
    std::size_t pool = 0;   // handle inside the cache
};

struct RoutedLayer {
    int index = 0;
    std::vector<RoutedTensor> tensors;
};

// GGUF keeps a MoE expert matrix as one rank-3 tensor with the experts last,
// for example blk.0.ffn_gate_exps.weight. Rank-2 and rank-4 names are not this.
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

// Deterministic routing stand-in. Real Gemma 4 routing depends on the prompt;
// a fixed sequence keeps a measurement comparable between builds and runs.
struct Routing {
    std::uint64_t state;
    explicit Routing(std::uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 17;
    }
    void select(unsigned experts, unsigned used, std::vector<std::uint64_t> &out) {
        out.clear();
        while (out.size() < used) {
            const auto candidate = next() % experts;
            if (std::find(out.begin(), out.end(), candidate) == out.end()) out.push_back(candidate);
        }
    }
};

// One replay step: the experts the router picked, grouped by layer.
using TraceStep = std::vector<std::pair<int, std::vector<std::uint64_t>>>;

std::vector<TraceStep> read_trace(const std::string &path, unsigned experts) {
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file) throw std::runtime_error("Cannot read the routing trace: " + path);
    struct Closer { std::FILE *file; ~Closer() { std::fclose(file); } } closer{file};
    std::vector<TraceStep> steps;
    char line[4096];
    long previous_step = -1;
    while (std::fgets(line, sizeof(line), file)) {
        const char *cursor = line;
        int consumed = 0;
        long step = 0, layer = 0;
        if (std::sscanf(cursor, "%ld %ld%n", &step, &layer, &consumed) != 2)
            throw std::runtime_error("Malformed routing trace line: " + path);
        if (step < 0 || layer < 0) throw std::runtime_error("Negative step or layer in the trace");
        cursor += consumed;
        std::vector<std::uint64_t> ids;
        long id = 0;
        while (std::sscanf(cursor, "%ld%n", &id, &consumed) == 1) {
            if (id < 0 || static_cast<unsigned long>(id) >= experts)
                throw std::runtime_error("Expert id in the trace is outside the file's expert count");
            ids.push_back(static_cast<std::uint64_t>(id));
            cursor += consumed;
        }
        if (ids.empty()) continue;
        if (step != previous_step) {
            steps.emplace_back();
            previous_step = step;
        }
        steps.back().emplace_back(static_cast<int>(layer), std::move(ids));
    }
    if (steps.empty()) throw std::runtime_error("The routing trace holds no steps");
    return steps;
}

ExpertProbeResult run(const ExpertProbeRequest &request) {
    ExpertProbeResult result;
    if (request.slots < 1 || request.slots > 4096) throw std::invalid_argument("slots must be 1..4096");
    if (request.trace_path.empty() && (request.tokens < 1 || request.tokens > 64))
        throw std::invalid_argument("tokens must be 1..64");

    GgufHandle gguf;
    gguf.ctx = gguf_init_from_file(request.path.c_str(), gguf_init_params{true, nullptr});
    if (!gguf.ctx) throw std::runtime_error("Cannot read GGUF metadata: " + request.path);
    const auto architecture_id = gguf_find_key(gguf.ctx, "general.architecture");
    if (architecture_id >= 0 && gguf_get_kv_type(gguf.ctx, architecture_id) == GGUF_TYPE_STRING)
        result.architecture = gguf_get_val_str(gguf.ctx, architecture_id);
    result.experts = static_cast<unsigned>(metadata_u64(gguf.ctx, result.architecture + ".expert_count"));
    result.experts_used = static_cast<unsigned>(metadata_u64(gguf.ctx, result.architecture + ".expert_used_count"));

    const auto data_offset = static_cast<std::uint64_t>(gguf_get_data_offset(gguf.ctx));
    std::vector<RoutedLayer> layers;
    for (std::int64_t i = 0, n = gguf_get_n_tensors(gguf.ctx); i < n; ++i) {
        int layer = 0;
        const char *name = gguf_get_tensor_name(gguf.ctx, i);
        if (!routed_expert_name(name, layer)) continue;
        const auto *ne = gguf_get_tensor_ne(gguf.ctx, i);
        if (ne[3] != 1) continue;
        const auto type = gguf_get_tensor_type(gguf.ctx, i);
        const auto size = static_cast<std::uint64_t>(gguf_get_tensor_size(gguf.ctx, i));
        std::uint64_t row_bytes = 0, rows_per_expert = 0, tensor_experts = 0;
        if (ne[2] >= 2) {
            // An expert-major matrix: rows of one expert are contiguous.
            tensor_experts = static_cast<std::uint64_t>(ne[2]);
            rows_per_expert = static_cast<std::uint64_t>(ne[1]);
            row_bytes = static_cast<std::uint64_t>(ggml_row_size(type, ne[0]));
        } else if (ne[1] == 1 && result.experts && ne[0] == static_cast<std::int64_t>(result.experts)
                   && size % result.experts == 0) {
            // A per-expert scalar vector, as Gemma 4's ffn_*_exps.scale tensors
            // are. One expert is a few bytes, which the reader must still fetch.
            tensor_experts = result.experts;
            rows_per_expert = 1;
            row_bytes = size / result.experts;
        } else {
            continue; // not per-expert data
        }
        const auto bytes_per_expert = row_bytes * rows_per_expert;
        if (bytes_per_expert * tensor_experts != size)
            throw std::runtime_error(std::string("Uneven expert extent in tensor ") + name);
        RoutedTensor tensor{ExpertExtent{data_offset + gguf_get_tensor_offset(gguf.ctx, i), row_bytes,
                                         rows_per_expert, tensor_experts}, bytes_per_expert};
        auto found = std::find_if(layers.begin(), layers.end(),
                                  [&](const RoutedLayer &candidate) { return candidate.index == layer; });
        if (found == layers.end()) {
            layers.push_back(RoutedLayer{layer, {}});
            found = layers.end() - 1;
        }
        if (found->tensors.size() && found->tensors.front().extent.experts != tensor_experts)
            throw std::runtime_error("Inconsistent expert count inside one layer");
        found->tensors.push_back(tensor);
        result.max_expert_bytes = std::max(result.max_expert_bytes, bytes_per_expert);
        ++result.routed_tensors;
    }
    if (layers.empty()) throw std::runtime_error("No routed expert tensors in this GGUF");
    std::sort(layers.begin(), layers.end(),
              [](const RoutedLayer &a, const RoutedLayer &b) { return a.index < b.index; });
    result.routed_layers = static_cast<unsigned>(layers.size());
    const auto file_experts = layers.front().tensors.front().extent.experts;
    if (!result.experts) result.experts = static_cast<unsigned>(file_experts);
    unsigned used = request.experts_per_token ? request.experts_per_token : result.experts_used;
    if (!used) used = 1;
    if (used > result.experts) throw std::invalid_argument("experts_per_token exceeds the file's expert count");
    result.experts_used = used;
    if (request.layers && request.layers < layers.size()) layers.resize(request.layers);
    result.replayed_layers = static_cast<unsigned>(layers.size());
    for (const auto &layer : layers)
        for (const auto &tensor : layer.tensors) result.per_token_bytes += tensor.bytes_per_expert * used;

    struct stat info{};
    if (stat(request.path.c_str(), &info) == 0) result.file_bytes = static_cast<std::uint64_t>(info.st_size);

    // `slots` is resident experts per tensor, as TurboFieldfare's per-layer cache
    // is: every routed tensor of a layer keeps that many experts, each slot
    // exactly one expert wide. The cache opens and owns the file, because PS5
    // rejects a dup() of a descriptor.
    std::size_t budget = 0;
    for (const auto &layer : layers)
        for (const auto &tensor : layer.tensors)
            budget += ((static_cast<std::size_t>(tensor.bytes_per_expert) + 63) & ~std::size_t(63))
                      * std::min<std::uint64_t>(request.slots, tensor.extent.experts);
    ExpertCache cache(request.path.c_str(), budget, request.read_workers);
    for (auto &layer : layers)
        for (auto &tensor : layer.tensors)
            tensor.pool = cache.add_extent(tensor.extent, request.slots);
    // A trace replays the router's own choices; otherwise the stand-in routing
    // draws uniformly, which understates the hit rate of real, skewed routing.
    std::vector<TraceStep> trace;
    if (!request.trace_path.empty()) {
        trace = read_trace(request.trace_path, result.experts);
        result.replayed_trace = true;
    }
    Routing routing(request.seed);
    std::vector<std::uint64_t> selected;
    const std::size_t total_steps = trace.empty() ? request.tokens : trace.size();
    for (std::size_t step = 0; step < total_steps; ++step) {
        const auto before_bytes = cache.stats().bytes_read;
        const auto start = std::chrono::steady_clock::now();
        if (trace.empty()) {
            for (auto &layer : layers) {
                routing.select(result.experts, used, selected);
                for (const auto expert : selected)
                    for (const auto &tensor : layer.tensors) {
                        auto lease = cache.acquire(tensor.pool, expert);
                        ++result.reads;
                    }
            }
        } else {
            for (const auto &row : trace[step]) {
                const auto found = std::find_if(layers.begin(), layers.end(),
                    [&](const RoutedLayer &candidate) { return candidate.index == row.first; });
                if (found == layers.end()) continue; // a dense layer, or outside a narrowed replay
                for (const auto expert : row.second)
                    for (const auto &tensor : found->tensors) {
                        auto lease = cache.acquire(tensor.pool, expert);
                        ++result.reads;
                    }
            }
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const auto read_bytes = cache.stats().bytes_read - before_bytes;
        result.slowest_step_ms = std::max(result.slowest_step_ms, elapsed);
        if (step == 0) {
            result.cold_ms = elapsed;
            result.cold_bytes = read_bytes;
        } else {
            result.warm_ms += elapsed;
            result.warm_bytes += read_bytes;
        }
        ++result.tokens;
    }
    const auto stats = cache.stats();
    result.slots = stats.slots;
    result.pools = stats.pools;
    result.read_workers = stats.read_workers;
    result.allocated_bytes = stats.allocated_bytes;
    result.hits = stats.hits;
    result.misses = stats.misses;
    result.evictions = stats.evictions;
    result.bytes_read = stats.bytes_read;
    result.ok = true;
    return result;
}

} // namespace

ExpertProbeResult probe_expert_stream(const ExpertProbeRequest &request) {
    try {
        return run(request);
    } catch (const std::exception &failure) {
        ExpertProbeResult result;
        result.error = failure.what();
        return result;
    } catch (...) {
        ExpertProbeResult result;
        result.error = "Unknown expert probe failure";
        return result;
    }
}

} // namespace prospero_streaming
