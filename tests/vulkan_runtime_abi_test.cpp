// SPDX-License-Identifier: GPL-3.0-or-later
#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include "llama.h"
#include "stable-diffusion.h"
#include "core/parallel.h"
#include <atomic>
#include "model_io/pickle_io.h"
#include <fstream>
#include <iterator>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
int main(int argc, char **argv)
{
    static_assert(GGML_MAX_NAME == 160);
    assert(sd::parallel_detail::executor == nullptr && !sd::parallel_detail::active);
    sd::ParallelExecutor executor(2);
    std::atomic<int> completed{0};
    std::vector<int> rows(64);
    {
        sd::ParallelScope scope(&executor);
        sd::parallel_for(0, 64, 1,
                         [&](int64_t first, int64_t last)
                         {
                             assert(sd::parallel_detail::executor == &executor &&
                                    sd::parallel_detail::active);
                             for (int64_t row = first; row < last; ++row)
                             {
                                 rows[row] = static_cast<int>(row);
                                 ++completed;
                             }
                         });
    }
    assert(completed == 64);
    for (int row = 0; row < 64; ++row)
        assert(rows[row] == row);
    assert(sd::parallel_detail::executor == nullptr && !sd::parallel_detail::active);
    llama_backend_init();
    // A long tensor name crosses the old 64-byte boundary. Both allocation and
    // access must agree with the library, not merely the application's header.
    auto *ctx = ggml_init({ggml_tensor_overhead() * 4, nullptr, true});
    assert(ctx);
    auto *tensor = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4);
    const std::string name(140, 'x');
    ggml_set_name(tensor, name.c_str());
    assert(std::strcmp(ggml_get_name(tensor), name.c_str()) == 0);
    assert(tensor->ne[0] == 4 && tensor->type == GGML_TYPE_F32);
    assert(std::strcmp(sd_commit(), "b13a48219d5cdf408edcb93da6cd69b612ed03d3") == 0);
    if (argc > 2)
    {
        std::ifstream file(argv[2], std::ios::binary);
        assert(file);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        std::vector<TensorStorage> tensors;
        std::unordered_map<std::string, uint64_t> sizes;
        std::string error;
        assert(parse_torch_state_dict_pickle(bytes.data(), bytes.size(), tensors, sizes, &error));
        assert(tensors.size() == 1 && tensors[0].name == "weight");
        assert(tensors[0].type == GGML_TYPE_F32 && tensors[0].n_dims == 2);
        assert(tensors[0].ne[0] == 2 && tensors[0].ne[1] == 2 && sizes.at("0") == 16);
        assert(!parse_torch_state_dict_pickle(bytes.data(), 3, tensors, sizes, &error));
    }
    sd_ctx_params_t parameters;
    sd_ctx_params_init(&parameters);
    parameters.backend = "CPU,diffusion=Vulkan0,vae=Vulkan0";
    parameters.params_backend = "Vulkan0";
    sd_img_gen_params_t generation;
    sd_img_gen_params_init(&generation);
    assert(generation.width > 0 && generation.height > 0);
    const size_t size = sd_list_devices(nullptr, 0);
    assert(size > 0);
    std::vector<char> devices(size + 1);
    sd_list_devices(devices.data(), devices.size());
    assert(std::strstr(devices.data(), "Vulkan0") && std::strstr(devices.data(), "CPU"));
    std::printf("vulkan-runtime ggml ABI: name=%d tensor=%zu; llama + SD see devices:\n%s\n",
                GGML_MAX_NAME, sizeof(ggml_tensor), devices.data());
    ggml_free(ctx);
    if (argc > 1 && argv[1][0])
    {
        ggml_backend_dev_t selected[] = {ggml_backend_dev_by_name("Vulkan0"), nullptr};
        assert(selected[0]);
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = 99;
        mp.devices = selected;
        auto *model = llama_model_load_from_file(argv[1], mp);
        assert(model);
        auto cp = llama_context_default_params();
        cp.n_ctx = 128;
        cp.n_batch = 16;
        cp.n_ubatch = 16;
        cp.n_threads = 2;
        cp.n_threads_batch = 2;
        auto *inference = llama_init_from_model(model, cp);
        assert(inference);
        const auto *vocab = llama_model_get_vocab(model);
        llama_token tokens[16];
        const char *prompt = "Once upon a time";
        int count = llama_tokenize(vocab, prompt, std::strlen(prompt), tokens, 16, true, true);
        assert(count > 0 && count <= 16);
        assert(llama_decode(inference, llama_batch_get_one(tokens, count)) == 0);
        const float *logits = llama_get_logits_ith(inference, -1);
        assert(logits);
        for (int i = 0; i < llama_vocab_n_tokens(vocab); ++i)
            assert(std::isfinite(logits[i]));
        std::puts("vulkan-runtime llama Vulkan decode: finite logits");
        llama_free(inference);
        llama_model_free(model);
    }
    llama_backend_free();
}
