#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Port the narrow Prism Hadamard model path needed by Bonsai into upstream llama.cpp."""
from __future__ import annotations

import argparse
from pathlib import Path
from prepare_build import write_text_if_changed


def extract_block(text: str, marker: str) -> str:
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"source block not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"source block has no body: {marker}")
    depth = 0
    i = brace
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == '"': state = "string"
            elif ch == "'": state = "char"
            elif ch == "/" and nxt == "/": state = "line"
            elif ch == "/" and nxt == "*": state = "block"
            elif ch == "{": depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        elif state == "string" or state == "char":
            if ch == "\\": i += 1
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif state == "line":
            if ch == "\n": state = "code"
        elif state == "block" and ch == "*" and nxt == "/":
            state = "code"
            i += 1
        i += 1
    raise SystemExit(f"unterminated source block: {marker}")


def insert_once(path: Path, anchor: str, addition: str) -> None:
    text = path.read_text()
    if addition.strip() in text:
        return
    if anchor not in text:
        raise SystemExit(f"{path}: missing insertion anchor: {anchor}")
    write_text_if_changed(path, text.replace(anchor, addition + anchor, 1))


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f"{path}: expected one replacement anchor, got {text.count(old)}: {old[:100]!r}")
    write_text_if_changed(path, text.replace(old, new, 1))


def prepare(llama: Path, prism: Path) -> None:
    upstream = llama / "src"
    prism_src = prism / "src"

    # The model metadata and its post-load transform buffers are copied from the
    # pinned Prism model loader. The rest of the upstream loader stays untouched.
    p_model = (prism_src / "llama-model.cpp").read_text()
    u_model = upstream / "llama-model.cpp"
    metadata = p_model[p_model.index("    uint32_t hadamard_version = 0;"):p_model.index("    // get general kv", p_model.index("    uint32_t hadamard_version = 0;"))]
    metadata = metadata.replace("            case LLM_ARCH_DSPARK:\n", "")
    metadata = metadata.replace(" || (arch == LLM_ARCH_DSPARK && weight_name != \"output.weight\")", "")
    metadata = metadata.replace(" || arch == LLM_ARCH_DSPARK", "")
    if "uint32_t hadamard_version = 0;" not in u_model.read_text():
        insert_once(u_model, "    // get general kv\n", metadata)
    model_buffers = extract_block(p_model, "    if (!hadamard_weight_blocks.empty() || !hadamard_inverse_blocks.empty())")
    insert_once(u_model, "    return true;\n}\n\nggml_tensor * llama_model_base::create_tensor", model_buffers + "\n\n")

    p_graph_h = (prism_src / "llama-graph.h").read_text()
    u_graph_h = upstream / "llama-graph.h"
    graph_types = p_graph_h[p_graph_h.index("// Maps a folded model weight"):p_graph_h.index("struct llama_cparams;")]
    insert_once(u_graph_h, "struct llama_cparams;\n", graph_types)
    insert_once(u_graph_h, "    const llama_prec_policy * prec_policy = nullptr;\n",
                "    const llama_hadamard_rotations * hadamard_rotations = nullptr;\n"
                "    const llama_hadamard_rotations * hadamard_inverses = nullptr;\n\n")
    insert_once(u_graph_h, "    const llama_prec_policy * prec_policy;\n",
                "    const llama_hadamard_rotations * hadamard_rotations;\n"
                "    const llama_hadamard_rotations * hadamard_inverses;\n"
                "    mutable std::map<std::pair<const ggml_tensor *, const ggml_tensor *>, ggml_tensor *> hadamard_memo;\n\n")
    insert_once(u_graph_h, "    ggml_tensor * build_norm(\n",
                "    ggml_tensor * build_embd_rows(ggml_tensor * tok_embd, ggml_tensor * ids) const;\n\n")

    p_model_h = (prism_src / "llama-model.h").read_text()
    u_model_h = upstream / "llama-model.h"
    model_fields = p_model_h[p_model_h.index("    // Hadamard-folded GGUF weights"):p_model_h.index("    // list of devices", p_model_h.index("    // Hadamard-folded GGUF weights"))]
    model_anchor = "    std::unordered_map<std::string, std::string> gguf_kv;\n\n"
    replace_once(u_model_h, model_anchor, model_anchor + model_fields)

    p_context_h = (prism_src / "llama-context.h").read_text()
    u_context_h = upstream / "llama-context.h"
    context_fields = p_context_h[p_context_h.index("    // the Hadamard transforms this context's graphs consult"):p_context_h.index("    // host buffer for the model output", p_context_h.index("    // the Hadamard transforms this context's graphs consult"))]
    context_anchor = "    llm_graph_result_ptr gf_res_reserve;\n\n"
    replace_once(u_context_h, context_anchor, context_anchor + context_fields)

    # Upstream only explicitly instantiates metadata accessors for enum keys;
    # Prism's Hadamard metadata uses literal GGUF keys.
    u_loader = upstream / "llama-model-loader.cpp"
    insert_once(u_loader,
                "    template bool llama_model_loader::get_arr<std::vector<uint32_t>>(enum llm_kv kid, std::vector<uint32_t> & result, bool required);\n",
                "    template bool llama_model_loader::get_arr<std::string>(const std::string & key, std::vector<std::string> & result, bool required);\n"
                "    template bool llama_model_loader::get_arr<int32_t>(const std::string & key, std::vector<int32_t> & result, bool required);\n")
    insert_once(u_loader,
                "    template bool llama_model_loader::get_key<std::string>(enum llm_kv kid, std::string & result, bool required);\n",
                "    template bool llama_model_loader::get_key<bool>(const std::string & key, bool & result, bool required);\n")

    # Feed model-owned transform tensors to every upstream graph. Matmul, expert
    # matmul-id and token lookup all use the same common graph builders.
    u_graph = upstream / "llama-graph.cpp"
    initializer = "    prec_policy      (params.prec_policy),\n"
    insert_once(u_graph, initializer,
                "    hadamard_rotations(params.hadamard_rotations),\n"
                "    hadamard_inverses (params.hadamard_inverses),\n")
    mm_start = "ggml_tensor * llm_graph_context::build_lora_mm("
    mm_fn = extract_block(u_graph.read_text(), mm_start)
    mm_insertion = """    ggml_tensor * cur_mm = cur;
    if (hadamard_rotations) {
        const auto it = hadamard_rotations->find(w);
        if (it != hadamard_rotations->end()) {
            const auto & t = it->second;
            const auto memo_key = std::make_pair((const ggml_tensor *) cur, (const ggml_tensor *) t.rot);
            const auto memo_it = hadamard_memo.find(memo_key);
            if (memo_it != hadamard_memo.end()) {
                cur_mm = memo_it->second;
            } else {
                if (t.perm_rep > 1) {
                    ggml_tensor * x = ggml_is_contiguous(cur_mm) ? cur_mm : ggml_cont(ctx0, cur_mm);
                    const int64_t ne1 = x->ne[1], ne2 = x->ne[2], ne3 = x->ne[3];
                    x = ggml_reshape_4d(ctx0, x, t.perm_hd, t.perm_nk, t.perm_rep, ne1*ne2*ne3);
                    x = ggml_cont(ctx0, ggml_permute(ctx0, x, 0, 2, 1, 3));
                    cur_mm = ggml_reshape_4d(ctx0, x, t.perm_hd*t.perm_nk*t.perm_rep, ne1, ne2, ne3);
                }
                if (t.signs) cur_mm = ggml_mul(ctx0, cur_mm, t.signs);
                cur_mm = llama_mul_mat_hadamard(ctx0, cur_mm, t.rot);
                hadamard_memo[memo_key] = cur_mm;
            }
        }
    }
"""
    if "ggml_tensor * cur_mm = cur;" not in mm_fn:
        mm_fn_new = mm_fn.replace("\n    ggml_tensor * res = ggml_mul_mat(ctx0, w, cur);\n",
                                  "\n" + mm_insertion + "\n    ggml_tensor * res = ggml_mul_mat(ctx0, w, cur_mm);\n")
        if mm_fn_new == mm_fn:
            raise SystemExit("upstream build_lora_mm insertion failed")
        replace_once(u_graph, mm_fn, mm_fn_new)

    id_start = "ggml_tensor * llm_graph_context::build_lora_mm_id("
    id_fn = extract_block(u_graph.read_text(), id_start)
    if "ggml_tensor * cur_mm = cur;" not in id_fn:
        id_fn_new = id_fn.replace("\n    ggml_tensor * res = ggml_mul_mat_id(ctx0, w, cur, ids);\n",
                                  "\n" + mm_insertion + "\n    ggml_tensor * res = ggml_mul_mat_id(ctx0, w, cur_mm, ids);\n")
        if id_fn_new == id_fn:
            raise SystemExit("upstream build_lora_mm_id insertion failed")
        replace_once(u_graph, id_fn, id_fn_new)

    embd_fn = """ggml_tensor * llm_graph_context::build_embd_rows(ggml_tensor * tok_embd, ggml_tensor * ids) const {
    ggml_tensor * cur = ggml_get_rows(ctx0, tok_embd, ids);
    if (hadamard_inverses) {
        const auto it = hadamard_inverses->find(tok_embd);
        if (it != hadamard_inverses->end()) {
            cur = llama_mul_mat_hadamard(ctx0, cur, it->second.rot);
            if (it->second.signs) cur = ggml_mul(ctx0, cur, it->second.signs);
        }
    }
    return cur;
}

"""
    insert_once(u_graph, "ggml_tensor * llm_graph_context::build_inp_embd(ggml_tensor * tok_embd) const {\n", embd_fn)
    replace_once(u_graph,
                 "cur = ggml_get_rows(ctx0, tok_embd, inp->tokens);",
                 "cur = build_embd_rows(tok_embd, inp->tokens);")

    # The upstream context stores maps keyed by its model tensors, passes them into
    # the graph and checks the first pristine graph before scheduling/copy insertion.
    p_context = (prism_src / "llama-context.cpp").read_text()
    u_context = upstream / "llama-context.cpp"
    verifier = extract_block(p_context, "static void llama_verify_hadamard_graph(")
    insert_once(u_context, "llama_context::llama_context(\n", verifier + "\n\n")
    insert_once(u_context, "    LLAMA_LOG_INFO(\"%s: constructing llama_context\\n\", __func__);\n",
                "    hadamard_rotations = model.hadamard_rotations;\n"
                "    hadamard_inverses = model.hadamard_inverses;\n\n")
    params_anchor = "        /*.prec_policy =*/ &model.prec_policy,\n"
    insert_once(u_context, params_anchor,
                "        /*.hadamard_rotations =*/ &hadamard_rotations,\n"
                "        /*.hadamard_inverses  =*/ &hadamard_inverses,\n")
    reserve_old = """    auto * gf = model.build_graph(gparams);

    this->n_input_tensors = llama_graph_n_input_tensors(gf);
"""
    reserve_new = """    auto * gf = model.build_graph(gparams);

    if (!hadamard_verified && gf && (!hadamard_rotations.empty() || !hadamard_inverses.empty())) {
        llama_verify_hadamard_graph(gf, hadamard_rotations, hadamard_inverses);
        hadamard_verified = true;
    }

    this->n_input_tensors = llama_graph_n_input_tensors(gf);
"""
    replace_once(u_context, reserve_old, reserve_new)

    # Keep this narrow adaptation reproducible after deps are fetched.
    print("Prepared upstream llama.cpp Bonsai Hadamard model path")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llama-source", type=Path, required=True)
    parser.add_argument("--prism-source", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.llama_source, args.prism_source)


if __name__ == "__main__":
    main()
