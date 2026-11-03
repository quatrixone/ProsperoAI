# PR #43 Image Studio — handoff state

Last updated: 2026-11-03

## Where the work is

- Checkout: `/home/dietpi/Projects/console/ProsperoAI`
- Branch: `codex/five-improvements-plan`, pushed to `origin` (PR #43). HEAD `0a1135d`.
- Test title: `PPSA99023` only. The current build is deployed and running there; the web
  assets under `/data/homebrew/PPSA99023/assets/web/` match the checkout.

## Done in this round

Each item below was verified; where the evidence is a console run it says so.

- **Media delete** (`e41bdd8`). The server only allowed deleting an archived copy stored in a
  folder named after its conversation, so the Delete button returned 409 for the flat
  `work/image` and `work/audio` files that make up most of the gallery. The folder-name rule is
  gone; `media_path_allowed` plus the single-reference check still bound what can be removed.
  Covered by a new case in `tests/test_http_api.py`.
- **FLUX.2 Klein** (`58fdabc`). Two defects. The adapter forced `prediction = FLOW_PRED`, which
  disables the library's own detection and builds a `DiscreteFlowDenoiser`; FLUX needs
  `FLUX_FLOW_PRED`, and the output was structured noise. Prediction, VAE format and scheduler are
  now left on auto. Separately, naming a params backend turns auto-fit off and pins every weight,
  and the model manager can only evict a tensor whose parameters live off its compute backend — so
  it had nothing to release and sampling died with 220 MB of an 11.7 GiB heap free. Auto-fit alone
  was not enough either; `max_vram = "6"` for the split-file presets moves the 2.4 GiB text
  encoder to disk. The same commit stops the split-file layout flag from demanding the FLUX.2
  edit strategy on plain text-to-image requests.
  Console evidence: 1024x1024 in 186 s, correct image; SDXL 94 s and the ControlNet guide 134 s,
  both ~20% faster than with the pinned placement.
- **FLUX.3 removed** (`9aa01bf`). See the blocker below.
- **Upscale factor** (`fcc9865`). Off / 2x / 4x in the web Studio, web Settings and native
  Settings, carried by the `upscale` request field and the `sd_upscale` setting. Real-ESRGAN x4
  only produces its own scale — the library ignores `upscale_factor` — so 2x is that pass
  box-averaged back down by `halve_image`. A boolean `upscale` and a stored `sd_upscale=1` still
  mean 4x. **Not yet exercised on the console.**
- **UI freeze during generation** (`0a1135d`). The once-a-second Vulkan memory poll took the
  runtime lock that generation holds for its whole run, and one worker serves every job, so no
  other action could start — the trace shows `refresh Vulkan memory: 243.01 s` after an ACE-Step
  run. The poll now tries the lock and keeps the previous figures. The regression test in
  `tests/native_controller_test.cpp` was confirmed to fail without the fix
  ("worker did not complete") and pass with it.

Host checks after each change: `make test` 134 tests OK (1 skipped), `make lint` clean,
PS5 app build OK.

## Image presets verified on PPSA99023

All at their preset defaults, seed 42, prompt "a red apple on a wooden table, photo":

| preset | size | steps | time |
| --- | --- | --- | --- |
| sd-turbo-fp16 | 512 | 1 | 29 s |
| realistic-vision-v51-fp16 | 512 | 20 | 34 s |
| sdxs-512-q8-0 | 512 | 1 | 8 s |
| sdxl-base-q8-0 | 1024 | 10 | 94 s |
| raehoshi-illustxl-1-1-q8-0 | 1024 | 28 | 166 s |
| ilustmix-v11-1-q8-0 | 832x1216 | 10 | 111 s |
| sdxl-inpainting-q8-0 | 1024 | 25 | 134 s |
| sd15-inpainting-q8-0 | 512 | 25 | 33 s |
| sdxl-canny-q8-0 | 1024 | 25 | 134 s |
| flux2-klein-4b-q8-0 | 1024 | 4 | 186 s |

Not yet run: `flux2-klein-9b-q4-0` and `z-image-turbo-q4-k`. Both finished downloading during
this session and are installed; neither has been generated once.

The verification script is `tools`-free and lives only in the session scratchpad; rewrite it from
the table above (POST `/api/generate` with the preset's `image_defaults` as `image_workflow`, then
GET the returned `image_path`). Look at the PNG — a run that reports `done` can still be noise,
which is exactly how the FLUX bug hid.

## Blocker: FLUX.3

The pinned stable-diffusion.cpp (`b13a482`, `tools/deps.json`) has no `VERSION_FLUX3`, so both
FLUX.3 Image Klein presets fail version detection at load with
`get sd version from file failed`. The presets were removed from `model-tools/presets.json`
rather than left as models that cannot run. PS5SD runs a newer revision that does support it.

Restoring them means moving the dependency pin forward, which also moves the ggml ABI the text
runtime shares (`static_assert(GGML_MAX_NAME == 160)`) and risks every preset that now works.
That is a dependency upgrade, not a bugfix — get the owner's decision before starting it.

The 6.3 GB `flux3-image-klein-q4-k` install is still on the console, orphaned now that the preset
is gone. It was deliberately left in place in case the pin moves; delete it with the owner's say-so.

## Next

1. Generate once with `flux2-klein-9b-q4-0` and once with `z-image-turbo-q4-k` and look at both
   PNGs. Z-Image uses the same split-file layout as FLUX but a different denoiser, so it is the
   case most likely to expose a wrong family inference; `max_vram = "6"` also applies to it and
   may be the wrong budget for a 9B model.
2. Exercise the upscale factor on the console: 2x and 4x on a small model (sdxs is 8 s), and
   check the output dimensions are what the factor promises. `halve_image` in
   `inference/adapters/media/stable_diffusion_ps5.cpp` is new and has no direct test.
3. Click Delete in the web Media page against the running title. The fix is covered by a host
   test but has not been exercised through the browser.
4. Still open from the owner's earlier list, not started: Enhance and prompt templates for the
   Image Studio, mirroring `enhance(field)` and `TPL_BUILTIN` in `assets/web/music.js`.
5. Still open: TTS voice selection in the web client. The server round-trips `kokoro_voice`
   correctly and the unit test passes, so the fault is on the client — most likely
   `applyRuntimeSettings` re-running `populateWorkspaceVoicePicker` and discarding the selection.
6. The native UI has never been seen in this work: `P5_SESSION` is unset, so no screenshots. The
   Media tab layout and the new three-way upscaler choice are unverified visually.

## Standing notes

- `max_vram = "6"` is tuned for FLUX.2 Klein 4B at 1024x1024. Treat it as a measured constant,
  not a principle; a different model size may need a different budget.
- Turn `sd_log`'s threshold down to `SD_LOG_INFO` in the adapter when debugging a load or memory
  failure. The auto-fit plan and the per-module memory breakdown only appear at INFO, and they
  are what made the FLUX diagnosis possible. Put it back to `SD_LOG_WARN` afterwards.
