# PS5SD-verified Image Models Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Qwen Image with PS5SD-console-demonstrated diffusion presets, add model-aware image generation workflows, and deliver a PS5SD-inspired Image Studio on web only.

**Architecture:** Curated image preset metadata will declare provenance, runtime status, and supported workflows. A typed image-generation request will carry validated uploads and model-specific options through the existing Vulkan runtime. The web Image Studio will provide Create workflows and reuse ProsperoAI's existing Models and Media pages; the native UI keeps its current text-to-image flow.

**Tech Stack:** C++20, stable-diffusion.cpp and ggml Vulkan, the existing PS5 HTTP/API and model downloader, HTML/CSS/JavaScript, Python unittest, and Node.js `node:test`.

**Spec:** `docs/superpowers/specs/2026-11-01-ps5sd-verified-image-models-design.md`

## Global Constraints

- Add only the exact image variants listed in the approved spec; keep existing ProsperoAI-verified SD-Turbo FP16 and Realistic Vision v5.1 FP16.
- Preserve exact model source, revision, quantization, file size, SHA-256, required assets, and license terms; do not bundle weights.
- Label PS5SD evidence and ProsperoAI-on-console evidence separately; a failed ProsperoAI run stays available but marked experimental or blocked.
- Do not represent FLUX.2 Klein 9B as commercially usable; surface its non-commercial restriction.
- Remove Qwen Image 2.1 product support and Qwen-only upload/projector/VRAM/UI paths without deleting installed files or generated media; keep Qwen3 encoders and shared code needed by other models.
- Keep text-to-image on the existing native surface; do not add advanced image workflow controls to native UI.
- Web image inputs use authenticated, bounded server-owned uploads; never accept a client filesystem path.
- Use `PPSA99023` only for console testing; never change `PPSA99004`, reboot, or delete unrelated data.
- Keep implementation changes on PR #43 and close an issue only if this work passes the issue's acceptance criteria.

## Review Focus

- An unsupported image mode or missing model-specific input must be rejected before runtime selection or generation; exercise capability and request validation in Tasks 1 and 3.
- An invalid, corrupt, oversized, expired, or concurrent source/mask/guide upload must not reach inference or delete an unrelated upload; exercise upload lifecycle and ownership in Task 3.
- Switching image models must clear or revalidate staged controls and must not silently apply unsupported options; exercise mode/model transitions in Task 4.
- A generation or archive failure must leave user source media recoverable and must not corrupt a saved conversation or the gallery; exercise failure and retry paths in Tasks 3–4.
- High-resolution, SDXL/ControlNet, and FLUX allocation failures must be reported without title restart and must not be labelled ProsperoAI-verified; exercise memory refusal and model switching in Task 5.

---

### Task 1: Add curated model variants and workflow metadata

**Files:**
- Modify: `model-tools/presets.json`
- Modify: `tools/generate-model-presets.py`
- Regenerate: `include/model_presets.hpp`
- Modify: `include/gpt_runtime.hpp`, `inference/router.cpp`
- Modify: `vulkan/http_server.cpp`, `vulkan/openai_api.inc`
- Modify: `tools/deps.json` and `tools/vulkan-runtime/CMakeLists.txt` only for the dependency pin needed by FLUX.3 Image Klein
- Test: `tests/test_image_model_catalog.py`
- Test: `tests/vulkan_router_test.cpp`

**Interfaces:**
- Each image preset carries `image_capabilities`, `image_edit_strategy` (`init_image_strength` or `reference_image` when image-to-image is supported), and `image_defaults` (`width`, `height`, `steps`, `cfg_scale`, `sampler`, `scheduler`); generated `Preset` metadata exposes all three to the router and `/api/models/presets`.
- Add `gpt_runtime_image_defaults_t { unsigned width, height, steps; float cfg_scale; const char *sampler, *scheduler; }`, capability flags `GPT_IMAGE_CAP_TEXT_TO_IMAGE`, `GPT_IMAGE_CAP_IMAGE_TO_IMAGE`, `GPT_IMAGE_CAP_INPAINTING`, `GPT_IMAGE_CAP_CONTROLNET`, and `bool gpt_runtime_model_image_metadata(unsigned index, std::uint32_t *capabilities, gpt_runtime_image_defaults_t *defaults, const char **edit_strategy)`.
- `GET /v1/models` exposes each installed image model's `details.capabilities`, `details.image_edit_strategy`, and `details.image_defaults`; unlisted custom SD models default to text-to-image only with current runtime defaults.
- The stable-diffusion.cpp dependency is pinned to `b13a48219d5cdf408edcb93da6cd69b612ed03d3` only if its FLUX.3 loader and shared ggml ABI pass the host build. Record the archive SHA-256 generated from that exact commit.

- [ ] **Step 1: Add failing catalog tests for exact requested variants**

Add `test_image_model_catalog.py` to load `model-tools/presets.json` and assert the IDs, model names, quantizations, repositories/revisions, required files, licenses, and workflow capability entries for SDXS-512 tinySDdistilled Q8_0; SDXL Base Q8_0, Raehoshi IllustXL 1.1, and iLustMix v11.1; SDXL and SD 1.5 Inpainting Q8_0; SDXL Base plus xinsir Canny; FLUX.2 Klein 4B Q8_0 and 9B Q4_0; FLUX.3 Image Klein Q4_K and Q6_K; and Z-Image Turbo Q4_K. Also assert existing SD-Turbo and Realistic Vision presets remain and no Qwen Image preset is added.

Run: `python3 -m unittest discover -s tests -p 'test_image_model_catalog.py' -v`

Expected: FAIL because these presets and capability fields are not present yet.

- [ ] **Step 2: Add model entries using the PS5SD source manifest**

Use the exact model files, source URLs, revisions, checksums, and license details from the approved PS5SD snapshot. Add no weights to the repository. Mark text-to-image for every image model; mark image-to-image only for compatible SD and FLUX.2 Klein models, inpainting only for the two inpainting presets, and ControlNet only for the SDXL Canny preset. Set `image_edit_strategy` to `init_image_strength` only on SD presets that declare image-to-image and `reference_image` on FLUX.2 Klein. Copy each variant's width, height, steps, CFG, sampler, and scheduler defaults from that variant's tested `# defaults` entry; do not substitute PS5SD values for existing ProsperoAI-verified presets.

- [ ] **Step 3: Generate and expose the capability metadata**

Update `tools/generate-model-presets.py` and regenerate `include/model_presets.hpp`. Extend the runtime catalog response so the router and web can read capabilities and exact defaults without identifying behavior from model IDs or display names. Preserve existing `kind` and preset-download semantics.

- [ ] **Step 4: Pin and build FLUX.3 support against the shared ABI**

Confirm the specified stable-diffusion.cpp commit contains the FLUX.3 Image Klein loader and its API can compile with ProsperoAI's shared ggml ABI. Update the archive URL and exact SHA-256 in `tools/deps.json` only after hashing the downloaded source. Keep dependency portability changes in `tools/vulkan-runtime/` limited to what the new loader requires.

Run: `make deps && make vulkan-runtime-libs`

Expected: PASS with SD and llama using the same `GGML_MAX_NAME == 160` ABI; if the commit breaks that contract, do not deploy it and record the concrete blocker rather than silently dropping the requested variants.

- [ ] **Step 5: Run the focused catalog and router tests**

Run: `python3 -m unittest discover -s tests -p 'test_image_model_catalog.py' -v` and `python3 -m unittest discover -s tests -p 'test_vulkan_router.py' -v`.

Expected: PASS for exact catalog coverage, generated metadata, model discovery, and a capability response that matches the curated preset.

### Task 2: Remove Qwen Image as a product model

**Files:**
- Modify: `inference/router.cpp`
- Modify: `include/gpt_runtime.hpp`
- Modify: `include/runtime_vulkan.hpp`
- Modify: `inference/runtime_adapter.hpp`
- Modify: `inference/adapters/registry.cpp`, `inference/adapters/llama/vulkan.cpp`
- Modify: `inference/adapters/media/stable_diffusion_ps5.cpp`
- Modify: `vulkan/http_server.cpp`, `vulkan/openai_api.inc`
- Modify: `assets/web/workspace.js`, `assets/web/app.js`, `assets/web/index.html`, `assets/web/app.css`
- Delete: `include/qwen_image_editing.hpp`, `include/qwen_image_vram_budget.hpp`, `assets/web/qwen-image-attachment.js`
- Modify: `tests/vulkan_router_test.cpp`, `tests/test_http_api.py`, `tests/http_api_harness.cpp`, `tests/native_controller_test.cpp`, `tests/audio_workflows_test.cpp`, `tests/web_workspace_audio_test.mjs`
- Delete: `tests/qwen_image_editing_test.cpp`, `tests/qwen_image_vram_budget_test.cpp`, `tests/test_qwen_image_editing.py`, `tests/test_qwen_image_vram_budget.py`, `tests/web_qwen_image_attachment_test.mjs`

**Interfaces:**
- Remove the Qwen-only trailing `image_path` parameter from `gpt_runtime_generate(...)` and `ps5_sd_generate(...)`; Task 3 adds the generic `const gpt_runtime_image_options_t *image_options = nullptr` parameter.
- `gpt_runtime_model_is_qwen_image_2_1(...)` and Qwen Image branches in the OpenAI chat path are removed. Generic model discovery recognizes supported diffusion pipelines and curated presets but never advertises Qwen Image 2.1; `/api/image/upload` remains generic for Task 3.
- Qwen3 tokenizers/encoders and shared third-party Qwen code stay when another supported architecture consumes them.
- Keep the generic model-delete path able to remove a user's already-installed legacy Qwen Image directory; this is deletion support only, not model discovery or generation support.

- [ ] **Step 1: Replace Qwen assertions with product-removal regression tests**

Update router tests to create a Qwen Image-shaped directory and assert that it is not discovered, while a fixture using a shared Qwen3 encoder for a supported model remains discoverable. Update web workspace tests to assert image modes follow server-declared capabilities rather than a Qwen model ID. Keep generic upload bounds/authentication tests and rename their fixture/path assertions from `.qwen-image-*` to a neutral image-upload prefix. Add API assertions that OpenAI chat rejects legacy `image_id` use instead of silently ignoring it.

Run: `PYTHONPATH=tests python3 -m unittest test_http_api.HttpApiTests.test_image_upload_returns_an_opaque_id_for_png_and_jpeg -v`, `python3 -m unittest discover -s tests -p 'test_vulkan_router.py' -v`, and `node --test tests/web_workspace_audio_test.mjs`

Expected: FAIL on the obsolete Qwen-only UI/router assertions and pass the preserved generic upload contract.

- [ ] **Step 2: Remove Qwen-specific runtime and client branches**

Delete Qwen Image detection, optional projector/context identity, Qwen VRAM estimation, Qwen chat image-input/archive path, ID-gated UI, and the Qwen-specific browser attachment module. Remove the obsolete `gpt_runtime_generate`/adapter `image_path` parameter and update its production callers, wrappers, and test stubs; Task 3 introduces generic typed image options. Keep the authenticated, bounded `/api/image/upload` route and shared image codec for the next task. Remove Qwen-only tests; do not delete user model or media files.

- [ ] **Step 3: Run removal regressions and inspect remaining Qwen references**

Run: `python3 tests/test_http_api.py -v`, `python3 -m unittest discover -s tests -p 'test_vulkan_router.py' -v`, `node --test tests/web_workspace_audio_test.mjs`, and `grep -Rni 'qwen-image-2.1\|qwen_image_2.1\|QwenImage21Pipeline' inference include src vulkan/openai_api.inc vulkan/http_server.cpp assets/web`.

Expected: production model discovery, generation, OpenAI chat, and web workspace contain no Qwen Image product path; remaining Qwen3/shared-library references are required by other models. The generic model-delete code may retain its legacy Qwen directory signature solely to preserve user deletion support.

### Task 3: Add typed image workflows to the shared runtime and API

**Files:**
- Modify: `include/gpt_runtime.hpp`
- Modify: `inference/runtime_adapter.hpp`
- Modify: `inference/router.cpp`
- Modify: `inference/adapters/media/stable_diffusion_ps5.cpp`
- Modify: `vulkan/http_server.cpp`, `vulkan/openai_api.inc`
- Modify: `tests/vulkan_router_test.cpp`, `tests/http_api_harness.cpp`, `tests/test_http_api.py`

**Interfaces:**
- Add `gpt_runtime_image_options_t` with `workflow`, `width`, `height`, `steps`, `seed`, `cfg_scale`, `strength`, `control_strength`, `canny`, `negative_prompt`, `sampler`, `scheduler`, `image_path`, `mask_path`, and `control_image_path`. `workflow` is one of `text_to_image`, `image_to_image`, `inpainting`, or `controlnet`; sampler/scheduler are validated names mapped to the pinned API enums. `image_to_image` uses the model's declared `image_edit_strategy`; reject `strength` for `reference_image` models.
- Append `const gpt_runtime_image_options_t *image_options = nullptr` to `gpt_runtime_generate(...)` and carry the pointer in `GenerationRequest`; non-image adapters ignore it.
- `POST /api/generate` accepts an `image_workflow` object with those scalar fields and opaque `image_id`, `mask_id`, and `guide_id` values, plus optional `async`, `output_session_id`, and `output_message_index`. Enforce dimensions 256–1536, multiples of 64, and at most 1,048,576 output pixels; steps 1–50; seed -1 or 0–4,294,967,295; CFG 0–20; strength 0–1; and control strength 0–2. It uses existing `/api/jobs` polling when async; when a session is supplied, it archives the generated PNG with `prospero_session::archive_media` and returns the archived path in the assistant result so `/api/media` can list it.
- Existing `POST /api/image/upload` returns an opaque 32-character ID, stores only validated PNG/JPEG input under the app-owned image work directory, requires authorization, and remains capped at 16 MiB and 2048 pixels per edge.

- [ ] **Step 1: Add failing runtime and HTTP contract tests**

Test every workflow's required-input rules and capability gate, valid/invalid dimensions, strength and CFG bounds, seed behavior, sampler/scheduler mapping, and parameter forwarding. Verify image, mask, and guide IDs resolve only to active server-owned uploads; malformed, expired, unowned, corrupt, or model-incompatible IDs fail before runtime invocation. Verify upload failure preserves unrelated files and successful/failed generation follows the documented lease cleanup behavior. Assert a generated file is saved to the existing image output root and, when a conversation is supplied, archived so it appears in `/api/media`.

Run: `python3 tests/test_http_api.py -v` and the `tests/vulkan_router_test.cpp` host target.

Expected: new workflow cases fail because the current runtime has no generic image-workflow options.

- [ ] **Step 2: Add the typed options structure and forward it through the adapter**

Implement `gpt_runtime_image_options_t` and `GenerationRequest` forwarding. Keep existing non-image `gpt_runtime_generate` callers source-compatible through the default null pointer; remove the Qwen-specific parameter replaced in Task 2. Preserve native text-to-image defaults when `image_options == nullptr`.

- [ ] **Step 3: Map each supported workflow to the pinned diffusion API**

In `stable_diffusion_ps5.cpp`, decode each provided image through the shared image codec and map `init_image_strength` image-to-image to `init_image` plus `strength`, inpainting to `init_image` plus `mask_image` and strength, and ControlNet to `control_image` plus `control_strength`; apply the pinned library's `preprocess_canny` to the guide image only when requested. Map `reference_image` editing to `ref_images` only for FLUX.2 Klein. Map validated sampler/scheduler names to `sd_img_gen_params_t` enums. Reject every unsupported combination before allocating the diffusion context. Use catalog-provided per-model defaults; preserve current native settings for null options.

- [ ] **Step 4: Parse and validate the image request before queuing generation**

Extend `POST /api/generate` and its async worker to parse the exact fields in the Interfaces block. Validate model purpose/capabilities, prompt, paired dimensions and pixel area, integer/float bounds, allowed sampler/scheduler, required image IDs, conversation ID, and upload lease acquisition before starting inference. Pass server-owned paths only. If session archival fails, retain the generated file and return its valid output path with an archive warning; never restart the title after memory refusal.

- [ ] **Step 5: Verify request validation and runtime forwarding**

Run: `python3 tests/test_http_api.py -v` and `python3 -m unittest discover -s tests -p 'test_vulkan_router.py' -v`.

Expected: PASS for each valid workflow's exact forwarded fields; invalid capabilities and uploads fail before inference; an async job reaches a terminal success/error state; generated output paths pass existing media ownership checks.

### Task 4: Build the web-only PS5SD-inspired Image Studio

**Files:**
- Modify: `assets/web/index.html`, `assets/web/workspace.js`, `assets/web/app.js`, `assets/web/app.css`
- Modify: `tests/web_workspace_audio_test.mjs`
- Create: `tests/web_image_studio_test.mjs`
- Modify: `vulkan/http_server.cpp` only if an image-specific asset route is needed

**Interfaces:**
- The web Image workspace has three destinations: `Create`, `Models`, and `Gallery`. Models opens the existing Models page filtered to image models; Gallery opens the existing Media page filtered to images. There is no second model catalog or media store.
- Create modes are `text_to_image`, `image_to_image`, `inpainting`, and `controlnet`. `ProsperoWorkspace.imageCapabilities(model)` reads the server-declared capability list; unavailable modes are hidden or disabled with a short explanation, never inferred from an ID substring.
- Image inputs are selected from a local PNG/JPEG file or from an existing Gallery image. The web holds a preview and uploads on submit; mask painting produces a bounded PNG mask. Gallery `Edit` and `Guide` actions stage the selected image in the corresponding Create mode.
- Image generation submits the Task 3 `/api/generate` request, displays progress from `/api/jobs`, and leaves successful outputs visible in the existing Media page and conversation history.

- [ ] **Step 1: Add failing web behavior tests**

Add Node tests for capability-driven mode visibility, empty states, model changes clearing incompatible staged options, model-specific default/settings visibility, missing-image validation, image/mask/guide file selection, Gallery `Edit`/`Guide` actions, and async job success/failure. Add structural assertions for Create/Models/Gallery navigation, responsive accessible controls, and the absence of Qwen-specific elements.

Run: `node --test tests/web_image_studio_test.mjs tests/web_workspace_audio_test.mjs`

Expected: FAIL because the current Workspace only has text-to-image/image-to-image tabs and a Qwen-gated attachment path.

- [ ] **Step 2: Add the Image Studio navigation and capability-aware forms**

Implement the three destinations and four generation modes in the current Workspace. Keep model selection and image options staged until Generate. Show model-specific size, steps, CFG, sampler, scheduler, seed, and negative prompt controls; expose strength for `init_image_strength` img2img and inpainting only, not for FLUX.2's `reference_image` strategy; expose Canny/control strength only for ControlNet. Start from the preset defaults and enforce the server's per-model bounds. Preserve the existing responsive theme, keyboard focus, labels, and `aria-pressed`/tab semantics. Point Models and Gallery to their existing image-filtered pages rather than copying their catalogs.

- [ ] **Step 3: Add image, mask, and guide input handling**

Support selecting PNG/JPEG from the browser, replacing/removing an input, displaying a preview, and uploading only when a request is submitted. Add a canvas mask painter over the selected source image with explicit paint/erase and clear actions. Selecting `Edit` or `Guide` from an image card loads that image through the authenticated same-origin API, stages it, and returns to Create; it must not send an archived path to the server as an input path.

- [ ] **Step 4: Submit, poll, and preserve generated results**

Submit capability-validated options and opaque upload IDs to `/api/generate`; poll `/api/jobs` without duplicate submissions. Persist the prompt/model/result in the existing conversation format, refresh image-only Media entries when a job completes, and preserve uploaded inputs after retryable generation/archive failures. Do not affect audio or text Workspace submissions.

- [ ] **Step 5: Run behavior tests and inspect rendered states**

Run: `node --test tests/web_image_studio_test.mjs tests/web_workspace_audio_test.mjs`. Start the task-scoped web fixture preview under `.amp/in/` with API fixtures for one model supporting each capability and inspect screenshots of Text-to-image, Image-to-image, Inpainting with a painted mask, ControlNet with a guide, and the image Gallery at desktop and mobile widths. Store screenshots in `.amp/in/artifacts/` after confirming `/.amp/in/` is present in the repository-local Git exclude file.

Expected: PASS; each model shows only supported modes, actions remain usable at both widths, and screenshots visibly distinguish the source/mask/guide and result states. Inspect all captures before continuing.

### Task 5: Validate the full build, test every preset on PS5, and review issues

**Files:**
- Modify: `docs/SUPPORTED_MODELS.md` and other model documentation owned by this change
- Modify: the existing PR #43 branch only
- Test: complete host suite, PS5 build/ELF comparison, and `PPSA99023`

**Interfaces:**
- Model documentation records the exact PS5SD evidence revision separately from per-model ProsperoAI status and known blockers.
- Console proof records the model ID/variant, requested workflow/settings, valid output path, model-switch result, memory/error evidence, and whether the candidate is ProsperoAI-verified or experimental/blocked.

- [ ] **Step 1: Run format, lint, host tests, and image/web regressions**

Run: `make format-check`, `make lint`, `make test`, `python3 tests/test_http_api.py -v`, `python3 -m unittest discover -s tests -p 'test_vulkan_router.py' -v`, `node --test tests/*.mjs`, and the catalog test from Task 1.

Expected: PASS; fix failures caused by this change before building the console app.

- [ ] **Step 2: Build the PS5 app and check its ELF before deployment**

Read `docs/BUILDING.md`, build with the repository's documented command, and compare `readelf -d` NEEDED entries and program headers against a known working build. Do not deploy if the shared ggml ABI or ELF requirements differ unexpectedly.

- [ ] **Step 3: Verify the target and test the curated presets on `PPSA99023`**

Load the configured PS5 environment, run `python3 tools/ps5ctl.py doctor`, and confirm `agent procs` shows the test title stopped and `shadow info PPSA99023` reports `mounted:false` before updating the folder-backed title. Do not reboot. Test every listed preset with its documented PS5SD resolution/settings, capture the app trace/klog, inspect each generated PNG, verify model switching back to a text or audio model, and exercise text-to-image, img2img, both inpainting presets, and SDXL Canny. Mark each candidate ProsperoAI-verified only after valid output and successful cross-modality return; otherwise record the exact blocker and label it experimental/blocked.

- [ ] **Step 4: Recheck issues #42 and #47 and update PR #43**

Read current PR #43 comments and both issue descriptions/statuses. Close only an issue whose acceptance behavior this implementation demonstrates; leave unrelated or still-failing issues open with the remaining blocker. Commit and push the finished changes to the existing PR #43 branch as requested, then subscribe to CI for the pushed head commit.

- [ ] **Step 5: Verify final CI and review the combined diff**

Inspect the complete diff for Qwen Image product references, stale tests, accidental data deletion, incorrect capability labels, and generated metadata drift. Verify the PR head matches the pushed branch and check the subscribed workflow result; if CI fails because of this change, fix and reverify it.

Expected: only requested code/docs are included in PR #43; no issue is closed without passing evidence; candidate verification status matches the console result.
