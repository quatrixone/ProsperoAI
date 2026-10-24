#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Let a host weight tensor supply its experts from somewhere other than memory.

The scheduler already copies only the experts a token uses out of an offloaded
MoE weight tensor, but it reads them straight from `input->data`, which requires
every expert to be resident. This adds one hook in front of that read so a
bounded reader can supply the bytes instead. With no hook installed the
behaviour is unchanged.
"""

from pathlib import Path
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='the ggml source tree')
args = parser.parse_args()
source = args.source


def replace(path, before, after, marker, expected=1):
    """Applies one edit once. `marker` is a token the edit introduces, so an
    already-patched tree is left alone even if the edit is later extended."""
    text = path.read_text()
    if marker in text:
        return False
    if text.count(before) != expected:
        raise SystemExit(f'{path}: expert-stream hook no longer matches pinned ggml')
    path.write_text(text.replace(before, after))
    return True


header = source / 'include/ggml-backend.h'
replace(header,
        '''    GGML_API void                 ggml_backend_sched_set_eval_callback(ggml_backend_sched_t sched, ggml_backend_sched_eval_callback callback, void * user_data);''',
        '''    GGML_API void                 ggml_backend_sched_set_eval_callback(ggml_backend_sched_t sched, ggml_backend_sched_eval_callback callback, void * user_data);

    // Supplies experts [first_id, last_id] of a host MoE weight tensor whose data
    // is not resident. Returns false to let the scheduler copy from weights->data
    // as usual. Set to NULL (the default) to keep the stock behaviour.
    typedef bool (*ggml_backend_expert_copy_t)(
            ggml_backend_t backend, struct ggml_tensor * destination,
            const struct ggml_tensor * weights, int32_t first_id, int32_t last_id,
            size_t expert_size, void * user_data);
    GGML_API void ggml_backend_set_expert_copy(ggml_backend_expert_copy_t callback, void * user_data);

    // Marks a buffer type whose tensors hold no data of their own: a loader must
    // not write their bytes, and the expert-copy callback supplies them instead.
    GGML_API void ggml_backend_set_streamed_buft(ggml_backend_buffer_type_t buft);
    GGML_API bool ggml_backend_buft_is_streamed(ggml_backend_buffer_type_t buft);''',
        marker='ggml_backend_expert_copy_t')

backend = source / 'src/ggml-backend.cpp'
replace(backend,
        '''// scheduler''',
        '''// expert streaming hook

static ggml_backend_expert_copy_t g_expert_copy = NULL;
static void * g_expert_copy_user_data = NULL;

void ggml_backend_set_expert_copy(ggml_backend_expert_copy_t callback, void * user_data) {
    g_expert_copy = callback;
    g_expert_copy_user_data = user_data;
}

static ggml_backend_buffer_type_t g_streamed_buft = NULL;

void ggml_backend_set_streamed_buft(ggml_backend_buffer_type_t buft) {
    g_streamed_buft = buft;
}

bool ggml_backend_buft_is_streamed(ggml_backend_buffer_type_t buft) {
    return buft != NULL && buft == g_streamed_buft;
}

// scheduler''',
        marker='g_expert_copy')

replace(backend,
        '''                    auto copy_experts = [&](int32_t first_id, int32_t last_id) {
                        const size_t expert_offset = first_id * expert_size;''',
        '''                    auto copy_experts = [&](int32_t first_id, int32_t last_id) {
                        if (g_expert_copy && g_expert_copy(split_backend, input_cpy, input,
                                                           first_id, last_id, expert_size,
                                                           g_expert_copy_user_data)) {
                            return;
                        }
                        const size_t expert_offset = first_id * expert_size;''',
        marker='g_expert_copy_user_data))')

print(f'{backend}: offloaded MoE experts can be supplied by a reader')
