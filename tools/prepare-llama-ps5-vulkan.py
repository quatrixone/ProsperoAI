#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add guarded, runtime-configurable PS5/PS5 Pro Vulkan tuning hooks."""
from pathlib import Path
from prepare_build import write_text_if_changed
import argparse


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--ggml-source", type=Path, required=True)
args = parser.parse_args()
path = args.ggml_source / "src/ggml-vulkan/ggml-vulkan.cpp"
text = path.read_text()
marker = "/* prospero-ps5-vulkan-profile */"
chunk_default = "            device->suballocation_block_size = 1024*1024*1024;"
chunk_256 = "            device->suballocation_block_size = 256*1024*1024; // PS5: keep weight buffers below large-allocation failures"
chunk_ps5 = "            device->suballocation_block_size = 128*1024*1024; // PS5: keep weight buffers below large-allocation failures"
if chunk_256 in text:
    text = text.replace(chunk_256, chunk_ps5, 1)
    write_text_if_changed(path, text)
elif chunk_default in text:
    text = text.replace(chunk_default, chunk_ps5, 1)
    write_text_if_changed(path, text)
elif chunk_ps5 not in text:
    raise SystemExit(f"{path}: expected Vulkan suballocation block size default")
if marker in text:
    print(f"{path}: PS5 Vulkan profile hooks already applied")
    raise SystemExit(0)


def replace(before: str, after: str, count: int = 1) -> None:
    global text
    found = text.count(before)
    if found != count:
        raise SystemExit(f"{path}: expected {count} match(es), found {found}:\n{before}")
    text = text.replace(before, after)


if "#include <cstdlib>" not in text:
    if "#include <cmath>" in text:
        replace("#include <cmath>", "#include <cmath>\n#include <cstdlib>")
    elif '#include "ggml-vulkan-common.h"' in text:
        replace('#include "ggml-vulkan-common.h"',
                '#include "ggml-vulkan-common.h"\n#include <cstdlib>')
    else:
        raise SystemExit(f"{path}: no safe include insertion point for PS5 profile hooks")
shader_loader_signature = next((signature for signature in (
    "static void ggml_vk_load_shaders(vk_device& device, vk_pipeline requested) {",
    "void ggml_vk_load_shaders(vk_device& device, vk_pipeline requested) {",
) if signature in text), None)
if shader_loader_signature is None:
    raise SystemExit(f"{path}: unsupported Vulkan shader-loader signature")
replace(
    shader_loader_signature,
    marker
    + r"""
static bool prospero_vk_read_u32(const char *name, uint32_t minimum, uint32_t maximum,
                                 uint32_t *result) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed < minimum || parsed > maximum) return false;
    *result = static_cast<uint32_t>(parsed);
    return true;
}

static uint64_t prospero_vk_submit_flops(uint64_t fallback) {
    const char *value = std::getenv("GGML_VK_PS5_SUBMIT_MFLOP");
    if (value == nullptr || *value == '\0') return fallback;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed > UINT64_MAX / 1000000ULL) return fallback;
    return static_cast<uint64_t>(parsed) * 1000000ULL;
}

""" + shader_loader_signature,
)
replace(
    "    uint32_t rm_iq = 2 * rm_kq;\n",
    """    uint32_t profile_value = 0;
    if (prospero_vk_read_u32("GGML_VK_PS5_RM_STDQ", 1, 1024, &profile_value)) rm_stdq = profile_value;
    if (prospero_vk_read_u32("GGML_VK_PS5_RM_KQ", 1, 1024, &profile_value)) rm_kq = profile_value;
    uint32_t rm_iq = 2 * rm_kq;
""",
)
replace(
    "    const uint32_t subgroup_size = (device->vendor_id == VK_VENDOR_ID_INTEL && device->subgroup_size_control && device->subgroup_min_size <= 16 && device->subgroup_max_size >= 16) ? 16 : device->subgroup_size;\n",
    """    uint32_t subgroup_size = (device->vendor_id == VK_VENDOR_ID_INTEL && device->subgroup_size_control && device->subgroup_min_size <= 16 && device->subgroup_max_size >= 16) ? 16 : device->subgroup_size;
    uint32_t requested_subgroup_size = 0;
    if (device->subgroup_size_control &&
        prospero_vk_read_u32("GGML_VK_PS5_DMMV_SUBGROUP", device->subgroup_min_size,
                             device->subgroup_max_size, &requested_subgroup_size)) {
        subgroup_size = requested_subgroup_size;
    }
    static bool prospero_profile_logged = false;
    if (!prospero_profile_logged) {
        prospero_profile_logged = true;
        GGML_LOG_INFO("ggml_vulkan: ps5-tuning arch=%d subgroup=%u device_subgroup=%u rm_stdq=%u rm_kq=%u dmmv_wg=%s submit_mflop=%s\\n",
                      (int) device->architecture, subgroup_size, device->subgroup_size,
                      rm_stdq, rm_kq,
                      std::getenv("GGML_VK_PS5_DMMV_WG") ? std::getenv("GGML_VK_PS5_DMMV_WG") : "auto",
                      std::getenv("GGML_VK_PS5_SUBMIT_MFLOP") ? std::getenv("GGML_VK_PS5_SUBMIT_MFLOP") : "auto");
    }
""",
)
replace(
    "        return ctx->device->pipeline_dequant_mul_mat_vec_q8_1_f32[dmmv_wg][a_type][num_cols-1];",
    """        uint32_t profile_wg = 0;
        if (prospero_vk_read_u32("GGML_VK_PS5_DMMV_WG", 0, 1, &profile_wg))
            dmmv_wg = profile_wg == 0 ? DMMV_WG_SIZE_SUBGROUP : DMMV_WG_SIZE_LARGE;
        return ctx->device->pipeline_dequant_mul_mat_vec_q8_1_f32[dmmv_wg][a_type][num_cols-1];""",
)
replace(
    "    return b_type == GGML_TYPE_F32 ? ctx->device->pipeline_dequant_mul_mat_vec_f32_f32[dmmv_wg][a_type][num_cols-1] : ctx->device->pipeline_dequant_mul_mat_vec_f16_f32[dmmv_wg][a_type][num_cols-1];",
    """    uint32_t profile_wg = 0;
    if (prospero_vk_read_u32("GGML_VK_PS5_DMMV_WG", 0, 1, &profile_wg))
        dmmv_wg = profile_wg == 0 ? DMMV_WG_SIZE_SUBGROUP : DMMV_WG_SIZE_LARGE;
    return b_type == GGML_TYPE_F32 ? ctx->device->pipeline_dequant_mul_mat_vec_f32_f32[dmmv_wg][a_type][num_cols-1] : ctx->device->pipeline_dequant_mul_mat_vec_f16_f32[dmmv_wg][a_type][num_cols-1];""",
)
replace(
    "        return ctx->device->pipeline_dequant_mul_mat_vec_id_q8_1_f32[dmmv_wg][a_type];",
    """        uint32_t profile_wg = 0;
        if (prospero_vk_read_u32("GGML_VK_PS5_DMMV_WG", 0, 1, &profile_wg))
            dmmv_wg = profile_wg == 0 ? DMMV_WG_SIZE_SUBGROUP : DMMV_WG_SIZE_LARGE;
        return ctx->device->pipeline_dequant_mul_mat_vec_id_q8_1_f32[dmmv_wg][a_type];""",
)
replace(
    "    return ctx->device->pipeline_dequant_mul_mat_vec_id_f32[dmmv_wg][a_type];",
    """    uint32_t profile_wg = 0;
    if (prospero_vk_read_u32("GGML_VK_PS5_DMMV_WG", 0, 1, &profile_wg))
        dmmv_wg = profile_wg == 0 ? DMMV_WG_SIZE_SUBGROUP : DMMV_WG_SIZE_LARGE;
    return ctx->device->pipeline_dequant_mul_mat_vec_id_f32[dmmv_wg][a_type];""",
)
replace(
    "    uint64_t flops_per_submit = std::min(flops_cap, ctx->last_total_flops / 40u);\n",
    "    uint64_t flops_per_submit = prospero_vk_submit_flops(std::min(flops_cap, ctx->last_total_flops / 40u));\n",
)

write_text_if_changed(path, text)
print(f"{path}: PS5 Vulkan profile hooks applied")
