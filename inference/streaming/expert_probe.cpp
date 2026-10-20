// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_probe.hpp"
#include "expert_cache.hpp"
#include "expert_layout.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace prospero_streaming {
namespace {

// Deterministic routing stand-in. Real routing depends on the prompt; a fixed
// sequence keeps a measurement comparable between builds and runs, and a
// recorded trace replaces it when one is given.
struct Routing {
    std::uint64_t state;
    explicit Routing(std::uint64_t seed)
        : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
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

    const auto layout = read_routed_expert_layout(request.path);
    result.architecture = layout.architecture;
    result.experts = layout.experts;
    result.experts_used = layout.experts_used;
    result.routed_layers = layout.layers;
    result.routed_tensors = static_cast<unsigned>(layout.tensors.size());
    result.max_expert_bytes = layout.max_expert_bytes;
    result.file_bytes = layout.file_bytes;

    // Group the tensors by layer: a replay fetches every tensor of the layer the
    // router picked an expert in.
    struct RoutedLayer {
        int index = 0;
        std::vector<RoutedExpertTensor> tensors;
        std::vector<std::size_t> pools;
    };
    std::vector<RoutedLayer> layers;
    for (const auto &tensor : layout.tensors) {
        if (layers.empty() || layers.back().index != tensor.layer)
            layers.push_back(RoutedLayer{tensor.layer, {}, {}});
        layers.back().tensors.push_back(tensor);
    }
    unsigned used = request.experts_per_token ? request.experts_per_token : result.experts_used;
    if (!used) used = 1;
    if (used > result.experts) throw std::invalid_argument("experts_per_token exceeds the file's expert count");
    result.experts_used = used;
    if (request.layers && request.layers < layers.size()) layers.resize(request.layers);
    result.replayed_layers = static_cast<unsigned>(layers.size());
    for (const auto &layer : layers)
        for (const auto &tensor : layer.tensors) result.per_token_bytes += tensor.bytes_per_expert * used;


    // `slots` is resident experts per tensor, as TurboFieldfare's per-layer cache
    // is: every routed tensor of a layer keeps that many experts, each slot
    // exactly one expert wide. The cache opens and owns the file, because PS5
    // rejects a dup() of a descriptor.
    std::uint64_t budget = 0;
    for (const auto &layer : layers)
        for (const auto &tensor : layer.tensors)
            budget += ((tensor.bytes_per_expert + 63) & ~std::uint64_t(63))
                      * std::min<std::uint64_t>(request.slots, tensor.extent.experts);
    ExpertCache cache(request.path.c_str(), static_cast<std::size_t>(budget), request.read_workers);
    for (auto &layer : layers) {
        layer.pools.clear();
        for (const auto &tensor : layer.tensors)
            layer.pools.push_back(cache.add_extent(tensor.extent, request.slots));
    }

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
                    for (const auto pool : layer.pools) {
                        auto lease = cache.acquire(pool, expert);
                        ++result.reads;
                    }
            }
        } else {
            for (const auto &row : trace[step]) {
                const auto found = std::find_if(layers.begin(), layers.end(),
                    [&](const RoutedLayer &candidate) { return candidate.index == row.first; });
                if (found == layers.end()) continue; // a dense layer, or outside a narrowed replay
                for (const auto expert : row.second)
                    for (const auto pool : found->pools) {
                        auto lease = cache.acquire(pool, expert);
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
