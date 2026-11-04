// SPDX-License-Identifier: GPL-3.0-or-later
// Compatibility declaration for the audio.cpp YuE2 sources, which call
// ggml_round_bf16 from the ggml fork audio.cpp vendors. ProsperoAI builds
// audio.cpp against the shared pinned llama.cpp ggml, where the helper does
// not exist; tools/yue2-ggml-compat.cpp provides the definition as an
// f32 -> bf16 -> f32 cast round trip, which is numerically identical to the
// fork's unary kernel. Injected into the audio.cpp subproject with -include.
#pragma once

struct ggml_context;
struct ggml_tensor;

#ifdef __cplusplus
extern "C" {
#endif
struct ggml_tensor * ggml_round_bf16(struct ggml_context * ctx, struct ggml_tensor * a);
#ifdef __cplusplus
}
#endif
