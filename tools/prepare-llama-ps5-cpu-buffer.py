#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bound PS5 CPU backend buffers so Chatterbox weights can use smaller blocks."""

from pathlib import Path
from prepare_build import write_text_if_changed
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--ggml-source", type=Path, required=True)
args = parser.parse_args()
source = args.source
ggml_source = args.ggml_source
path = ggml_source / "src/ggml-backend.cpp"
text = path.read_text()
marker = "/* prospero-ps5-cpu-buffer-limit */"
if marker not in text:
    anchor = "static ggml_backend_buffer_t ggml_backend_cpu_buffer_type_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size) {"
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: expected one CPU buffer allocator")
    text = text.replace(
        anchor,
        marker
        + "\nstatic size_t ggml_backend_cpu_buffer_type_get_max_size(ggml_backend_buffer_type_t) {\n"
        + "    return 192ull * 1024ull * 1024ull;\n}\n\n"
        + anchor,
        1,
    )
replacement = "            /* .get_max_size        = */ ggml_backend_cpu_buffer_type_get_max_size, // PS5: bounded allocation blocks"
default = "            /* .get_max_size        = */ NULL, // defaults to SIZE_MAX"
old_limit = "    return 256ull * 1024ull * 1024ull;"
new_limit = "    return 192ull * 1024ull * 1024ull;"
if old_limit in text:
    text = text.replace(old_limit, new_limit, 1)
if replacement not in text:
    if text.count(default) < 1:
        raise SystemExit(f"{path}: expected a default CPU max-size hook")
    text = text.replace(default, replacement, 1)
write_text_if_changed(path, text)
print(f"{path}: CPU buffer allocation blocks capped at 192 MiB")

allocator = ggml_source / "src/ggml.c"
text = allocator.read_text()
map_marker = "/* prospero-ps5-large-cpu-mmap */"
if map_marker in text:
    text = text.replace(map_marker + "\n", "", 1)
    text = text.replace(
        "#if defined(__FreeBSD__)\n#include <sys/mman.h>\n#endif\n", "", 1)
    mmap_alloc = """  #elif defined(__FreeBSD__)
    if (size >= 64ull * 1024ull * 1024ull) {
        void * mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped != MAP_FAILED) return mapped;
    }
    int result = posix_memalign(&aligned_memory, alignment, size);
  #else
    int result = posix_memalign(&aligned_memory, alignment, size);
  #endif"""
    text = text.replace(mmap_alloc,
                        "  #else\n    int result = posix_memalign(&aligned_memory, alignment, size);\n  #endif", 1)
    mmap_free = """#elif defined(__FreeBSD__)
    if (ptr != NULL) {
        if (size >= 64ull * 1024ull * 1024ull) munmap(ptr, size);
        else free(ptr);
    }
#else
    free(ptr);
#endif"""
    text = text.replace(mmap_free, "#else\n    free(ptr);\n#endif", 1)
    write_text_if_changed(allocator, text)
