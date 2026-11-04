# PS5SD-verified image models without Qwen Image

Status: approved by user.

## Intent

Replace ProsperoAI's failed Qwen Image 2.1 product path with the image models and
image workflows that Darkatek7/PS5SD has actually generated on a PS5. The goal is
to turn that external evidence into a model catalog and user-facing workflows in
ProsperoAI, then separately prove which ones work in ProsperoAI on its own test
title. A PS5SD result is evidence for prioritizing a model, not proof that this
app's adapter, dependency pin, or settings work.

## Model scope

The source evidence is PS5SD `docs/CONSOLE.md` and `ps5/app/catalog.hpp` at
`0654534bc7042d9164167835fba67e74215f1617`. Add the exact catalog variants below
that have console generation evidence; do not imply that every checkpoint in a
family or every quantization has been tested:

- SDXS-512 tinySDdistilled, Q8_0.
- SDXL Base 1.0, Q8_0; the specifically tested Raehoshi IllustXL 1.1 and iLustMix
  v11.1 finetunes, using the console-tested Q8_0 loading path.
- SDXL Inpainting 0.1, Q8_0; SD 1.5 Inpainting, Q8_0.
- SDXL Base Q8_0 with the tested xinsir Canny ControlNet.
- FLUX.2 Klein 4B Q8_0 and 9B Q4_0.
- FLUX.3 Image Klein Q4_K and Q6_K.
- Z-Image Turbo Q4_K.

Keep ProsperoAI's already verified SD-Turbo FP16 and Realistic Vision v5.1 FP16
presets. PS5SD's October 2026 console log explicitly says its SD-Turbo run was
not measured; it is retained because ProsperoAI has its own successful runs.

Include the models PS5SD classifies as image/diffusion models, including FLUX and
Z-Image, not its audio or video catalog. Keep each model's exact upstream source,
quantization, checksum, required encoder/VAE/control files, and license notice in
the app's existing curated-preset mechanism. Do not bundle model weights. Clearly
surface the non-commercial restriction on FLUX.2 Klein 9B.

Keep every requested preset available even if ProsperoAI's console run fails, but
show separate provenance and runtime status: “verified by PS5SD” is not
“verified by ProsperoAI.” A failed local run remains clearly marked experimental
or blocked with the known limitation; never present it as working.

## User-facing behavior

- Remove Qwen Image 2.1 from ProsperoAI's supported image-model list and remove
  its Qwen-specific image input, projector handling, VRAM budget, and Qwen-only
  web/API affordances. Do not delete user-installed model files or generated
  media.
- Preserve Qwen3 text encoders and the shared upstream Qwen diffusion code where
  other supported architectures (notably Z-Image and FLUX.3 Klein) need them.
- Add text-to-image for every listed curated model, with generation defaults
  matching the model-specific console-tested preset.
- Provide image-to-image using the runtime's native `init_image`/strength path
  for compatible SD models. Provide prompt-guided reference editing for FLUX.2
  Klein only if its current backend contract supports it. Add mask-based
  inpainting for the two tested inpainting presets and guide-image Canny for the
  tested SDXL ControlNet preset. Do not expose controls unsupported by the
  selected model.
- Keep text-to-image available through the existing native surface and the web
  Image Studio. Use model capabilities rather than Qwen-specific model IDs to
  decide which web inputs and controls appear.
- Give the web Image workspace a PS5SD-WebUI-inspired Create / Models / Gallery
  navigation. Create offers text-to-image, image-to-image, inpainting, and
  ControlNet modes; show each mode and its controls only when the selected model
  supports it. Reuse ProsperoAI's existing model-management and Media pages for
  the Models and Gallery destinations instead of creating parallel catalogs or
  storage. Gallery image actions can start an edit or use an image as a
  ControlNet guide.
- Keep this expanded Image Studio interface web-only. The native UI keeps its
  existing image-generation interaction; do not add new native image workflow
  controls as part of this interface request.
- Keep source-image uploads bounded, authenticated, server-owned, and scoped to
  each request/conversation. Preserve generated files and existing conversation
  media when Qwen Image support is removed.

## Runtime and dependency constraints

- The currently pinned stable-diffusion.cpp revision is `f89d9b13d730eabeede7314ce49dacd18d3c90c2`.
  Its checked-in model enum includes SDXS, SD 1.5/SDXL inpainting, SDXL ControlNet,
  FLUX.2 Klein, and Z-Image, but not FLUX.3 Image Klein. Full model scope therefore
  needs the smallest safe dependency update or upstream change that adds the
  missing loader; PS5SD's corresponding stable-diffusion.cpp pin is
  `b13a48219d5cdf408edcb93da6cd69b612ed03d3`. Preserve the shared ggml ABI and
  rerun host/build checks before any console deployment.
- PS5SD's measurements were made on FW 13.00 with an 11.44 GiB unified pool.
  ProsperoAI's designated test title is `PPSA99023` on a different runtime/build
  profile. Existing ProsperoAI notes record SDXL workspace exhaustion and a
  FLUX.2 Klein encoder allocation failure; both must be retested after a clean
  model switch and must not be reported as supported based only on PS5SD results.
- Keep the test title and the production title separate. Never overwrite
  `PPSA99004`; do not reboot the console or delete unrelated files.

## Non-goals and anti-goals

- Do not add every family merely recognized by PS5SD; only the exact console-
  demonstrated variants above are in scope.
- Do not restore Qwen Image 2.1 text-to-image, editing, projector, or offload code.
- Do not remove generic Qwen3 encoders, shared third-party model code, existing
  SD-Turbo/Realistic Vision presets, user model files, or generated media.
- Do not treat a successful host build, model load, or PS5SD's results as a
  successful ProsperoAI generation.
- Do not close GitHub issues until the corresponding ProsperoAI behavior passes
  its acceptance checks. Recheck issue #42 after image-generation verification;
  do not close unrelated issue #47 on this work's evidence.

## Acceptance criteria

1. The curated model list contains the exact PS5SD-console-tested variants
   above, with valid source provenance, file sizes/checksums, and license notes;
   existing ProsperoAI-verified image presets remain available.
2. Qwen Image 2.1 is no longer discoverable, installable as a ProsperoAI product
   model, or exposed by image UI/API behavior. Existing user data and generated
   media remain untouched, and Qwen3 encoders used by other models still work.
3. The web Image Studio has Create / Models / Gallery navigation and supports
   text-to-image, compatible img2img, the two inpainting presets, and the SDXL
   Canny workflow. Controls are limited to the selected model's capabilities;
   requests validate source, mask, and guide inputs and return useful errors.
   The native image-generation interface remains unchanged.
4. Host regression tests cover model/preset discovery, capability selection,
   Qwen Image removal, upload/mask/guide validation, runtime parameter
   forwarding, and web mode selection/navigation. Existing unrelated
   multimodal behavior remains intact.
5. Run lint and the complete host test suite, then build the PS5 test app.
   Before deployment, verify the target title is stopped and reports
   `mounted:false`; compare ELF requirements against the known working build.
6. On `PPSA99023`, attempt every curated candidate using its documented
   resolution/settings, inspect generated PNGs, repeat representative model
   switches and image workflows, and capture app trace/klog plus memory/failure
   evidence. Mark a model as ProsperoAI verified only after a valid output and a
   successful return to another modality; if a candidate still fails, keep its
   preset available with an experimental/blocked status and the exact blocker,
   without presenting PS5SD's proof as ProsperoAI proof.
7. Recheck issues #42 and #47 against the final tested behavior; close only an
   issue that this change demonstrably resolves.
8. Keep all requested implementation changes on the existing PR #43 branch.

## Builder decisions

The builder may choose the internal capability representation, the smallest
compatible dependency revision, the precise upload/mask/guide transport shape,
and the order of console tests. These choices must preserve the user-facing
scope, model provenance, runtime ABI, existing model data, and the distinction
between upstream and ProsperoAI console verification.
