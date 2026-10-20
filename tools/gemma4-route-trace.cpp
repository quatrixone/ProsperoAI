// SPDX-License-Identifier: GPL-3.0-or-later
// Records which routed experts a MoE GGUF actually selects, so a cache policy can
// be measured against real routing instead of a uniform stand-in. CPU backend,
// mmap: this reads the model, it does not need it resident.
#include "llama.h"
#include "ggml.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Capture {
    std::vector<int> layers;                       // layer index per captured node, in graph order
    std::vector<std::vector<int>> rows;            // one row per (layer, token): the selected ids
    std::vector<int> row_layer;
    int used = 0;
    bool failed = false;
};

int layer_of(const char *name) {
    const char *dash = std::strrchr(name, '-');
    return dash && dash[1] ? std::atoi(dash + 1) : -1;
}

bool observe(ggml_tensor *t, bool ask, void *user_data) {
    auto &capture = *static_cast<Capture *>(user_data);
    const bool wanted = std::strncmp(t->name, "ffn_moe_topk", 12) == 0;
    if (ask) return wanted;
    if (!wanted) return true;
    const int layer = layer_of(t->name);
    if (layer < 0 || t->type != GGML_TYPE_I32) {
        capture.failed = true;
        return true;
    }
    const int used = int(t->ne[0]), tokens = int(t->ne[1]);
    std::vector<int> ids(std::size_t(used) * tokens);
    ggml_backend_tensor_get(t, ids.data(), 0, ids.size() * sizeof(int));
    capture.used = used;
    for (int token = 0; token < tokens; ++token) {
        capture.rows.emplace_back(ids.begin() + std::size_t(token) * used,
                                  ids.begin() + std::size_t(token + 1) * used);
        capture.row_layer.push_back(layer);
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s MODEL.gguf OUT.trace STEPS [PROMPT]\n"
            "Writes one line per decode step and layer: 'step layer id id ...'\n", argv[0]);
        return 2;
    }
    const char *model_path = argv[1], *out_path = argv[2];
    const int steps = std::atoi(argv[3]);
    const char *prompt = argc > 4 ? argv[4] : "Explain why the sky is blue, in two sentences.";
    if (steps < 1) return 2;

    llama_backend_init();
    auto mp = llama_model_default_params();
    mp.n_gpu_layers = 0;                  // CPU only
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;  // the host has less RAM than the model
    auto *model = llama_model_load_from_file(model_path, mp);
    if (!model) {
        std::fprintf(stderr, "cannot load %s\n", model_path);
        return 1;
    }
    Capture capture;
    auto cp = llama_context_default_params();
    cp.n_ctx = 512;
    cp.n_batch = cp.n_ubatch = 64;
    cp.cb_eval = observe;
    cp.cb_eval_user_data = &capture;
    auto *context = llama_init_from_model(model, cp);
    if (!context) {
        std::fprintf(stderr, "cannot create a context\n");
        return 1;
    }
    const auto *vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(256);
    const int count = llama_tokenize(vocab, prompt, int(std::strlen(prompt)), tokens.data(),
                                     int(tokens.size()), true, true);
    if (count <= 0) {
        std::fprintf(stderr, "cannot tokenize the prompt\n");
        return 1;
    }
    tokens.resize(count);

    std::FILE *out = std::fopen(out_path, "w");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    // Prefill is one step; every generated token is another. Each step records the
    // rows the router actually selected, which is what a cache policy must serve.
    int step = 0;
    auto flush = [&] {
        for (std::size_t i = 0; i < capture.rows.size(); ++i) {
            std::fprintf(out, "%d %d", step, capture.row_layer[i]);
            for (const int id : capture.rows[i]) std::fprintf(out, " %d", id);
            std::fputc('\n', out);
        }
        capture.rows.clear();
        capture.row_layer.clear();
        ++step;
    };
    if (llama_decode(context, llama_batch_get_one(tokens.data(), count)) != 0) {
        std::fprintf(stderr, "prefill failed\n");
        return 1;
    }
    flush();
    auto *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    for (int generated = 0; generated < steps; ++generated) {
        llama_token next = llama_sampler_sample(sampler, context, -1);
        if (llama_vocab_is_eog(vocab, next)) break;
        if (llama_decode(context, llama_batch_get_one(&next, 1)) != 0) {
            std::fprintf(stderr, "decode failed at step %d\n", generated);
            break;
        }
        flush();
        std::fprintf(stderr, "\rstep %d/%d", generated + 1, steps);
    }
    std::fputc('\n', stderr);
    std::fclose(out);
    std::printf("{\"steps\":%d,\"experts_used\":%d,\"capture_failed\":%s,\"trace\":\"%s\"}\n",
                step, capture.used, capture.failed ? "true" : "false", out_path);
    llama_sampler_free(sampler);
    llama_free(context);
    llama_model_free(model);
    llama_backend_free();
    return capture.failed || step < 2 ? 1 : 0;
}
