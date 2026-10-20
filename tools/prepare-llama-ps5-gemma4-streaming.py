#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enable mmap-backed lazy reads for Gemma 4 routed experts in pinned llama.cpp."""

from pathlib import Path
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
args = parser.parse_args()
source = args.source


def replace(path, before, after, expected=1):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != expected:
        raise SystemExit(f'{path}: expected {expected} Gemma 4 lazy-expert hooks in pinned llama.cpp')
    path.write_text(text.replace(before, after))


model = source / 'src/models/gemma4.cpp'
replace(model,
        '{n_embd, n_ff_exp * 2, n_expert}, TENSOR_NOT_REQUIRED);',
        '{n_embd, n_ff_exp * 2, n_expert}, TENSOR_NOT_REQUIRED | TENSOR_READ_LAZY);')
replace(model,
        '{n_embd, n_ff_exp, n_expert}, 0);',
        '{n_embd, n_ff_exp, n_expert}, TENSOR_READ_LAZY);', expected=2)
replace(model,
        '{n_ff_exp, n_embd, n_expert}, 0);',
        '{n_ff_exp, n_embd, n_expert}, TENSOR_READ_LAZY);')

# The explicit PS5 mode must fail closed if mmap cannot be used. Falling back to
# eager loading would turn a low-memory experiment into a full model allocation.
loader = source / 'src/llama-model-loader.cpp'
replace(loader,
        '''    if (!llama_mmap::SUPPORTED) {
        LLAMA_LOG_WARN("%s: mmap is not available, so tensor %s (size = %zu MiB) is loaded into RAM in full\\n",
                __func__, name.c_str(), ggml_nbytes(t)/1024/1024);
        return false;
    }''',
        '''    if (!llama_mmap::SUPPORTED) {
        if (mode == LLAMA_LAZY_MODE_ON) {
            throw std::runtime_error(format("%s: explicit lazy loading requires mmap (tensor %s)",
                    __func__, name.c_str()));
        }
        LLAMA_LOG_WARN("%s: mmap is not available, so tensor %s (size = %zu MiB) is loaded into RAM in full\\n",
                __func__, name.c_str(), ggml_nbytes(t)/1024/1024);
        return false;
    }''')

print(f'{model}: routed Gemma 4 expert matrices use lazy mmap reads')
