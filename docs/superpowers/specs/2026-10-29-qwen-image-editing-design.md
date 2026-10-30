# Qwen Image 2.1 image-to-image design

Status: approved.

## Goal

Add local image-conditioned editing to Qwen Image 2.1 through ProsperoAI's web
composer and authenticated API, while preserving existing text-to-image behavior.
Verify both paths on the designated PS5 test title before closing issue #42.

## Approved product behavior

- A user can attach one PNG or JPEG to a Qwen Image 2.1 prompt, preview it, and
  remove or replace it before submitting. Other model types do not expose this
  control.
- The same operation is available to API clients: upload the image as binary,
  then reference its opaque upload ID on the generation request. The existing
  JSON request-size limit remains unchanged.
- Image bytes stay on the console. Web edits associate the source image with
  their saved conversation; deleting that conversation removes its archived
  source image. Conversation exports preserve the local media reference, not the
  image bytes. Unused uploads expire and are deleted, and a failed generation
  leaves an unexpired upload available for retry.
- Image editing uses Qwen Image 2.1's reference-image conditioning. It does not
  introduce the denoising-strength control used by other img2img pipelines.
- Existing Qwen text-to-image installations remain discoverable and continue
  working without the optional vision projector.

## Model requirements

Qwen editing needs the Qwen3-VL vision projector in addition to the existing
diffusion, language, and VAE files. Use `mmproj-Qwen3VL-8B-Instruct-F16.gguf`
from the Qwen3-VL GGUF repository, as listed in the pinned stable-diffusion.cpp
download instructions. Store it beside the existing Qwen files. Treat it as
optional for model discovery and text-to-image, but return a clear
editing-specific error when it is missing.

Load the projector only for an image-edit request, passing it as
`sd_ctx_params_t.llm_vision_path`. Include projector presence in the Qwen
context identity so a text-only context cannot be reused for editing without
the projector, and so text-to-image does not pay its memory cost when editing
is not requested. The input image is passed to `sd_img_gen_params_t.ref_images`
with request-scoped storage held until generation completes.

## Upload and generation contract

- `POST /api/image/upload` accepts authenticated raw PNG or JPEG bytes with a
  required `Content-Length`; uploads are capped at 16 MiB and decoded image
  dimensions are capped at 2048 × 2048.
- The server streams each upload to a uniquely created temporary file beneath
  `work/image/`, validates the encoded format and decoded dimensions, and
  returns an unguessable opaque ID. It never accepts a client-supplied path.
  An unused ID expires after 15 minutes; cleanup must skip an upload while a
  generation is using it.
- `/v1/chat/completions` accepts an optional top-level `image_id`. It is valid
  only for an installed Qwen Image 2.1 model and a user prompt. Web requests also
  send their `conversation_id`; API clients may omit it when they do not need
  persistent conversation media. A supplied ID must name an existing image
  conversation. Missing, expired, malformed, or unsupported image IDs fail
  before model execution.
- On success, requests with a `conversation_id` archive the source image against
  the final user message and include its local media path in the chat response.
  The web client stores that path as the optional `image` message field, which
  conversation save, load, sync, and export preserve; the UI renders it as media
  rather than prompt text. Successful uploads are removed after archival, or
  immediately after generation when no conversation ID was supplied. On failure,
  an upload remains retryable until expiry. Expired uploads are cleaned up
  without touching unrelated user files.
- The durable session copy is normalized to PNG. If archival fails after image
  generation, preserve the generated output and retryable upload, then return
  an explicit archive error instead of silently dropping the source image.
- The web composer uploads the selected file only when the user submits. It
  sends the returned ID with the generation request and displays the source
  image in the saved user message alongside the generated result.

## Architecture

```text
Web composer ── binary upload ──> authenticated HTTP upload handler
      │                                 │
      └── image_id + prompt ────────────┴──> chat generation request
                                                   │
                      request-scoped image input ──┤
                                                   v
                              inference adapter -> Qwen Image 2.1
                              llm_vision projector + ref_images
```

The HTTP layer owns bounded upload streaming, ID validation, expiry, and file
lifecycle. The runtime receives an explicit request-scoped image input; it must
not use a process-global current-image variable. The Qwen adapter decodes the
validated file to the pixel format expected by stable-diffusion.cpp and holds
the decoded buffer through the synchronous generation call. When a conversation
ID is supplied, conversation storage owns the durable source copy and deletes
it with the conversation. Conversation export records the local media path but
does not bundle image bytes; importing the export elsewhere does not transfer
the source image. The durable copy is encoded as PNG so the existing session
media archive can store and serve it without adding another persistent format.

## Error and memory behavior

- Reject unauthenticated, empty, oversized, unsupported, corrupt, or
  over-dimensioned uploads before inference; interrupted uploads leave no
  published file.
- Reject expired or unknown IDs and reject image input for non-Qwen models with
  an explicit client error. Do not treat an invalid edit as text-to-image.
- Preserve an upload after generation failure for retry; remove it after a
  successful archive or expiry. File creation and publication must not overwrite
  existing files, and cleanup must be scoped to this feature's temporary files.
- Report a missing projector distinctly from a Vulkan memory failure. Preserve
  the existing dynamic Qwen VRAM budget and test whether loading the projector
  and processing a reference image fits on the PS5.

## Non-goals

- Native-console UI image picking, unrestricted filesystem browsing, masks,
  inpainting, multiple reference images, or controls for non-Qwen models.
- Base64 image bodies, external image services, a general-purpose upload
  framework, or changes to the existing 256 KiB JSON request limit.
- Changes to Qwen text-to-image defaults, output format, or model licensing.

## Acceptance criteria

1. The web composer exposes an accessible PNG/JPEG attachment control only for
   Qwen Image 2.1, with a preview and remove/replace behavior.
2. An authenticated binary upload returns an opaque ID; malformed, oversized,
   invalid-format, invalid-dimension, interrupted, and unauthenticated uploads
   are rejected without overwriting files or leaving partial uploads.
3. An API edit request uses the ID as Qwen `ref_images`; invalid or expired IDs
   and non-Qwen models fail before inference. Text-to-image requests remain
   unchanged.
4. The Qwen edit context loads the optional vision projector only when needed,
   and cannot reuse an incompatible text-only context.
5. The source image appears in the persisted user message as a server-owned PNG
   media path, survives same-console conversation sync and export as a path
   reference (the bytes are not bundled), and is deleted with its conversation.
   Temporary uploads expire and successful uploads are cleaned up.
6. Host tests cover request validation, upload streaming and failure cleanup,
   image-to-runtime forwarding, model/projector checks, and conversation media
   lifecycle. UI tests cover attach/preview/remove, submit, and retry states; the
   rendered result is inspected.
7. On the designated PS5 test title, verify Qwen text-to-image and image-to-image
   generation, output files and conversation media, missing-projector behavior,
   invalid image rejection, temporary cleanup, and that the app remains alive
   under the measured VRAM budget. Do not modify the main title `PPSA99004`.
8. Close issue #42 only after the host checks and PS5 image-to-image test pass.

## Verification plan

- Implement host regression tests before production changes and observe the
  expected failures first.
- Run the focused HTTP/runtime/UI tests, `make lint`, and the complete
  `HOST_CXX=clang++-18 HOST_CC=clang-18 make test` suite.
- Build the PS5 app after reading `docs/BUILDING.md`; compare the ELF NEEDED list
  and program headers with a working build before deployment.
- After build validation, check `agent procs`, deploy only to the configured
  test title, wait for `/api/settings/runtime`, and exercise both Qwen text and
  image editing through the app API. Inspect app trace/klog and process state
  after failures. Do not reboot or touch `PPSA99004`.
- Current environment has no `.env.ps5.local`; `PS5_HOST` and
  `PROSPERO_TEST_TITLE` must be configured before console verification can run.
