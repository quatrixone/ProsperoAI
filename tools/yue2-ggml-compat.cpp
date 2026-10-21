// SPDX-License-Identifier: GPL-3.0-or-later
// Definition companion of tools/yue2-ggml-compat.hpp. The fork's kernel
// rounds f32/f16/bf16 input onto the bf16 grid and widens to f32; casting
// bf16 -> f32 after rounding uses the same round-to-nearest-even conversion
// the fork's scalar path performs.
#include "yue2-ggml-compat.hpp"

#include "ggml.h"

extern "C" struct ggml_tensor * ggml_round_bf16(struct ggml_context * ctx,
                                                struct ggml_tensor * a)
{
    struct ggml_tensor * narrowed = ggml_cast(ctx, a, GGML_TYPE_BF16);
    return ggml_cast(ctx, narrowed, GGML_TYPE_F32);
}
