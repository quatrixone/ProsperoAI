// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a MoE GGUF with its routed experts streamed from the file instead of
// resident: the expert tensors get address space from the streaming buffer type
// and their bytes arrive per token through the bounded cache. Writes the
// generated token ids so a resident run can be compared against it.
#include "expert_cache.hpp"
#include "expert_layout.hpp"
#include "expert_stream.hpp"
#include "ggml.h"
#include "llama.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

using namespace prospero_streaming;

namespace {
// A streamed tensor's address space is reserved, never mapped, so touching it
// faults on a recognizable address. Report which address and where.
void report_fault(int signal, siginfo_t *info, void *) {
    void *frames[32];
    const int count = backtrace(frames, 32);
    std::fprintf(stderr, "\nFAULT signal %d at %p (streamed address space starts at 0x5452414d53000000)\n",
                 signal, info ? info->si_addr : nullptr);
    backtrace_symbols_fd(frames, count, 2);
    _exit(139);
}
} // namespace

int main(int argc, char **argv) {
    struct sigaction action{};
    action.sa_sigaction = report_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: %s MODEL.gguf OUT.tokens STEPS SLOTS [READ_WORKERS] [PROMPT]\n"
            "SLOTS is resident experts per routed tensor.\n", argv[0]);
        return 2;
    }
    const char *model_path = argv[1], *out_path = argv[2];
    const int steps = std::atoi(argv[3]);
    const unsigned slots = unsigned(std::atoi(argv[4]));
    const unsigned workers = argc > 5 ? unsigned(std::atoi(argv[5])) : 2;
    const char *prompt = argc > 6 ? argv[6] : "Explain why the sky is blue, in two sentences.";
    if (steps < 1 || !slots) return 2;

    // Routed experts must reach the GPU through the scheduler's expert copy even
    // for a single token, or they would be computed on the CPU from weights that
    // are not resident.
    setenv("GGML_OP_OFFLOAD_MIN_BATCH", "1", 1);
    llama_backend_init();

    const auto layout = read_routed_expert_layout(model_path);
    std::uint64_t budget = 0, streamed_tensors = 0;
    for (const auto &tensor : layout.tensors)
        if (tensor.expert_major) {
            budget += ((tensor.bytes_per_expert + 63) & ~std::uint64_t(63))
                      * std::min<std::uint64_t>(slots, tensor.extent.experts);
            ++streamed_tensors;
        }
    std::fprintf(stderr, "%s: %u routed layers, %zu per-expert tensors, %u experts (%u used), "
                 "streaming %llu of them, cache %.2f GiB for %u experts per tensor\n",
                 layout.architecture.c_str(), layout.layers, layout.tensors.size(),
                 layout.experts, layout.experts_used, (unsigned long long) streamed_tensors,
                 double(budget) / (1u << 30), slots);

    ExpertCache cache(model_path, std::size_t(budget), workers);
    std::size_t resolved = 0;
    ExpertStream stream(cache, [&](const std::string &name, StreamedTensor &out) {
        for (const auto &tensor : layout.tensors)
            if (tensor.expert_major && tensor.name == name) {
                out.extent = tensor.extent;
                out.slots = slots;
                ++resolved;
                return true;
            }
        return false;
    });
    stream.install();

    ggml_backend_dev_t devices[] = {ggml_backend_dev_by_name("Vulkan0"), nullptr};
    if (!devices[0]) {
        std::fprintf(stderr, "no Vulkan0 device\n");
        return 1;
    }
    // Only the expert-major matrices: the per-expert scalar vectors beside them
    // are a few bytes per expert and stay resident.
    // Refuse streaming on a device that will not run MUL_MAT_ID itself: the
    // experts would be computed on the CPU from weights that are not there.
    std::string reason;
    bool can_stream = true;
    for (const auto &tensor : layout.tensors)
        if (can_stream && tensor.expert_major)
            can_stream = device_runs_routed_experts(devices[0], tensor.type,
                                                     std::int64_t(tensor.extent.row_bytes /
                                                         ggml_type_size(ggml_type(tensor.type)) *
                                                         ggml_blck_size(ggml_type(tensor.type))),
                                                     std::int64_t(tensor.extent.rows_per_expert),
                                                     std::int64_t(tensor.extent.experts),
                                                     std::max(1u, layout.experts_used), 1, &reason);
    if (!can_stream) {
        std::fprintf(stderr, "cannot stream routed experts on this device: %s\n", reason.c_str());
        return 2;
    }

    const llama_model_tensor_buft_override overrides[] = {
        {"_exps\\.weight$", stream.buffer_type()}, {nullptr, nullptr}};
    auto mp = llama_model_default_params();
    mp.devices = devices;
    mp.n_gpu_layers = 99;
    mp.load_mode = LLAMA_LOAD_MODE_NONE;     // mmap would bypass the buffer type
    mp.tensor_buft_overrides = overrides;
    auto *model = llama_model_load_from_file(model_path, mp);
    if (!model) {
        std::fprintf(stderr, "cannot load %s\n", model_path);
        return 1;
    }
    std::fprintf(stderr, "streamed tensors resolved: %zu of %zu\n", resolved, layout.tensors.size());

    auto cp = llama_context_default_params();
    cp.n_ctx = 512;
    cp.n_batch = cp.n_ubatch = 64;
    auto *context = llama_init_from_model(model, cp);
    if (!context) {
        std::fprintf(stderr, "cannot create a context\n");
        return 1;
    }
    const auto *vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(256);
    const int count = llama_tokenize(vocab, prompt, int(std::strlen(prompt)), tokens.data(),
                                     int(tokens.size()), true, true);
    if (count <= 0) return 1;
    tokens.resize(count);

    const auto start_prefill = llama_time_us();
    if (llama_decode(context, llama_batch_get_one(tokens.data(), count)) != 0) {
        std::fprintf(stderr, "prefill failed\n");
        return 1;
    }
    const auto prefill_us = llama_time_us() - start_prefill;

    std::FILE *out = std::fopen(out_path, "w");
    if (!out) return 1;
    auto *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    std::int64_t decode_us = 0;
    int generated = 0;
    for (; generated < steps; ++generated) {
        llama_token next = llama_sampler_sample(sampler, context, -1);
        std::fprintf(out, "%d\n", next);
        char piece[256]{};
        const int n = llama_token_to_piece(vocab, next, piece, sizeof(piece) - 1, 0, true);
        std::fprintf(stderr, "%.*s", n > 0 ? n : 0, piece);
        if (llama_vocab_is_eog(vocab, next)) break;
        const auto step_start = llama_time_us();
        if (llama_decode(context, llama_batch_get_one(&next, 1)) != 0) {
            std::fprintf(stderr, "\ndecode failed at step %d\n", generated);
            break;
        }
        decode_us += llama_time_us() - step_start;
    }
    std::fclose(out);
    const auto served = stream.stats();
    const auto cached = cache.stats();
    std::fprintf(stderr, "\n");
    std::printf("{\"tokens\":%d,\"prefill_ms\":%.1f,\"decode_ms_per_token\":%.1f,"
                "\"copies\":%llu,\"experts\":%llu,\"rejected\":%llu,"
                "\"hits\":%llu,\"misses\":%llu,\"bytes_read\":%llu,\"cache_bytes\":%zu}\n",
                generated, double(prefill_us) / 1000.0,
                generated ? double(decode_us) / 1000.0 / generated : 0.0,
                (unsigned long long) served.copies, (unsigned long long) served.experts,
                (unsigned long long) served.rejected, (unsigned long long) cached.hits,
                (unsigned long long) cached.misses, (unsigned long long) cached.bytes_read,
                cached.allocated_bytes);
    llama_sampler_free(sampler);
    llama_free(context);
    llama_model_free(model);
    llama_backend_free();
    return served.copies ? 0 : 1;   // nothing streamed means the hook never ran
}
