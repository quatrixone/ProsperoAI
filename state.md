# PR #43 Image Studio — handoff state

Last updated: 2026-11-11

## Current work

- Checkout: `/home/dietpi/Projects/console/ProsperoAI`
- Branch: `codex/five-improvements-plan`, PR #43 open as draft.
- PR head before this work: `0cb7bf66ddc8d604ac39e03459c72a5a63cedf4a`.
- Latest `origin/main`: `ab13fea738fc3fccdaceab6d808d8e83dafe1ac4`; PR conflicts with main's UI-kit source fix.
- Test title: `PPSA99023`; currently running. Do not touch `PPSA99004` on console.
- Console setup is already permission-free in this environment; `tools/ps5ctl.py doctor` passes.

## This round: llama.cpp upstream

- Updated llama.cpp to `cb44fdb1892eebca072e0949875a60c11245a8bb` and shared GGML to `cd6d2b75f5d3e4b2f3fe8e8fbc6c8b84ac80e74a`.
- Updated the Bonsai and PS5 Vulkan compatibility adapters for the new upstream layouts/APIs.
- Host Vulkan runtime build, `make test`, and `make lint` passed.
- PS5 folder build deployed and launched on `PPSA99023`; runtime API returned successfully and title remained alive after generations.
- Remote Play welcome/workspace screenshot reviewed: clean dark/gold layout. Screenshot: `build/pr43-validation/screenshots/upstream-welcome.jpg`.
- Validation details: `docs/performance/2026-11-11/pr43-upstream-validation.json`; per-preset run data: `docs/performance/2026-11-11/pr43-upstream-all-models.json`.

## Model validation and blockers

- 22/25 catalog entries passed the generic benchmark (five text, twelve image generation requests, five audio/speech generation requests); generic image calls do not prove inpainting or ControlNet functionality.
- Ten ordinary text-to-image outputs looked plausible. Inpainting and ControlNet outputs were poor under generic text-to-image input; proper image upload IDs were rejected, so these modes remain unverified.
- Stable Audio 3, ACE-Step, Kokoro, Chatterbox and YuE2 generated valid audio outputs. SheetSage Cover and HTDemucs Stems could not open a WAV source even though it was listed over FTP; both remain blocked.
- Real-ESRGAN was independently verified at 2x (1024x1024) and 4x (2048x2048).
- Two extra local standalone GGUFs (Gemma 4 26B and Qwen3.8 27B) failed to load on Vulkan; Bonsai and Qwen3VL passed. Two other GGUF files are components and were not tested as standalone models.
- Do not describe all models/features as passing. Resolve or clearly scope these issues before a release claiming full model support.

## PR and release next steps

1. Merge latest `origin/main` into this branch; preserve its JVK3R UI-kit source and combine the fallback mirror list.
2. Inspect the two workflow input failures and unsupported local GGUFs if release scope requires all-model coverage.
3. Rerun the build and checks after merge, update PR description/title, and complete the PR/release flow only with accurate validation claims.
4. Clean only the task-owned temporary input `/data/prosperoai/work/audio/pr43-input.wav` if it still exists. Preserve generated media in the persistent work folders and all user `work/` content.

## PS5 state

`PPSA99023` is mounted and its app process is alive. No reboot was performed. Latest deployed build is the PR #43 upstream llama.cpp/GGML folder build.
