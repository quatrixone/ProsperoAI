#!/usr/bin/env python3
"""Backport ggml_graph_set_n_nodes for runtimes built against older pinned ggml."""

from __future__ import annotations

import argparse
from pathlib import Path
from prepare_build import write_text_if_changed


ROOT = Path(__file__).resolve().parents[1]
HEADER_DECL = "    GGML_API int                   ggml_graph_n_nodes(struct ggml_cgraph * cgraph);"
HEADER_SETTER = "    GGML_API void                  ggml_graph_set_n_nodes(struct ggml_cgraph * cgraph, int n_nodes);"
SOURCE_GETTER = """int ggml_graph_n_nodes(struct ggml_cgraph * cgraph) {
    return cgraph->n_nodes;
}"""
SOURCE_SETTER = """void ggml_graph_set_n_nodes(struct ggml_cgraph * cgraph, int n_nodes) {
    GGML_ASSERT(n_nodes >= 0);
    GGML_ASSERT(n_nodes <= cgraph->size);
    cgraph->n_nodes = n_nodes;
}"""
MATH_CONDITION = "elseif (NOT WIN32 AND NOT DEFINED ENV{ONEAPI_ROOT})"
PS5_MATH_CONDITION = "elseif (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL \"FreeBSD\" AND NOT DEFINED ENV{ONEAPI_ROOT})"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=ROOT / "build/prism-llama-source/ggml",
        help="ggml source tree to patch (default: ProsperoAI's pinned build source)",
    )
    args = parser.parse_args()
    source = args.source.resolve()
    header = source / "include/ggml.h"
    implementation = source / "src/ggml.c"
    cmake = source / "src/CMakeLists.txt"
    if not header.is_file() or not implementation.is_file() or not cmake.is_file():
        raise SystemExit(f"{source}: not a ggml source tree (missing required source files)")

    header_text = header.read_text()
    implementation_text = implementation.read_text()
    cmake_text = cmake.read_text()
    header_has_setter = HEADER_SETTER in header_text
    implementation_has_setter = SOURCE_SETTER in implementation_text
    if header_has_setter != implementation_has_setter:
        raise SystemExit(f"{source}: partial graph setter patch; inspect both files before continuing")
    if not header_has_setter:
        if header_text.count(HEADER_DECL) != 1:
            raise SystemExit(f"{header}: expected exactly one compatibility insertion point")
        if implementation_text.count(SOURCE_GETTER) != 1:
            raise SystemExit(f"{implementation}: expected exactly one compatibility insertion point")
        write_text_if_changed(header, header_text.replace(HEADER_DECL, HEADER_DECL + "\n" + HEADER_SETTER, 1))
        write_text_if_changed(implementation,
            implementation_text.replace(SOURCE_GETTER, SOURCE_GETTER + "\n\n" + SOURCE_SETTER, 1)
        )
    if PS5_MATH_CONDITION not in cmake_text:
        if cmake_text.count(MATH_CONDITION) != 1:
            raise SystemExit(f"{cmake}: expected exactly one math-library compatibility site")
        write_text_if_changed(cmake, cmake_text.replace(MATH_CONDITION, PS5_MATH_CONDITION, 1))
    header_changed = not header_has_setter
    print(
        f"{source}: graph setter {'added' if header_changed else 'present'}; "
        "PS5 libm-link compatibility present"
    )


if __name__ == "__main__":
    main()
