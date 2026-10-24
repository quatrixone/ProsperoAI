#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Let the loader leave streamed expert tensors unread.

A tensor in a streamed buffer type has address space and no memory: its experts
are read per token. Without this, the loader writes the whole tensor into that
address space, which is reserved and not mapped.
"""

from pathlib import Path
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='the llama.cpp source tree')
args = parser.parse_args()

loader = args.source / 'src/llama-model-loader.cpp'
text = loader.read_text()
before = '''        size_t n_size = ggml_nbytes(cur);

        const bool from_mapping = use_mmap || lazy.has(cur);'''
after = '''        size_t n_size = ggml_nbytes(cur);

        // A streamed tensor holds no data of its own; reading it here would write
        // into reserved address space. Its experts arrive per token instead.
        if (cur->buffer && ggml_backend_buft_is_streamed(ggml_backend_buffer_get_type(cur->buffer))) {
            size_done += n_size;
            continue;
        }

        const bool from_mapping = use_mmap || lazy.has(cur);'''
if after in text:
    raise SystemExit(0)
if text.count(before) != 1:
    raise SystemExit(f'{loader}: streamed-tensor skip no longer matches pinned llama.cpp')
loader.write_text(text.replace(before, after))
print(f'{loader}: streamed expert tensors are not read at load time')
