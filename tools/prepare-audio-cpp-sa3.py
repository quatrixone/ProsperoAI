#!/usr/bin/env python3
"""Apply guarded source compatibility edits to a disposable pinned audio.cpp checkout."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path
from prepare_build import write_text_if_changed

PIN = "886076fc89c09b9902592aa4bfbd12922d4a9e26"
ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    source = path.read_text()
    # Some PS5 adapters extend a line that remains inside the replacement. In
    # that case the old fragment is still present after patching, so recognize
    # the complete replacement before applying it again. For disjoint patches,
    # wait until the old fragment is gone; the replacement may be common text.
    if old in new and new in source:
        return
    if old not in source:
        if not new or new in source:
            return
    if source.count(old) != 1:
        raise SystemExit(f"{path}: expected one known patch site; inspect source changes")
    write_text_if_changed(path, source.replace(old, new, 1))


def adapt_weak_canonical(path: Path) -> None:
    text = path.read_text()
    helper_block = re.compile(
        r"namespace \{\nstd::filesystem::path ps5_weakly_canonical\("
        r"const std::filesystem::path & path\) \{.*?\n\}\n\} // namespace\n\n",
        re.S,
    )
    text = helper_block.sub("", text)
    text = text.replace("std::filesystem::weakly_canonical(", "ps5_weakly_canonical(")
    if "ps5_weakly_canonical(" not in text:
        return
    marker = "namespace engine::"
    at = text.find(marker)
    if at < 0:
        raise SystemExit(f"{path}: namespace insertion point for filesystem adapter not found")
    helper = '''namespace {
std::filesystem::path ps5_weakly_canonical(const std::filesystem::path & path) {
#if defined(__FreeBSD__)
    return path.lexically_normal();
#else
    return std::filesystem::weakly_canonical(path);
#endif
}
} // namespace

'''
    write_text_if_changed(path, text[:at] + helper + text[at:])


def omit_cmake_sources(path: Path) -> None:
    excluded = {
        "src/framework/codecs/wan_video_vae_runtime.cpp",
        "src/framework/modules/vocoders/bigvgan_vocoder.cpp",
        "src/framework/modules/optimizations/fast_conv_modules.cpp",
        "src/framework/modules/optimizations/fast_projection_modules.cpp",
        "src/framework/modules/attention/common_relative_attention.cpp",
        "src/framework/modules/speaker_encoders/ecapa_tdnn_runtime.cpp",
        "src/framework/modules/speaker_encoders/titanet_runtime.cpp",
        "src/framework/modules/transformers/causal_decoder.cpp",
        "src/framework/modules/transformers/causal_decoder_runtime.cpp",
        "src/framework/modules/transformers/qwen35_decoder_runtime.cpp",
        "src/framework/modules/text_encoders/qwen3_vl_encoder_runtime.cpp",
        "src/framework/modules/speech_encoders/qwen3_audio_encoder_runtime.cpp",
        "src/framework/modules/transformers/gemma_decoder.cpp",
        "src/framework/tokenizers/hf_tokenizer_json.cpp",
        "src/framework/tokenizers/qwen_bpe_bundle.cpp",
        "$<TARGET_OBJECTS:engine_model_silero_vad>",
        "$<TARGET_OBJECTS:engine_model_marblenet_vad>",
    }
    lines = path.read_text().splitlines(keepends=True)
    for entry in excluded:
        matches = [i for i, line in enumerate(lines) if line.strip() == entry]
        if len(matches) > 1:
            raise SystemExit(f"{path}: duplicate CMake entry for {entry}")
        if matches:
            del lines[matches[0]]
    write_text_if_changed(path, "".join(lines))


def include_sa3_dependencies(path: Path) -> None:
    entries = (
        "src/framework/modules/text_encoders/t5_base_encoder.cpp",
        "src/framework/modules/text_encoders/t5_gemma_encoder.cpp",
    )
    source = path.read_text()
    present = [entry in source for entry in entries]
    if all(present):
        return
    if any(present):
        raise SystemExit(f"{path}: partial Stable Audio text-encoder source selection")
    anchor = "    src/framework/modules/positional_modules.cpp\n"
    if source.count(anchor) != 1:
        raise SystemExit(f"{path}: expected one source insertion point for Stable Audio encoders")
    insertion = anchor + "".join(f"    {entry}\n" for entry in entries)
    write_text_if_changed(path, source.replace(anchor, insertion, 1))


def include_ace_step_dependencies(path: Path) -> None:
    """Restore shared runtime pieces omitted from the smaller Stable Audio build."""
    source = path.read_text()
    required = {
        "src/framework/modules/transformers/decoder.cpp":
            "    src/framework/modules/transformers/transformer_blocks.cpp\n",
        "src/framework/modules/optimizations/fast_kv_modules.cpp":
            "    src/framework/modules/structural_modules.cpp\n",
        "src/framework/tokenizers/llama_bpe.cpp":
            "    src/framework/tokenizers/sentencepiece.cpp\n",
        "external/llama_tokenizer/bpe-core.cpp":
            "    src/framework/tokenizers/llama_bpe.cpp\n",
        "external/llama_tokenizer/unicode.cpp":
            "    external/llama_tokenizer/bpe-core.cpp\n",
        "external/llama_tokenizer/unicode-data.cpp":
            "    external/llama_tokenizer/unicode.cpp\n",
    }
    for entry, anchor in required.items():
        line = f"    {entry}\n"
        if line in source:
            continue
        if source.count(anchor) != 1:
            raise SystemExit(f"{path}: cannot restore ACE-Step dependency {entry}")
        source = source.replace(anchor, anchor + line, 1)
    write_text_if_changed(path, source)


def reuse_parent_ggml(path: Path) -> None:
    """Reuse the parent runtime's upstream GGML target when added as a subproject."""
    guarded = '''if (NOT TARGET ggml)
    add_subdirectory("${AUDIOCPP_GGML_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ggml")
endif()'''
    if guarded in path.read_text():
        return
    replace_once(
        path,
        'add_subdirectory("${AUDIOCPP_GGML_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ggml")',
        guarded,
    )


def adapt_ace_step_ggml_round(source: Path) -> None:
    replace_once(
        source / "src/framework/modules/transformers/decoder.cpp",
        "return core::wrap_tensor(ggml_round_bf16(ctx.ggml, input.tensor), input.shape, GGML_TYPE_F32);",
        "return core::wrap_tensor(ggml_cast(ctx.ggml, ggml_cast(ctx.ggml, input.tensor, GGML_TYPE_BF16), GGML_TYPE_F32), input.shape, GGML_TYPE_F32);",
    )


def adapt_ace_step_staged_memory(source: Path) -> None:
    """Load ACE-Step stages on demand and drop each one when it is done (mem_saver).

    Upstream prepare() loads the planner LM, pre-DiT/text encoder, DiT and VAE together,
    a 7.4 GiB Vulkan peak that, with the CPU side, filled the PS5 pool: the second request
    or one after another model failed to allocate. The adapter rebuilds the session for
    every request anyway, so each stage is loaded by run() and released after use.
    """
    session = source / "src/models/ace_step/session.cpp"
    replace_once(session,
        "void AceStepSession::prepare(const runtime::SessionPreparationRequest &request) {\n    ensure_planner();",
        "void AceStepSession::prepare(const runtime::SessionPreparationRequest &request) {\n"
        "    if (mem_saver_) { // PS5: run() loads each stage on demand\n        mark_prepared();\n        return;\n    }\n"
        "    ensure_planner();")
    # Both planner paths (caption rewrite and generation) drop the planner, as on Metal.
    planner_old = "        planner_->release_graph_workspace();\n        if (execution_context().backend_type() == core::BackendType::Metal) {"
    planner_new = "        planner_->release_graph_workspace();\n        if (mem_saver_ || execution_context().backend_type() == core::BackendType::Metal) {"
    text = session.read_text()
    if text.count(planner_new) != 2:
        if text.count(planner_old) != 2:
            raise SystemExit(f"{session}: expected two ACE-Step planner release sites")
        write_text_if_changed(session, text.replace(planner_old, planner_new))
    replace_once(session,
        "        pre_dit_->release_runtime_graphs();\n",
        "        pre_dit_->release_runtime_graphs();\n        pre_dit_.reset(); // PS5: free the text encoder before diffusion\n")
    replace_once(session,
        "    diffusion_->release_graph_workspace();\n",
        "    diffusion_->release_graph_workspace();\n"
        "    if (mem_saver_) { // PS5: free the DiT before the VAE decoder loads\n"
        "        diffusion_.reset();\n        dit_weights_runtime_.reset();\n    }\n")


def ggml_has_api(ggml_source: Path, *names: str) -> bool:
    headers = list((ggml_source / "include").glob("ggml*.h"))
    text = "\n".join(header.read_text(errors="replace") for header in headers)
    return all(name in text for name in names)


def adapt_chatterbox_s3_tokenizer_arena(source: Path) -> None:
    """Keep Chatterbox's fixed graph metadata arenas within PS5 host allocation limits."""
    path = source / "src/models/chatterbox/s3_tokenizer_impl.cpp"
    replace_once(
        path,
        "        params.mem_size = 512ull * 1024ull * 1024ull;",
        "        params.mem_size = 64ull * 1024ull * 1024ull; // PS5: graph metadata only",
    )
    replace_once(
        path,
        "        768ull * 1024ull * 1024ull);",
        "        64ull * 1024ull * 1024ull); // PS5: tensor metadata only",
    )
    t3_runner = source / "src/models/chatterbox/components/t3_runtime.h"
    # Older local preparations used oversized metadata-only arenas. Tensor
    # payloads are backend-allocated; the 131072-node prefill graph needs
    # roughly 8 MiB for its graph tables, not hundreds of MiB of host heap.
    text = t3_runner.read_text()
    text = text.replace(
        "params.mem_size = 96ull * 1024ull * 1024ull; // PS5: graph metadata only",
        "params.mem_size = 256ull * 1024ull * 1024ull;",
    ).replace(
        "params.mem_size = 128ull * 1024ull * 1024ull; // PS5: graph metadata only",
        "params.mem_size = 768ull * 1024ull * 1024ull;",
    )
    write_text_if_changed(t3_runner, text)
    replace_once(
        t3_runner,
        "        params.mem_size = 256ull * 1024ull * 1024ull;",
        "        params.mem_size = 32ull * 1024ull * 1024ull; // PS5: decode graph metadata only",
    )
    replace_once(
        t3_runner,
        "        params.mem_size = 768ull * 1024ull * 1024ull;",
        "        params.mem_size = 32ull * 1024ull * 1024ull; // PS5: prefill graph metadata only",
    )
    # T3 decode attention: explicit F32 QK^T/softmax/V instead of upstream's
    # ggml_flash_attn_ext. Both are correct once the runner buffers are cleared
    # (below); on the PS5 the explicit form decoded a 19-word sentence in about
    # 3.1 s versus 4.4-5.2 s with flash attention (2026-10-14).
    replace_once(t3_runner,
        '    auto * flash = ggml_flash_attn_ext(\n        ctx.ggml,\n        q_heads.tensor,\n        all_k_heads.tensor,\n        all_v_heads.tensor,\n        attention_mask.tensor,\n        1.0f / std::sqrt(static_cast<float>(head_dim)),\n        0.0f,\n        0.0f);\n    ggml_flash_attn_ext_set_prec(flash, GGML_PREC_F32);\n    auto context = core::wrap_tensor(\n        flash,\n        core::TensorShape::from_dims({q_heads.shape.dims[0], q_heads.shape.dims[2], q_heads.shape.dims[1], head_dim}),\n        GGML_TYPE_F32);\n    context = contiguous(ctx, context);',
        '    // PS5: match the reference attention with explicit F32 scores/softmax.\n    auto k_transposed = modules::TransposeModule({{0, 1, 3, 2}, 4}).build(ctx, all_k_heads);\n    auto scores = modules::MatMulModule().build(ctx, q_heads, k_transposed);\n    scores = core::wrap_tensor(ggml_scale(ctx.ggml,scores.tensor,1.0f / std::sqrt(static_cast<float>(head_dim))),scores.shape,GGML_TYPE_F32);\n    auto mask = core::wrap_tensor(ggml_cast(ctx.ggml,attention_mask.tensor,GGML_TYPE_F32),attention_mask.shape,GGML_TYPE_F32);\n    scores = add_contiguous(ctx,scores,mask);\n    auto attn = core::wrap_tensor(ggml_soft_max(ctx.ggml,contiguous(ctx,scores).tensor),scores.shape,GGML_TYPE_F32);\n    auto context = modules::MatMulModule().build(ctx,attn,all_v_heads);\n    context = modules::TransposeModule({{0, 2, 1, 3}, 4}).build(ctx,context);\n    context = contiguous(ctx, context);')
    # Vulkan flash attention reads the full capacity. Masked unused values must
    # be zero: stale NaNs survive multiplication by zero in the attention kernel.
    for label in ("batch_key", "batch_value"):
        replace_once(t3_runner,
            f"std::vector<float> {label}(static_cast<size_t>(cache_state_.steps * hidden_size_), 0.0f);",
            f"std::vector<float> {label}(static_cast<size_t>(cache_steps_ * hidden_size_), 0.0f); // PS5: initialize the masked KV tail")
    t3_component = source / "src/models/chatterbox/t3_component.cpp"
    replace_once(t3_component, '#include <chrono>',
        '#include <chrono>\n#include <cmath>\n#include <cstdio>\n#if defined(PROSPERO_PS5_MODEL_IO)\nextern "C" int sceKernelDebugOutText(int, const char *);\n#endif')
    # Narrow PS5 diagnostic: distinguish immediate EOS from non-finite logits.
    replace_once(t3_component,
        "        generated_ids.push_back(next_token);",
        """        generated_ids.push_back(next_token);
#if defined(PROSPERO_PS5_MODEL_IO)
        if (step < 4 || next_token == kStopSpeechToken) {

            size_t invalid = 0;
            for (float value : logits) if (!std::isfinite(value)) ++invalid;
            char trace[192];
            std::snprintf(trace,sizeof(trace),"[prosperoai] T3 step=%lld token=%d masked_or_invalid_logits=%zu\\n", static_cast<long long>(step),next_token,invalid);
            sceKernelDebugOutText(0,trace);
        }
#endif""")

    replace_once(
        t3_component,
        "    constexpr int64_t kDecodeCapacityChunk = 256;",
        "    constexpr int64_t kDecodeCapacityChunk = 32; // PS5: bound the decode cache allocation",
    )
    replace_once(
        t3_runner,
        '                throw std::runtime_error("failed to allocate backend tensors for T3 decode runner");',
        '''                size_t requested_bytes = 0;
                for (ggml_tensor * tensor = ggml_get_first_tensor(ggml_); tensor != nullptr;
                     tensor = ggml_get_next_tensor(ggml_, tensor)) {
                    requested_bytes += ggml_nbytes(tensor);
                }
                throw std::runtime_error("failed to allocate backend tensors for T3 decode runner (" +
                    std::to_string(requested_bytes / (1024 * 1024)) + " MiB)");''',
    )
    # The T3 runners read KV cache slots that are masked but never written (the prefix
    # pass imports an empty cache). PS5 direct memory is not zeroed when reused, so after
    # another runtime (llama.cpp) ran in the process those slots held stale data, and
    # 0 * NaN from masked attention made every decode logit NaN. Clear new runner buffers.
    plan = "            engine::core::prepare_host_graph_plan(owner_->execution_context(), graph_, cpu_plan_);\n"
    cleared = ("            ggml_backend_buffer_clear(buffer_, 0); // PS5: reused direct memory is not zeroed\n"
               + plan)
    text = t3_runner.read_text()
    if text.count(cleared) != 2:
        if text.count(plan) != 2:
            raise SystemExit(f"{t3_runner}: expected two T3 runner buffer allocations")
        write_text_if_changed(t3_runner, text.replace(plan, cleared))
    # S3 retains attention and feed-forward runners for every encoder layer.
    # Their no_alloc contexts contain metadata only, but upstream reserves
    # 128/192 MiB per runner, exhausting the host heap before audio decoding.
    s3_runtime = source / "src/framework/codecs/s3gen_runtime.cpp"
    text = s3_runtime.read_text()
    for arena_mb, expected_count in ((192, 3), (128, 1), (512, 1)):
        old = f"params.mem_size = {arena_mb}ull * 1024ull * 1024ull;"
        previous = (f"params.mem_size = 32ull * 1024ull * 1024ull; "
               f"// PS5: S3 metadata arena (upstream {arena_mb} MiB)")
        size = 32 if arena_mb == 512 else 8
        new = (f"params.mem_size = {size}ull * 1024ull * 1024ull; "
               f"// PS5: S3 metadata arena (upstream {arena_mb} MiB)")
        if text.count(new) == expected_count:
            continue
        text = text.replace(previous,old)
        if text.count(old) != expected_count:
            raise SystemExit(f"{s3_runtime}: unexpected S3 {arena_mb} MiB arena count")
        text = text.replace(old, new)
    write_text_if_changed(s3_runtime, text)
    hift = source / "src/framework/modules/vocoders/hift_vocoder.cpp"
    replace_once(
        hift,
        "ggml_init_params params{128ull * 1024ull * 1024ull, nullptr, true};",
        "ggml_init_params params{32ull * 1024ull * 1024ull, nullptr, true}; // PS5: F0 metadata only",
    )
    replace_once(
        hift,
        "const size_t context_size = static_cast<size_t>(kBaseBytes + clamped_frames * kPerFrameBytes);",
        "const size_t context_size = 32ull * 1024ull * 1024ull; // PS5: HiFT metadata only; payloads use backend buffers",
    )
    # BackendWeightStore also uses no_alloc=true. Its GiB-sized contexts are
    # tensor metadata reservations, not model payloads or Vulkan workspaces.
    t3_weights = source / "src/models/chatterbox/t3_weights.cpp"
    for size in (1024,4096):
        replace_once(t3_weights,
            f"constexpr size_t kDefaultWeightStoreBytes = {size}ull * 1024ull * 1024ull;",
            f"constexpr size_t kDefaultWeightStoreBytes = 32ull * 1024ull * 1024ull; // PS5: T3 metadata (upstream {size} MiB)")
    for label,size in (("s3gen.encoder.weights",1024),("s3gen.decoder.weights",2048)):
        replace_once(s3_runtime,
            f'"{label}",\n        {size}ull * 1024ull * 1024ull);',
            f'"{label}",\n        32ull * 1024ull * 1024ull); // PS5: weight tensor metadata only')
    replace_once(hift,
        '"framework.hift.weights",\n        768ull * 1024ull * 1024ull);',
        '"framework.hift.weights",\n        32ull * 1024ull * 1024ull); // PS5: weight tensor metadata only')


def adapt_gguf_streaming(path: Path) -> None:
    """Keep PS5 model inspection metadata-only; never map/copy the whole checkpoint."""
    source = path.read_text()
    marker = "// ProsperoAI: bounded GGUF tensor reads"
    if marker in source:
        new_budget = "constexpr size_t max_tensor_bytes = 1024ull * 1024 * 1024;"
        if new_budget in source:
            return
        old_budgets = (
            "constexpr size_t max_tensor_bytes = 256ull * 1024 * 1024;",
            "constexpr size_t max_tensor_bytes = 512ull * 1024 * 1024;",
        )
        old_budget = next((budget for budget in old_budgets if source.count(budget) == 1), None)
        if old_budget is None:
            raise SystemExit(f"{path}: expected one known GGUF read budget")
        write_text_if_changed(path, source.replace(old_budget, new_budget, 1))
        return
    begin = source.index("class GgufTensorSource final : public TensorSource {")
    end = source.index("\nstd::unordered_map<std::string, std::string> parse_indexed_tensor_weight_map", begin)
    section = source[begin:end]
    def patch(old: str, new: str) -> None:
        nonlocal section
        if section.count(old) != 1:
            raise SystemExit(f"{path}: GGUF streaming patch site changed")
        section = section.replace(old, new, 1)
    patch("        bytes_ = engine::io::read_binary_blob(source_path_);", """#if defined(__FreeBSD__) || defined(AUDIOCPP_PS5_STREAM_GGUF)
        file_size_ = std::filesystem::file_size(source_path_);
#else
        bytes_ = engine::io::read_binary_blob(source_path_);
#endif""")
    patch("        bytes_ = engine::io::BinaryBlob();", """        bytes_ = engine::io::BinaryBlob();
        std::vector<std::byte>().swap(tensor_bytes_);""")
    patch("""        if (bytes_.empty()) bytes_ = engine::io::read_binary_blob(source_path_);
        const size_t offset = data_begin_ + info.data_offset;""", """        // ProsperoAI: bounded GGUF tensor reads
#if defined(__FreeBSD__) || defined(AUDIOCPP_PS5_STREAM_GGUF)
        constexpr size_t max_tensor_bytes = 1024ull * 1024 * 1024;
        if (data_begin_ > file_size_ || info.data_offset > file_size_ - data_begin_)
            throw std::runtime_error("GGUF tensor offset is out of bounds: " + info.logical_name);
        const size_t offset = data_begin_ + info.data_offset;
        if (info.byte_size > file_size_ - offset || info.byte_size > max_tensor_bytes)
            throw std::runtime_error("GGUF tensor exceeds PS5 bounded read budget: " + info.logical_name);
        FILE * file = std::fopen(source_path_.string().c_str(), "rb");
        if (file == nullptr)
            throw std::runtime_error("failed to open GGUF tensor: " + info.logical_name);
        try {
            if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0)
                throw std::runtime_error("failed to seek GGUF tensor: " + info.logical_name);
            tensor_bytes_.resize(info.byte_size);
            size_t done = 0;
            while (done < info.byte_size) {
                const size_t chunk = std::min<size_t>(1024 * 1024, info.byte_size - done);
                if (std::fread(tensor_bytes_.data() + done, 1, chunk, file) != chunk)
                    throw std::runtime_error("short read of GGUF tensor: " + info.logical_name);
                done += chunk;
            }
        } catch (...) {
            std::fclose(file);
            throw;
        }
        std::fclose(file);
        return {tensor_bytes_.data(), info.byte_size};
#else
        if (bytes_.empty()) bytes_ = engine::io::read_binary_blob(source_path_);
        const size_t offset = data_begin_ + info.data_offset;""")
    patch("        return {bytes_.data() + static_cast<std::ptrdiff_t>(offset), info.byte_size};", """        return {bytes_.data() + static_cast<std::ptrdiff_t>(offset), info.byte_size};
#endif""")
    patch("    mutable engine::io::BinaryBlob bytes_;", """    size_t file_size_ = 0;
    mutable std::vector<std::byte> tensor_bytes_;
    mutable engine::io::BinaryBlob bytes_;""")
    write_text_if_changed(path, source[:begin] + section + source[end:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "build/audio-cpp-upstream")
    parser.add_argument("--ggml-source", type=Path, default=ROOT / "build/prism-llama-source/ggml")
    args = parser.parse_args()
    source = args.source.resolve()
    ggml_source = args.ggml_source.resolve()
    if not (source / "CMakeLists.txt").is_file():
        raise SystemExit(f"{source}: missing audio.cpp CMakeLists.txt")
    revision = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if revision != PIN:
        raise SystemExit(f"{source}: expected {PIN}, found {revision}")

    cmake = source / "CMakeLists.txt"
    omit_cmake_sources(cmake)
    include_sa3_dependencies(cmake)
    include_ace_step_dependencies(cmake)
    reuse_parent_ggml(cmake)
    if not ggml_has_api(ggml_source, "ggml_round_bf16"):
        adapt_ace_step_ggml_round(source)
    adapt_ace_step_staged_memory(source)
    adapt_chatterbox_s3_tokenizer_arena(source)
    replace_once(
        cmake,
        "add_library(audiocpp SHARED src/capi/audiocpp.cpp)",
        "add_library(audiocpp STATIC src/capi/audiocpp.cpp)",
    )
    # audio.cpp's public API and model loader are called under ProsperoAI's
    # single inference mutex. Avoid C++ TLS destructors on the PS5 HTTP worker:
    # each API request runs on a short-lived ScePthread and TLS teardown after
    # sending a successful audio response crashes the title on FW 12.70.
    replace_once(
        source / "src/capi/audiocpp.cpp",
        "thread_local std::string g_last_error;",
        "static std::string g_last_error; // PS5: calls are serialized by the app router",
    )
    replace_once(
        source / "src/framework/model_spec/package.cpp",
        """thread_local std::optional<std::filesystem::path> active_model_spec_override;
thread_local std::optional<std::filesystem::path> active_model_path;
thread_local bool active_embedded_spec_checked = false;
thread_local std::optional<assets::GgufEmbeddedModelSpec> active_embedded_spec;
thread_local std::unordered_set<std::string> legacy_embedded_contract_warnings;""",
        """// PS5: model loading is serialized by ProsperoAI's inference mutex; keep
// this cache out of short-lived HTTP worker TLS to avoid unsupported destructors.
static std::optional<std::filesystem::path> active_model_spec_override;
static std::optional<std::filesystem::path> active_model_path;
static bool active_embedded_spec_checked = false;
static std::optional<assets::GgufEmbeddedModelSpec> active_embedded_spec;
static std::unordered_set<std::string> legacy_embedded_contract_warnings;""",
    )
    if not ggml_has_api(ggml_source, "GGML_MUL_MAT_LOWERING_VULKAN_F32_INPUTS",
                        "ggml_mul_mat_set_lowering"):
        replace_once(
            source / "src/framework/modules/linear_module.cpp",
        """    if (config_.cuda_nvfp4_f16_activation_lowering) {
        ggml_mul_mat_set_lowering(projected_raw, GGML_MUL_MAT_LOWERING_CUDA_NVFP4_F16_ACTIVATION);
    }
    if (config_.vulkan_f32_activation_lowering) {
        ggml_mul_mat_set_lowering(projected_raw, GGML_MUL_MAT_LOWERING_VULKAN_F32_INPUTS);
    }""",
        "    // The pinned ProsperoAI ggml predates the optional backend-specific lowering hints.",
        )
    if not ggml_has_api(ggml_source, "GGML_CONCAT_LOWERING_CUDA_CONTIGUOUS_4D",
                        "ggml_concat_set_lowering"):
        replace_once(
            source / "src/framework/modules/structural_modules.cpp",
        """    if (config_.cuda_contiguous_4d_lowering) {
        ggml_concat_set_lowering(raw, GGML_CONCAT_LOWERING_CUDA_CONTIGUOUS_4D);
    }
""",
        "",
        )
    if not ggml_has_api(ggml_source, "ggml_mul_mat_acc"):
        replace_once(
            source / "src/framework/modules/conv_modules.cpp",
        """    // accumulate-in-place GEMM only where the tensor-core mm kernel applies
    // (mirrors the Metal supports gate); otherwise keep the mul_mat + add chain
    const bool use_acc = in_channels >= 64 && output_frames > 8;
""",
        "",
        )
        replace_once(
            source / "src/framework/modules/conv_modules.cpp",
        """        } else if (use_acc) {
            acc = ggml_mul_mat_acc(ctx.ggml, tap_weights, columns, acc);
        } else {""",
        "        } else {",
        )
    if not ggml_has_api(ggml_source, "ggml_rope_interleaved_pairs"):
        replace_once(
            source / "src/framework/modules/primitive_modules.cpp",
        """    return core::wrap_tensor(
        ggml_rope_interleaved_pairs(ctx.ggml, even.tensor, odd.tensor, cos.tensor, sin.tensor),
        output_shape,
        even.type);""",
        """    (void) output_shape;
    throw std::runtime_error("RopeInterleavedPairs is unavailable in the pinned ProsperoAI ggml");""",
        )
    for relative in (
        "src/framework/assets/tensor_source.cpp",
        "src/framework/model_spec/package.cpp",
        "src/framework/assets/resource_bundle.cpp",
        "src/framework/runtime/model.cpp",
        "src/framework/io/filesystem.cpp",
    ):
        adapt_weak_canonical(source / relative)
    tensor_source = source / "src/framework/assets/tensor_source.cpp"
    adapt_gguf_streaming(tensor_source)
    sidecar_root_patch = '''#if defined(__FreeBSD__)
    const auto root = std::filesystem::path("/download0/audiocpp-gguf") / fingerprint.str();
#else
    const auto root = std::filesystem::temp_directory_path() / "audiocpp-gguf" / fingerprint.str();
#endif'''
    tensor_text = tensor_source.read_text()
    if sidecar_root_patch not in tensor_text:
        if 'canonical.parent_path() / ".audiocpp-gguf"' in tensor_text:
            replace_once(
                tensor_source,
                'const auto root = canonical.parent_path() / ".audiocpp-gguf" / fingerprint.str();',
                sidecar_root_patch,
            )
        else:
            replace_once(
                tensor_source,
                'const auto root = std::filesystem::temp_directory_path() / "audiocpp-gguf" / fingerprint.str();',
                sidecar_root_patch,
            )
    registry = source / "src/framework/runtime/registry.cpp"
    replace_once(
        registry,
        "#include \"engine/models/marblenet_vad/session.h\"\n#include \"engine/models/silero_vad/session.h\"\n",
        "",
    )
    replace_once(
        registry,
        """        engine::models::silero_vad::make_silero_vad_loader(),
        engine::models::marblenet_vad::make_marblenet_vad_loader(),
""",
        "",
    )
    print(f"{source}: prepared narrow SA3 build at {PIN}")


if __name__ == "__main__":
    main()
