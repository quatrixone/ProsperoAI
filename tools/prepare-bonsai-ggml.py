#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add the pinned Bonsai private quant formats to the upstream GGML source tree.

This is a build-time compatibility adapter. It keeps the upstream GGML ABI and
does not link or expose the separate Prism llama runtime.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from prepare_build import write_text_if_changed


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one patch anchor, got {count}: {old[:90]!r}")
    write_text_if_changed(path, text.replace(old, new, 1))


def extract_function(source: Path, signature: str) -> str:
    text = source.read_text()
    start = text.find(signature)
    if start < 0 or text.find(signature, start + 1) >= 0:
        raise SystemExit(f"{source}: expected one function {signature!r}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"{source}: no function body for {signature!r}")
    depth = 0
    for end in range(brace, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start : end + 1]
    raise SystemExit(f"{source}: unterminated function {signature!r}")


def add_before(path: Path, anchor: str, addition: str) -> None:
    text = path.read_text()
    if addition.strip() in text:
        return
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: expected one insertion point {anchor!r}")
    write_text_if_changed(path, text.replace(anchor, addition + "\n\n" + anchor, 1))


def add_case_to_function(path: Path, signature: str, anchor: str, case: str) -> None:
    """Add one type case to one upstream function without touching other switches."""
    function = extract_function(path, signature)
    if case in function:
        return
    if function.count(anchor) != 1:
        raise SystemExit(f"{path}: expected one case anchor in {signature!r}")
    patched = function.replace(anchor, anchor + case, 1)
    replace_once(path, function, patched)


def add_line_after_prefix(path: Path, prefix: str, line: str) -> None:
    text = path.read_text()
    if line in text:
        return
    lines = text.splitlines(keepends=True)
    matches = [i for i, current in enumerate(lines) if prefix in current]
    if len(matches) != 1:
        raise SystemExit(f"{path}: expected one line containing {prefix!r}, got {len(matches)}")
    lines.insert(matches[0] + 1, line + "\n")
    write_text_if_changed(path, "".join(lines))


def prepare_ptq1_vulkan(ggml: Path, prism: Path) -> None:
    """Port PTQ1_0 shader decoding into upstream's shader source tree."""
    vk = ggml / "src/ggml-vulkan"
    prism_vk = prism / "src/ggml-vulkan"
    shader_dir = vk / "vulkan-shaders"
    prism_shader_dir = prism_vk / "vulkan-shaders"

    for name in ("ptq1_0.glsl", "mul_mat_vec_ptq1_0.comp", "dequant_ptq1_0.comp"):
        target = shader_dir / name
        source = prism_shader_dir / name
        if not target.exists():
            write_text_if_changed(target, source.read_text())

    types = shader_dir / "types.glsl"
    add_before(types, "#define QUANT_K_Q2_0 64", """
// PTQ1_0 private ternary format (128 values per block).
#define QUANT_K_PTQ1_0 128
#define QUANT_R_PTQ1_0 1
struct block_ptq1_0 { uint8_t qs[24]; uint8_t qh[2]; float16_t d; };
#if defined(DATA_A_PTQ1_0)
#define QUANT_K QUANT_K_PTQ1_0
#define QUANT_R QUANT_R_PTQ1_0
#define QUANT_AUXF 1
#define A_TYPE block_ptq1_0
#endif
""")

    dequant = shader_dir / "dequant_funcs.glsl"
    add_before(dequant, "#if defined(DATA_A_Q1_0)\nvec2 dequantize", """
#if defined(DATA_A_PTQ1_0)
#include "ptq1_0.glsl"
vec2 dequantize(uint ib, uint iqs, uint a_offset) {
    return vec2(ptq1_0_trit(ib, a_offset, iqs), ptq1_0_trit(ib, a_offset, iqs + 1u));
}
vec4 dequantize4(uint ib, uint iqs, uint a_offset) {
    return vec4(ptq1_0_trit(ib, a_offset, iqs), ptq1_0_trit(ib, a_offset, iqs + 1u),
                ptq1_0_trit(ib, a_offset, iqs + 2u), ptq1_0_trit(ib, a_offset, iqs + 3u));
}
vec2 get_dm(uint ib, uint a_offset) { return vec2(float(data_a[a_offset + ib].d), 0); }
#endif
""")

    generator = shader_dir / "vulkan-shaders-gen.cpp"
    add_before(generator, '    "q2_0",', '    "ptq1_0",')
    replace_once(generator,
        'tname == "tq2_0" || tname == "tq1_0") ? "mul_mat_vec_" + tname + ".comp" : "mul_mat_vec.comp";',
        'tname == "tq2_0" || tname == "tq1_0" || tname == "ptq1_0") ? "mul_mat_vec_" + tname + ".comp" : "mul_mat_vec.comp";')

    # Register the generated PTQ shaders in upstream's Vulkan backend and let
    # the backend select them for mat-vec decode. Prefill can use upstream's
    # existing dequantize-to-f16 fallback when no PTQ matrix-matrix kernel is
    # available; this avoids importing Prism's backend ABI.
    backend = vk / "ggml-vulkan.cpp"
    pipeline_anchors = (
        ('            ggml_vk_create_pipeline(device, device->pipeline_dequant_mul_mat_vec_f32_f32[w][GGML_TYPE_Q1_0][i],',
         '            ggml_vk_create_pipeline(device, device->pipeline_dequant_mul_mat_vec_f32_f32[w][GGML_TYPE_PTQ1_0][i], "mul_mat_vec_ptq1_0_f32_f32", arr_dmmv_ptq1_0_f32_f32_len[reduc], arr_dmmv_ptq1_0_f32_f32_data[reduc], "main", mul_mat_vec_num_bindings, sizeof(vk_mat_vec_push_constants), {2*rm_stdq, 1, 1}, {wg_size_subgroup, 2*rm_stdq, i+1}, 1, true, use_subgroups, force_subgroup_size);'),
        ('            ggml_vk_create_pipeline(device, device->pipeline_dequant_mul_mat_vec_f16_f32[w][GGML_TYPE_Q1_0][i],',
         '            ggml_vk_create_pipeline(device, device->pipeline_dequant_mul_mat_vec_f16_f32[w][GGML_TYPE_PTQ1_0][i], "mul_mat_vec_ptq1_0_f16_f32", arr_dmmv_ptq1_0_f16_f32_len[reduc], arr_dmmv_ptq1_0_f16_f32_data[reduc], "main", mul_mat_vec_num_bindings, sizeof(vk_mat_vec_push_constants), {2*rm_stdq, 1, 1}, {wg_size_subgroup, 2*rm_stdq, i+1}, 1, true, use_subgroups, force_subgroup_size);'),
    )
    for anchor, addition in pipeline_anchors:
        add_line_after_prefix(backend, anchor, addition)

    # Dedicated dequant/get_rows shaders are generated for PTQ1_0 above.
    for anchor, addition in (
        ('    ggml_vk_create_pipeline(device, device->pipeline_dequant[GGML_TYPE_Q1_0],',
         '    ggml_vk_create_pipeline(device, device->pipeline_dequant[GGML_TYPE_PTQ1_0], "dequant_ptq1_0", dequant_ptq1_0_len, dequant_ptq1_0_data, "main", 2, 5 * sizeof(uint32_t), {256 * 8, 1, 1}, {}, 1);'),
        ('    ggml_vk_create_pipeline(device, device->pipeline_get_rows[GGML_TYPE_Q1_0],',
         '    ggml_vk_create_pipeline(device, device->pipeline_get_rows[GGML_TYPE_PTQ1_0], "get_rows_ptq1_0", get_rows_ptq1_0_len, get_rows_ptq1_0_data, "main", 3, sizeof(vk_op_binary_push_constants), {1024, 1, 1}, {}, 1);'),
        ('    ggml_vk_create_pipeline(device, device->pipeline_get_rows_f32[GGML_TYPE_Q1_0],',
         '    ggml_vk_create_pipeline(device, device->pipeline_get_rows_f32[GGML_TYPE_PTQ1_0], "get_rows_ptq1_0_f32", get_rows_ptq1_0_f32_len, get_rows_ptq1_0_f32_data, "main", 3, sizeof(vk_op_binary_push_constants), {1024, 1, 1}, {}, 1);'),
    ):
        add_line_after_prefix(backend, anchor, addition)

    case_anchor = "        case GGML_TYPE_Q1_0:\n"
    add_case_to_function(backend, "vk_pipeline ggml_vk_get_to_fp16(", case_anchor,
                         "        case GGML_TYPE_PTQ1_0:\n")
    add_case_to_function(backend, "static vk_pipeline ggml_vk_get_dequantize_mul_mat_vec(", case_anchor,
                         "        case GGML_TYPE_PTQ1_0:\n")
    # supports_op has several type matrices; only matmul and GET_ROWS have a
    # generated/dequant fallback for PTQ in this initial upstream port.
    support_fn = extract_function(backend, "static bool ggml_backend_vk_device_supports_op(")
    support_patched = support_fn
    marker = "                    case GGML_TYPE_Q1_0:\n                    case GGML_TYPE_Q2_0:"
    patched_marker = marker.replace(
        "case GGML_TYPE_Q1_0:\n",
        "case GGML_TYPE_Q1_0:\n                    case GGML_TYPE_PTQ1_0:\n",
    )
    if "case GGML_TYPE_PTQ1_0:" not in support_patched:
        if support_patched.count(marker) < 2:
            raise SystemExit(f"{backend}: expected matmul and GET_ROWS type cases")
        support_patched = support_patched.replace(marker, patched_marker, 2)
    if support_patched != support_fn:
        replace_once(backend, support_fn, support_patched)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ggml-source", type=Path, required=True)
    parser.add_argument("--prism-source", type=Path, required=True)
    args = parser.parse_args()
    ggml = args.ggml_source
    prism = args.prism_source / "ggml"

    # The custom IDs are outside upstream's current range, so every ordinary
    # upstream enum value and its GGUF encoding stays unchanged.
    replace_once(
        ggml / "include/ggml.h",
        "        GGML_TYPE_COUNT   = 43,",
        "        GGML_TYPE_PQ2_0  = 142, // PrismML Q2, group 128\n"
        "        GGML_TYPE_PTQ1_0 = 143, // PrismML ternary, group 128\n"
        "        GGML_TYPE_COUNT   = 144,",
    )
    # Block definitions are copied from the exact Prism source pin that defines
    # the file format; they are not aliases for upstream Q2_0/TQ1_0 blocks.
    prism_common = (prism / "src/ggml-common.h").read_text()
    p0 = prism_common.index("#define QK_PQ2_0 128")
    p1 = prism_common.index("#define QK4_0", p0)
    private_blocks = prism_common[p0:p1]
    common = ggml / "src/ggml-common.h"
    text = common.read_text()
    anchor = "#define QK4_0"
    if "#define QK_PQ2_0 128" not in text:
        if text.count(anchor) != 1:
            raise SystemExit(f"{common}: upstream Q4_0 block anchor changed")
        write_text_if_changed(common, text.replace(anchor, private_blocks + "\n" + anchor, 1))

    # Preserve upstream's generic tensor API while adding the exact codec entry
    # points and layouts used by the model file.
    qh = ggml / "src/ggml-quants.h"
    add_before(qh, "// Dequantization", """
GGML_API void quantize_row_pq2_0_ref(const float * GGML_RESTRICT, block_pq2_0 * GGML_RESTRICT, int64_t);
GGML_API void quantize_row_ptq1_0_ref(const float * GGML_RESTRICT, block_ptq1_0 * GGML_RESTRICT, int64_t);
GGML_API void dequantize_row_pq2_0(const block_pq2_0 * GGML_RESTRICT, float * GGML_RESTRICT, int64_t);
GGML_API void dequantize_row_ptq1_0(const block_ptq1_0 * GGML_RESTRICT, float * GGML_RESTRICT, int64_t);
GGML_API size_t quantize_pq2_0(const float * GGML_RESTRICT, void * GGML_RESTRICT, int64_t, int64_t, const float *);
GGML_API size_t quantize_ptq1_0(const float * GGML_RESTRICT, void * GGML_RESTRICT, int64_t, int64_t, const float *);
""")
    qsrc = ggml / "src/ggml-quants.c"
    psrc = prism / "src/ggml-quants.c"
    codec_functions = [
        extract_function(psrc, "void quantize_row_pq2_0_ref("),
        extract_function(psrc, "void dequantize_row_pq2_0("),
        extract_function(psrc, "void quantize_row_ptq1_0_ref("),
        extract_function(psrc, "void dequantize_row_ptq1_0("),
        extract_function(psrc, "size_t quantize_pq2_0("),
        extract_function(psrc, "size_t quantize_ptq1_0("),
    ]
    if "static const size_t ptq1_0_stages[3]" not in qsrc.read_text():
        add_before(qsrc, "void quantize_row_q4_0_ref(",
                   "static const size_t ptq1_0_stages[3] = {32, 16, 8};")
    for function in codec_functions:
        signature = function[: function.index("{")].strip()
        if signature not in qsrc.read_text():
            add_before(qsrc, "void quantize_row_q4_0_ref(", function)

    # Expose the quantized types to GGML's central type table.
    core = ggml / "src/ggml.c"
    add_before(core, "    [GGML_TYPE_Q4_0] = {", """
    [GGML_TYPE_PQ2_0] = {
        .type_name = "pq2_0", .blck_size = QK_PQ2_0, .type_size = sizeof(block_pq2_0),
        .is_quantized = true, .to_float = (ggml_to_float_t) dequantize_row_pq2_0,
        .from_float_ref = (ggml_from_float_t) quantize_row_pq2_0_ref,
    },
    [GGML_TYPE_PTQ1_0] = {
        .type_name = "ptq1_0", .blck_size = QK_PTQ1_0, .type_size = sizeof(block_ptq1_0),
        .is_quantized = true, .to_float = (ggml_to_float_t) dequantize_row_ptq1_0,
        .from_float_ref = (ggml_from_float_t) quantize_row_ptq1_0_ref,
    },
""")
    replace_once(
        core,
        "        case GGML_TYPE_Q2_0:    result = quantize_q2_0   (src + start, (char *) dst + start_row * row_size, nrows, n_per_row, imatrix); break;",
        "        case GGML_TYPE_Q2_0:    result = quantize_q2_0   (src + start, (char *) dst + start_row * row_size, nrows, n_per_row, imatrix); break;\n"
        "        case GGML_TYPE_PQ2_0: result = quantize_pq2_0(src + start, (char *) dst + start_row * row_size, nrows, n_per_row, imatrix); break;\n"
        "        case GGML_TYPE_PTQ1_0: result = quantize_ptq1_0(src + start, (char *) dst + start_row * row_size, nrows, n_per_row, imatrix); break;",
    )

    # CPU fallback: the selected PS5 Bonsai model stays in system memory until
    # its Vulkan kernels have been ported. Dot products intentionally use the
    # exact group-128 layouts and Q8_0 activations.
    cpu = ggml / "src/ggml-cpu"
    cpu_quants = cpu / "quants.c"
    cpu_prism_quants = prism / "src/ggml-cpu/quants.c"
    cpu_functions = [
        extract_function(cpu_prism_quants, "void ggml_vec_dot_pq2_0_q8_0_generic("),
        extract_function(cpu_prism_quants, "void ggml_vec_dot_ptq1_0_q8_0_generic("),
    ]
    cpu_wrappers = """
void quantize_row_pq2_0(const float * GGML_RESTRICT x, void * GGML_RESTRICT y, int64_t k) {
    quantize_row_pq2_0_ref(x, y, k);
}
void quantize_row_ptq1_0(const float * GGML_RESTRICT x, void * GGML_RESTRICT y, int64_t k) {
    quantize_row_ptq1_0_ref(x, y, k);
}
"""
    if "void quantize_row_pq2_0(" not in cpu_quants.read_text():
        add_before(cpu_quants, "void ggml_vec_dot_q4_0_q8_0_generic(", cpu_wrappers)
    for function in cpu_functions:
        signature = function[: function.index("{")].strip()
        if signature not in cpu_quants.read_text():
            add_before(cpu_quants, "void ggml_vec_dot_q4_0_q8_0_generic(", function)
    add_before(cpu / "quants.h", "void quantize_row_q4_0(", """
void quantize_row_pq2_0(const float * GGML_RESTRICT, void * GGML_RESTRICT, int64_t);
void quantize_row_ptq1_0(const float * GGML_RESTRICT, void * GGML_RESTRICT, int64_t);
""")
    add_before(cpu / "quants.h", "void ggml_vec_dot_q4_0_q8_0(", """
void ggml_vec_dot_pq2_0_q8_0(int, float * GGML_RESTRICT, size_t, const void * GGML_RESTRICT, size_t, const void * GGML_RESTRICT, size_t, int);
void ggml_vec_dot_ptq1_0_q8_0(int, float * GGML_RESTRICT, size_t, const void * GGML_RESTRICT, size_t, const void * GGML_RESTRICT, size_t, int);
""")
    fallback = cpu / "arch-fallback.h"
    replace_once(
        fallback,
        "#elif defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64)\n// quants.c\n#define ggml_vec_dot_q2_0_q8_0_generic ggml_vec_dot_q2_0_q8_0",
        "#elif defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64)\n// quants.c\n"
        "#define ggml_vec_dot_pq2_0_q8_0_generic ggml_vec_dot_pq2_0_q8_0\n"
        "#define ggml_vec_dot_ptq1_0_q8_0_generic ggml_vec_dot_ptq1_0_q8_0\n"
        "#define ggml_vec_dot_q2_0_q8_0_generic ggml_vec_dot_q2_0_q8_0",
    )
    add_before(cpu / "ggml-cpu.c", "    [GGML_TYPE_Q4_0] = {", """
    [GGML_TYPE_PQ2_0] = {
        .from_float = quantize_row_pq2_0,
        .vec_dot = ggml_vec_dot_pq2_0_q8_0,
        .vec_dot_type = GGML_TYPE_Q8_0,
        .nrows = 1,
    },
    [GGML_TYPE_PTQ1_0] = {
        .from_float = quantize_row_ptq1_0,
        .vec_dot = ggml_vec_dot_ptq1_0_q8_0,
        .vec_dot_type = GGML_TYPE_Q8_0,
        .nrows = 1,
    },
""")
    ops = cpu / "ops.cpp"
    text = ops.read_text()
    if "case GGML_TYPE_PQ2_0:" not in text:
        old = "        case GGML_TYPE_Q2_0:\n"
        if text.count(old) < 1:
            raise SystemExit(f"{ops}: no Q2_0 operation cases")
        text = text.replace(old, old + "        case GGML_TYPE_PQ2_0:\n        case GGML_TYPE_PTQ1_0:\n")
        write_text_if_changed(ops, text)
    prepare_ptq1_vulkan(ggml, prism)
    print(f"{ggml}: added Bonsai private quant format metadata and codecs")


if __name__ == "__main__":
    main()
