# PR #43 Image Studio — handoff state

Last updated: 2026-11-12

## Current work

- Checkout: `/home/dietpi/Projects/console/ProsperoAI`
- Branch: `codex/five-improvements-plan`, PR #43 open as draft.
- PR head before follow-up fixes: `1b62b87ad4e2501824e8b6b2946ea973d50e4b61`.
- Latest `origin/main`: `ab13fea738fc3fccdaceab6d808d8e83dafe1ac4`; UI-kit source conflict was resolved and PR is mergeable.
- Test title: `PPSA99023`; currently running. Do not touch `PPSA99004` on console.
- Console setup is already permission-free in this environment; `tools/ps5ctl.py doctor` passes.

## This round: llama.cpp upstream

- Updated llama.cpp to `cb44fdb1892eebca072e0949875a60c11245a8bb` and shared GGML to `cd6d2b75f5d3e4b2f3fe8e8fbc6c8b84ac80e74a`.
- Updated the Bonsai and PS5 Vulkan compatibility adapters for the new upstream layouts/APIs.
- Host Vulkan runtime build, `make test`, and `make lint` passed.
- PS5 folder build deployed and launched on `PPSA99023`; runtime API returned successfully and title remained alive after generations.
- Remote Play welcome/workspace screenshot reviewed: clean dark/gold layout. Screenshot: `build/pr43-validation/screenshots/upstream-welcome.jpg`.
- Validation details: `docs/performance/2026-11-11/pr43-upstream-validation.json`; per-preset run data: `docs/performance/2026-11-11/pr43-upstream-all-models.json`.

## Follow-up validation (2026-11-12)

- Image upload lease validation and audio source validation now open files with `O_NOFOLLOW` and verify them with `fstat`; symlink regression checks were added.
- SDXL ControlNet now budgets memory for both model weights and its guide-network sampling workspace.
- On `PPSA99023`, real image uploads and both inpainting presets passed; downloaded outputs were visually reviewed. SDXL Canny ControlNet passed with a real guide and its default 25 steps; output was visually reviewed.
- SheetSage Cover completed in about 41 seconds and produced ABC plus event JSON. HTDemucs produced all five expected stem WAV files.
- The 25 installed curated presets have successful generic or workflow-specific checks. Real-ESRGAN 2x and 4x passed. Remote Play welcome/workspace screenshot remains clean and readable.
- `make test`: 138 passed, 2 skipped; host integration suite passed. `make lint` passed.
- Two optional non-catalog local models (Gemma 4 26B and Qwen3.8 27B) exceed available console Vulkan memory. This does not establish arbitrary local GGUF support. Component GGUFs are not standalone models.
- Full results: `docs/performance/2026-11-12/pr43-console-validation.json`.

## PR and release next steps

1. Commit and push the verified fixes and validation report to PR #43.
2. Wait for PR checks, then merge and publish the requested release if they pass.
3. Keep claims scoped to the 25 installed curated presets and mention the two optional local GGUF memory limits.
4. Preserve generated media in persistent work folders and all user `work/` content.

## PS5 state

`PPSA99023` is mounted and its app process is alive after workflow runs. No reboot was performed. Latest deployed build is the PR #43 upstream llama.cpp/GGML folder build with the verified file and ControlNet fixes.
