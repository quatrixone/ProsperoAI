// SPDX-License-Identifier: GPL-3.0-or-later
#include "expert_stream.hpp"
#include "ggml-backend-impl.h"
#include "ggml.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace prospero_streaming {
namespace {
// Not a mapping: an address that cannot be confused with real memory, so a
// dereference of a streamed tensor faults with a recognizable fault address.
std::uint8_t *const stream_base = reinterpret_cast<std::uint8_t *>(0x5452414d53000000ULL);
constexpr std::size_t stream_alignment = 64;
std::atomic<ExpertStream::State *> installed{nullptr};
}

// The Vulkan device reads GGML_OP_OFFLOAD_MIN_BATCH once, when the backend
// registry is built, and nothing can change it afterwards. Streaming needs the
// GPU to take routed experts even for a single-token batch, or the scheduler
// computes them on the CPU, where streamed weights are not resident. Setting it
// from a load path is too late if any model was loaded earlier in the session,
// so a build that links this file sets it before main runs. It applies to every
// model in that build, which is why only the streaming build links it.
const int offload_min_batch = [] {
    setenv("GGML_OP_OFFLOAD_MIN_BATCH", "1", 0);   // an explicit setting still wins
    return 0;
}();

struct ExpertStream::State {
    ExpertCache &cache;
    Resolver resolve;
    ggml_backend_buffer_type buffer_type{};
    std::unordered_map<const ggml_tensor *, std::size_t> pools;  // tensor -> cache pool
    std::unordered_map<const ggml_tensor *, std::uint64_t> expert_bytes;
    Stats stats;
    mutable std::mutex mutex;
    State(ExpertCache &cache, Resolver resolve) : cache(cache), resolve(std::move(resolve)) {}
};

namespace {
ExpertStream::State *state_of(ggml_backend_buffer_type_t buft) {
    return static_cast<ExpertStream::State *>(buft->context);
}

const char *buffer_type_name(ggml_backend_buffer_type_t) { return "Prospero-ExpertStream"; }
std::size_t buffer_type_alignment(ggml_backend_buffer_type_t) { return stream_alignment; }
bool buffer_type_is_host(ggml_backend_buffer_type_t) { return true; }

void buffer_free(ggml_backend_buffer_t) {}
void *buffer_get_base(ggml_backend_buffer_t) { return stream_base; }
void buffer_clear(ggml_backend_buffer_t, std::uint8_t) {}

// A streamed tensor's bytes are never written or read through the buffer: the
// loader must skip it and the scheduler must go through the expert hook.
void buffer_set_tensor(ggml_backend_buffer_t, ggml_tensor *tensor, const void *, std::size_t,
                       std::size_t) {
    GGML_ABORT("%s: a streamed expert tensor cannot be written through its buffer", tensor->name);
}
void buffer_get_tensor(ggml_backend_buffer_t, const ggml_tensor *tensor, void *, std::size_t,
                       std::size_t) {
    GGML_ABORT("%s: a streamed expert tensor cannot be read through its buffer", tensor->name);
}
void buffer_memset_tensor(ggml_backend_buffer_t, ggml_tensor *tensor, std::uint8_t, std::size_t,
                          std::size_t) {
    GGML_ABORT("%s: a streamed expert tensor cannot be cleared", tensor->name);
}

ggml_status buffer_init_tensor(ggml_backend_buffer_t buffer, ggml_tensor *tensor) {
    auto *state = state_of(buffer->buft);
    StreamedTensor streamed;
    if (!state->resolve(tensor->name, streamed)) return GGML_STATUS_FAILED;
    const auto bytes = streamed.extent.row_bytes * streamed.extent.rows_per_expert;
    // The scheduler indexes experts by nb[2]; the file layout must agree with it.
    if (tensor->ne[2] != static_cast<std::int64_t>(streamed.extent.experts) ||
        tensor->nb[2] != bytes)
        return GGML_STATUS_FAILED;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->pools[tensor] = state->cache.add_extent(streamed.extent, streamed.slots);
    state->expert_bytes[tensor] = bytes;
    return GGML_STATUS_SUCCESS;
}

ggml_backend_buffer_t buffer_type_alloc(ggml_backend_buffer_type_t buft, std::size_t size) {
    static const ggml_backend_buffer_i iface = {
        /* .free_buffer   = */ buffer_free,
        /* .get_base      = */ buffer_get_base,
        /* .init_tensor   = */ buffer_init_tensor,
        /* .memset_tensor = */ buffer_memset_tensor,
        /* .set_tensor    = */ buffer_set_tensor,
        /* .get_tensor    = */ buffer_get_tensor,
        /* .set_tensor_2d = */ nullptr,
        /* .get_tensor_2d = */ nullptr,
        /* .cpy_tensor    = */ nullptr,
        /* .clear         = */ buffer_clear,
        /* .reset         = */ nullptr,
    };
    return ggml_backend_buffer_init(buft, iface, state_of(buft), size);
}

bool copy_experts(ggml_backend_t backend, ggml_tensor *destination, const ggml_tensor *weights,
                  std::int32_t first_id, std::int32_t last_id, std::size_t expert_size,
                  void *user_data) {
    auto *state = static_cast<ExpertStream::State *>(user_data);
    std::size_t pool = 0;
    std::uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->pools.find(weights);
        if (found == state->pools.end()) {
            ++state->stats.rejected;
            return false;  // not ours: the scheduler reads resident weights itself
        }
        pool = found->second;
        bytes = state->expert_bytes[weights];
        ++state->stats.copies;
    }
    if (bytes != expert_size)
        throw std::runtime_error("Streamed expert stride does not match the scheduler's");

    // Hold every lease until the asynchronous copies have been submitted and the
    // backend has consumed them, so a refill cannot overwrite bytes in flight.
    std::vector<ExpertCache::Lease> leases;
    leases.reserve(static_cast<std::size_t>(last_id - first_id + 1));
    for (std::int32_t id = first_id; id <= last_id; ++id) {
        leases.push_back(state->cache.acquire(pool, static_cast<std::uint64_t>(id)));
        ggml_backend_tensor_set_async(backend, destination, leases.back().data(),
                                      static_cast<std::size_t>(id) * expert_size, expert_size);
    }
    ggml_backend_synchronize(backend);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stats.experts += leases.size();
        state->stats.bytes += leases.size() * expert_size;
    }
    return true;
}
} // namespace

bool device_runs_routed_experts(ggml_backend_dev_t device, int type, std::int64_t columns,
                                 std::int64_t rows, std::int64_t experts,
                                 std::int64_t experts_used, std::int64_t tokens,
                                 std::string *reason) {
    if (!device) {
        if (reason) *reason = "no device";
        return false;
    }
    auto *ctx = ggml_init({ggml_tensor_overhead() * 8, nullptr, true});
    if (!ctx) {
        if (reason) *reason = "no context for the probe graph";
        return false;
    }
    auto *weights = ggml_new_tensor_3d(ctx, static_cast<ggml_type>(type), columns, rows, experts);
    auto *activations = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, columns, experts_used, tokens);
    auto *ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, experts_used, tokens);
    auto *node = ggml_mul_mat_id(ctx, weights, activations, ids);
    const bool supported = ggml_backend_dev_supports_op(device, node);
    const bool offloads = ggml_backend_dev_offload_op(device, node);
    if (reason && !(supported && offloads))
        *reason = supported ? "the backend declines to take this batch size"
                            : "the backend cannot run MUL_MAT_ID for this expert type and shape";
    ggml_free(ctx);
    return supported && offloads;
}

ExpertStream::ExpertStream(ExpertCache &cache, Resolver resolve)
    : state_(std::make_unique<State>(cache, std::move(resolve))) {
    state_->buffer_type.iface = {
        /* .get_name         = */ buffer_type_name,
        /* .alloc_buffer     = */ buffer_type_alloc,
        /* .alloc_buffer_n   = */ nullptr,
        /* .get_alignment    = */ buffer_type_alignment,
        /* .get_max_size     = */ nullptr,
        /* .get_alloc_size   = */ nullptr,
        /* .get_alloc_size_n = */ nullptr,
        /* .is_host          = */ buffer_type_is_host,
    };
    // The scheduler resolves a weight buffer to a backend through its device.
    // These weights are host-side and offloaded, exactly like CPU-resident ones.
    state_->buffer_type.device = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!state_->buffer_type.device) throw std::runtime_error("No CPU device for streamed weights");
    state_->buffer_type.context = state_.get();
}

ExpertStream::~ExpertStream() {
    if (installed.load() == state_.get()) uninstall();
}

ggml_backend_buffer_type_t ExpertStream::buffer_type() { return &state_->buffer_type; }

void ExpertStream::install() {
    installed.store(state_.get());
    ggml_backend_set_expert_copy(copy_experts, state_.get());
    // Also tell the loader not to write these tensors: without this it reads the
    // whole tensor into address space that was never mapped.
    ggml_backend_set_streamed_buft(&state_->buffer_type);
}

void ExpertStream::uninstall() {
    installed.store(nullptr);
    ggml_backend_set_expert_copy(nullptr, nullptr);
    ggml_backend_set_streamed_buft(nullptr);
}

ExpertStream::Stats ExpertStream::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}
} // namespace prospero_streaming
