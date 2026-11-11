# Qwen Image 2.1 Image-to-Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add authenticated, local Qwen Image 2.1 image editing to the web composer and chat API, preserve source images with saved conversations, and prove text-to-image and image-to-image on the designated PS5 test title before closing issue #42.

**Architecture:** The HTTP layer streams a bounded PNG/JPEG upload into a server-owned temporary file, returns an opaque expiring ID, and passes a leased server path through an explicit runtime request to the Qwen adapter. A shared image codec validates and decodes inputs and normalizes archived sources to PNG; session storage owns the durable copy, while the web composer uploads only on submit and persists the returned local path with the user message.

**Tech Stack:** C++20, existing PS5 HTTP server/runtime and stable-diffusion.cpp APIs, vendored stb_image/stb_image_write, Python unittest harnesses, Node UI tests, and the repository's PS5 build/deployment tooling.

**Spec:** `docs/superpowers/specs/2026-10-29-qwen-image-editing-design.md`

## Global Constraints

- Accept one PNG or JPEG, no larger than 16 MiB and no more than 2048 × 2048 pixels.
- Require authenticated binary upload with `Content-Length`; leave the existing 256 KiB JSON request limit unchanged.
- Give unused uploads a 15-minute expiry and skip cleanup while an upload is leased by generation.
- Keep Qwen's vision projector optional for discovery and text-to-image; load it only for image edits.
- Preserve the existing dynamic Qwen VRAM budget and measure the projector/reference-image memory peak on the test console.
- Use `mmproj-Qwen3VL-8B-Instruct-F16.gguf` beside the existing Qwen files.
- Preserve a failed generation's upload for retry; remove successful uploads after archival or successful non-conversation generation.
- Persist source images as server-owned PNGs; exports contain only the local media reference, not image bytes.
- Do not add native-console image picking, arbitrary paths, multiple references, masks, base64 uploads, or denoising-strength controls.
- Never deploy to or modify `PPSA99004`; do not reboot the console.
- Do not close issue #42 before all host checks and the PS5 image-to-image test pass.

## Review Focus

- A PNG/JPEG with a forged MIME type, corrupt bytes, zero dimensions, or a 2049-pixel edge is rejected before inference; pin in Tasks 1–2.
- A missing `Content-Length`, oversized body, or interrupted stream leaves no published ID or partial file; pin in Task 2.
- Cleanup racing a generation does not delete a leased upload, and a failed generation can retry it; pin in Tasks 2 and 5.
- A Qwen text context cannot be reused for editing without the projector, while text-to-image still works without it; pin in Task 3 and verify again on-console in Task 7.
- If generation succeeds but conversation archival fails, the output remains available and the source upload remains retryable; pin in Tasks 4–5.

---

### Task 1: Add a bounded PNG/JPEG codec

**Files:**
- Create: `include/image_codec.hpp`
- Create: `src/image_codec.cpp`
- Create: `tests/image_codec_test.cpp`
- Create: `tests/test_image_codec.py`
- Modify: `tools/build-vulkan-native.sh`

**Interfaces:**
- `prospero_image::EncodedFormat` has `Png` and `Jpeg` values.
- `prospero_image::DecodedImage` contains `unsigned width`, `unsigned height`, `unsigned channels`, and an stb-owned `unsigned char *pixels` pointer.
- `inspect_file(const char *, EncodedFormat *, unsigned *, unsigned *)` accepts only PNG/JPEG signatures, checks dimensions from 1 through 2048 on each edge before pixel allocation, and fully decodes/releases the image so truncated pixel data is rejected before an upload ID is published.
- `decode_file(const char *, DecodedImage *)`, `free_image(DecodedImage *)`, and `write_png(std::FILE *, const DecodedImage &)` respectively decode, release, and stream a normalized PNG to a file already opened by its owner. Every successful decode remains alive until `free_image`.
- `include/image_codec.hpp` includes `<cstdio>` for the public `std::FILE *` writer interface.

- [ ] **Step 1: Add codec regression tests and fixtures**

Add a tiny valid PNG and JPEG under `tests/fixtures/`. In `tests/image_codec_test.cpp`, assert that `inspect_file` reports each correct format and dimensions, that `decode_file` returns non-null pixels, that `write_png` produces a PNG readable by `inspect_file`, and that a truncated file, a BMP renamed to `.png`, and a 2049-pixel-wide PNG are rejected.

```cpp
using namespace prospero_image;
const char *png_path = "tests/fixtures/one-pixel.png";
const char *normalized_path = "build/tests/normalized.png";
const char *corrupt_path = "build/tests/corrupt.png";
EncodedFormat format{};
unsigned width = 0, height = 0;
DecodedImage decoded{};
assert(inspect_file(png_path, &format, &width, &height));
assert(format == EncodedFormat::Png && width == 1 && height == 1);
assert(decode_file(png_path, &decoded) && decoded.pixels);
std::FILE *normalized = std::fopen(normalized_path, "wbx");
assert(normalized && write_png(normalized, decoded));
assert(std::fclose(normalized) == 0);
free_image(&decoded);
assert(!inspect_file(corrupt_path, &format, &width, &height));
```

- [ ] **Step 2: Run the codec test and confirm the expected failure**

Run: `python3 tests/test_image_codec.py -v`
Expected: FAIL because the codec interface and implementation do not exist yet.

- [ ] **Step 3: Implement the codec with the pinned vendored decoder**

Implement the declared interface in `src/image_codec.cpp` using the pinned `.deps/stable-diffusion-vulkan/thirdparty/stb_image.h` and `stb_image_write.h` implementations in this one translation unit. Check file signatures before decoding, reject dimensions outside 1–2048 before allocating decoded pixels, preserve RGB/RGBA channels, and pair every decoded buffer with `stbi_image_free`. Implement the writer with `stbi_write_png_to_func` so it writes to the caller-owned `std::FILE *`; it must not open, truncate, close, or remove that file.

- [ ] **Step 4: Add the codec to PS5 and host test builds**

Add `src/image_codec.cpp` to the `sources` array in `tools/build-vulkan-native.sh` and add the vendored `thirdparty` include directory for that translation unit. Make `tests/test_image_codec.py` compile the test and codec with the same include directory and `HOST_CXX` selection used by the other C++ tests.

```python
command = [compiler, "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
           "-I" + str(ROOT / ".deps/stable-diffusion-vulkan/thirdparty"),
           str(ROOT / "tests/image_codec_test.cpp"), str(ROOT / "src/image_codec.cpp"),
           "-o", str(binary)]
subprocess.run(command, check=True)
```

Also add `src/image_codec.cpp` and the same include path to the compile command in `tests/test_session_store.py`, because `archive_image()` uses the codec. Add a JPEG-to-PNG test fixture without adding a package dependency; generate the over-dimension PNG fixture with the existing vendored writer or a tiny stdlib-only fixture generator.

- [ ] **Step 5: Run the focused codec test**

Run: `python3 tests/test_image_codec.py -v`
Expected: PASS for valid PNG/JPEG, PNG normalization, and every rejected fixture; no memory sanitizer findings.

### Task 2: Stream authenticated uploads and manage expiring IDs

**Files:**
- Modify: `vulkan/http_server.cpp` near `read_request()` and the `/api/music/uploadbin` route
- Modify: `tests/test_http_api.py`
- Modify: `tests/http_api_harness.cpp` for the deterministic upload-expiry test mode

**Interfaces:**
- `POST /api/image/upload` accepts raw bytes and `Content-Length`; it returns `{"id":"<32 lowercase hex characters>"}`.
- Store each upload at `.qwen-image-<id>.upload` under `PROSPERO_IMAGE_DIR`; derive the path only from a validated 32-character hex ID, derive expiry from the file modification time, and track active leases in memory.
- `ImageUploadLease` contains the validated `std::string id`, server-owned `std::string path`, and `bool active`; it is noncopyable and its destructor releases an unfinished lease without deleting the file. `bool create_image_upload(ImageUploadLease *lease, std::FILE **file)` ensures `PROSPERO_IMAGE_DIR` exists, generates a 128-bit ID, opens its exclusive file, and leases it while streaming; `bool acquire_image_upload(const char *id, ImageUploadLease *lease)` validates and leases an existing upload for one generation; `void finish_image_upload(ImageUploadLease *lease, bool remove)` releases it and optionally removes its file, clears `active`, and retains `id`/`path`; `void cleanup_expired_image_uploads(std::time_t now)` scans and removes only expired files matching the exact feature-owned filename pattern, skipping active IDs.
- `PROSPERO_IMAGE_DIR` defaults to `/download0/ProsperoAI/work/image/` in the app and is overridden to a per-test directory in `tests/test_http_api.py`.

- [ ] **Step 1: Add failing HTTP tests for the upload contract**

In `tests/test_http_api.py`, use `request_raw()` to cover authenticated PNG upload and opaque ID response, unauthenticated upload, empty body, 16 MiB + 1, corrupt/unsupported image bytes, 2049-pixel dimensions, and a body shorter than its declared length. Add a `request_without_length()` helper because `request_raw()` always supplies `Content-Length`. Also submit a decimal `Content-Length` that overflows `size_t` and verify it is rejected before any file is opened. Assert failed requests do not leave any new file in the isolated image directory and do not overwrite a pre-existing sentinel file.

```python
png_bytes = (ROOT / "tests/fixtures/one-pixel.png").read_bytes()
head, body = self.request_raw(
    "/api/image/upload", png_bytes,
    headers=["Authorization: Bearer secret"], method="POST",
)
self.assertIn("200 OK", head)
upload_id = json.loads(body)["id"]
self.assertRegex(upload_id, r"^[0-9a-f]{32}$")
```

- [ ] **Step 2: Run the focused HTTP tests before implementation**

Run: `python3 tests/test_http_api.py -v`
Expected: the new `/api/image/upload` cases fail because the route is not implemented.

- [ ] **Step 3: Implement the streaming upload route**

Reuse the existing request-header parsing and binary streaming pattern, but cap this endpoint at 16 MiB, require `Content-Length`, reject length overflow, authenticate before opening a file, and create an exclusive `.qwen-image-<id>.upload` file under `PROSPERO_IMAGE_DIR`. The ID is not returned until the full body is received and validated; remove the owned file on every failed/truncated/invalid request. Generate 128 random bits from `/dev/urandom`, encode them as lowercase hex, and retry on `O_EXCL` collisions.

```cpp
ImageUploadLease lease{};
std::FILE *file = nullptr;
if (!create_image_upload(&lease, &file)) {
    openai_error(api, sock, "500 Internal Server Error", "upload_write_failed",
                 "Could not create a unique temporary upload file.");
    return 0;
}
const std::string &temp_path = lease.path;
std::size_t written = std::min<std::size_t>(total > body_start ? total - body_start : 0,
                                            content_length);
bool ok = written == 0 || std::fwrite(buffer + body_start, 1, written, file) == written;
std::vector<char> chunk(65536);
while (ok && written < content_length) {
    const int received = api.recv(sock, chunk.data(),
        std::min<std::size_t>(chunk.size(), content_length - written), 0);
    if (received <= 0) { ok = false; break; }
    const auto count = static_cast<std::size_t>(received);
    ok = std::fwrite(chunk.data(), 1, count, file) == count;
    written += count;
}
if (std::fclose(file) != 0) ok = false;
prospero_image::EncodedFormat format{};
unsigned width = 0, height = 0;
const bool valid = prospero_image::inspect_file(temp_path.c_str(), &format, &width, &height);
if (!ok || written != content_length || !valid) {
    finish_image_upload(&lease, true);
    if (!ok || written != content_length)
        openai_error(api, sock, "400 Bad Request", "incomplete_upload",
                     "The image upload ended before Content-Length bytes arrived.");
    else
        openai_error(api, sock, "400 Bad Request", "invalid_image",
                     "Upload must be a decodable PNG or JPEG no larger than 2048 pixels per edge.");
    return 0;
}
finish_image_upload(&lease, false);
send_json(api, sock, nlohmann::json{{"id", lease.id}}.dump().c_str());
return 0;
```

- [ ] **Step 4: Implement leasing, expiry, and scoped cleanup**

Treat an upload as expired when `now - st_mtime >= 900` seconds; the file's last successful write starts its expiry window. Run expiry cleanup on upload and generation entry points. Protect the active-lease set, acquisition, and cleanup scan/unlink with the same mutex so cleanup cannot race a new lease. The active set covers both a stream being written and a generation using the upload; reject a second simultaneous lease for the same ID. On release, remove the upload only when the caller passed `remove=true`, otherwise leave it available until its original mtime expires. Deriving the path and age from the ID/file lets a retry survive an app restart without a client-supplied path.

Add a host-only `--test-image-upload-lifecycle` mode in `tests/http_api_harness.cpp`; it creates a feature-named fixture, sets its mtime to at least 900 seconds before an explicit future `std::time_t now`, acquires it, and proves cleanup skips the active lease but removes it after release. Create the isolated `PROSPERO_IMAGE_DIR` in `tests/test_http_api.py`, pass it as a compile definition, and add `src/image_codec.cpp` plus its vendored include directory to the HTTP harness compile command.

- [ ] **Step 5: Verify upload and lifecycle regression tests**

Run: `python3 tests/test_http_api.py -v`
Expected: all new upload cases pass, including no partial file after interrupted streams and a deterministic test that cleanup leaves an acquired upload intact until release.

### Task 3: Forward image input through runtime and Qwen context setup

**Files:**
- Modify: `include/gpt_runtime.hpp`
- Modify: `vulkan/http_server.cpp` (`run_generation`)
- Modify: `inference/runtime_adapter.hpp`
- Modify: `inference/router.cpp`
- Modify: `inference/adapters/registry.cpp`
- Modify: `inference/adapters/media/stable_diffusion_ps5.cpp`
- Modify: `tests/vulkan_router_test.cpp` to pin Qwen discovery without a projector
- Create: `include/qwen_image_editing.hpp`
- Create: `tests/qwen_image_editing_test.cpp`
- Create: `tests/test_qwen_image_editing.py`
- Modify: `tests/http_api_harness.cpp`
- Modify: `tests/test_http_api.py`

**Interfaces:**
- Append optional `const char *image_path = nullptr` to `gpt_runtime_generate(...)` and `run_generation(...)`; existing callers omit it and retain text-only behavior.
- Add `const char *image_path = nullptr` to `prospero::inference::GenerationRequest`.
- Pass that path to `ps5_sd_generate(root, prompt, image_path, ...)`; other modality adapters ignore it.
- `bool gpt_runtime_model_is_qwen_image_2_1(const char *model_id)` checks the selected installed model's core Qwen Image 2.1 files, without requiring the projector.
- `prospero_sd::qwen_projector_path(const char *root, bool image_edit, char *output, std::size_t capacity)` returns the optional projector path for edit requests and succeeds with an empty path for text-to-image.
- `std::string prospero_sd::qwen_context_identity(const char *root, const char *vision_path)` gives text and projector-enabled contexts distinct cache identities; `stable_diffusion_ps5.cpp` stores the full key in `std::string`, not a fixed-size root buffer.
- `ensure_context(root, image_edit)` includes the edit/projector state in its identity and sets `sd_ctx_params_t.llm_vision_path` only when `image_edit` is true.

- [ ] **Step 1: Add a harness assertion that image paths reach the runtime**

Update the deterministic `gpt_runtime_generate` stub in `tests/http_api_harness.cpp` to record its optional path to a test file. Add a chat-route test that sends a valid `image_id` and asserts the runtime received the path resolved by the server, not the ID or a caller-provided path. In `tests/qwen_image_editing_test.cpp`, use a temporary model directory to assert text-to-image succeeds with no projector, edit mode rejects the absent projector, and edit mode resolves the exact projector filename after the test creates it. In `tests/vulkan_router_test.cpp`, assert the Qwen model remains discoverable when its three core files exist without a projector, and adding the optional projector does not change its model identity.

```cpp
char root_template[] = "/tmp/prospero-qwen-edit-XXXXXX";
const char *root = ::mkdtemp(root_template);
assert(root);
const std::string model_root(root);
char projector_path[512]{};
assert(prospero_sd::qwen_projector_path(model_root.c_str(), false,
                                       projector_path, sizeof(projector_path)));
assert(projector_path[0] == '\0');
assert(!prospero_sd::qwen_projector_path(model_root.c_str(), true,
                                         projector_path, sizeof(projector_path)));
std::ofstream(model_root + "/mmproj-Qwen3VL-8B-Instruct-F16.gguf") << "fixture";
assert(prospero_sd::qwen_projector_path(model_root.c_str(), true,
                                        projector_path, sizeof(projector_path)));
assert(std::string(projector_path).ends_with("/mmproj-Qwen3VL-8B-Instruct-F16.gguf"));
assert(prospero_sd::qwen_context_identity(model_root.c_str(), "") !=
       prospero_sd::qwen_context_identity(model_root.c_str(), projector_path));
std::filesystem::remove_all(model_root);
```

In the existing `qwen_root` fixture in `tests/vulkan_router_test.cpp`, add:

```cpp
const std::string qwen_id_before = gpt_runtime_model_id(3);
const auto projector = qwen_root / "mmproj-Qwen3VL-8B-Instruct-F16.gguf";
assert(!std::filesystem::exists(projector));
std::ofstream(projector) << "fixture";
gpt_runtime_refresh_models();
assert(gpt_runtime_model_count() == 4);
assert(std::strcmp(gpt_runtime_model_id(3), qwen_id_before.c_str()) == 0);
```

- [ ] **Step 2: Run the forwarding test and confirm the expected failure**

Run: `python3 tests/test_http_api.py -v && python3 tests/test_qwen_image_editing.py -v`
Expected: the new forwarding and projector tests fail because image-ID resolution and the projector helper do not exist yet.

- [ ] **Step 3: Thread the optional request path through inference**

Add the optional parameter/field above, set `GenerationRequest::image_path` in `inference/router.cpp`, and pass it through `inference/adapters/registry.cpp`. Implement `gpt_runtime_model_is_qwen_image_2_1()` against the resolved model root so HTTP can reject non-Qwen image models before runtime work. Keep all text, audio, TTS, and existing image-generation callers on the null default.

```cpp
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &settings, char *output,
                         std::size_t capacity, gpt_runtime_stats_t *stats,
                         gpt_runtime_progress_fn progress, const char *image_path);
```

- [ ] **Step 4: Load the projector and bind the reference image only for Qwen edits**

Implement `qwen_projector_path()` and `qwen_context_identity()` in `include/qwen_image_editing.hpp` and use them in the adapter. Resolve and validate the optional projector before changing the cached context; require `mmproj-Qwen3VL-8B-Instruct-F16.gguf` only when an image path is present, set `parameters.llm_vision_path` only in that case, and use the full helper identity as the `std::string` context cache key. Changing between text and edit mode releases and recreates the context. Return a projector-specific diagnostic before context creation if the optional file is missing. After context setup succeeds, decode through `prospero_image::decode_file()`, create one `sd_image_t` whose data points at the owned decoded buffer, assign `parameters.ref_images` and `parameters.ref_images_count = 1`, and keep a scoped owner alive through `generate_image()` so every early return releases the pixels.

```cpp
struct DecodedImageOwner {
    prospero_image::DecodedImage image{};
    ~DecodedImageOwner() { prospero_image::free_image(&image); }
} decoded;
if (image_path && *image_path && !prospero_image::decode_file(image_path, &decoded.image)) {
    std::snprintf(response, response_capacity, "The reference image could not be decoded.");
    return 3;
}
sd_image_t reference{decoded.image.width, decoded.image.height,
                     decoded.image.channels, decoded.image.pixels};
parameters.ref_images = image_path && *image_path ? &reference : nullptr;
parameters.ref_images_count = parameters.ref_images ? 1 : 0;
const bool generated = generate_image(context, &parameters, &images, &image_count);
```

- [ ] **Step 5: Run runtime-focused checks**

Run: `python3 tests/test_http_api.py -v && python3 tests/test_image_codec.py -v && python3 tests/test_qwen_image_editing.py -v`
Expected: the API harness confirms exact path forwarding, codec ownership passes, and projector path tests prove the optional-file rule.

### Task 4: Persist normalized source images with conversations

**Files:**
- Modify: `include/session_store.hpp`
- Modify: `src/session_store.cpp`
- Modify: `tests/session_store_test.cpp`
- Modify: `tests/test_session_store.py`

**Interfaces:**
- Add optional `std::string image` to `prospero_session::Message`; old saved records without `image` remain readable and load with an empty value.
- Persist the reference in an optional `message-NNN.image` sidecar beside the existing message text file; an absent sidecar means an empty image field, so legacy message bytes remain untouched.
- `bool write_optional_image_sidecar(const char *id, unsigned index, const char *path)`, `bool read_optional_image_sidecar(const char *id, unsigned index, std::string *output)`, and `bool remove_optional_image_sidecar(const char *id, unsigned index)` use the conversation's own directory; a missing sidecar reads as empty, while an I/O or malformed-path failure returns false. Accept only a PNG path with the exact conversation-owned media prefix and generated filename pattern, including a leaf message index matching `index`, never an arbitrary client path.
- Add `archive_image(const char *conversation_id, unsigned message_index, const char *source, char *output, std::size_t output_capacity)`; it writes a normalized PNG in the conversation-owned media directory, persists its path in that message's sidecar, and returns the server-local path.
- Conversation deletion removes the sidecars and archived PNG through the existing scoped session and media cleanup.

- [ ] **Step 1: Add failing round-trip and lifecycle assertions**

Extend `tests/session_store_test.cpp` to save/load an optional `Message::image`, archive a JPEG source as PNG, verify the archived bytes have the PNG signature and decode to the original dimensions, then erase the conversation and assert the archived file is gone. Keep the existing text/audio archive assertions unchanged.

```cpp
const char *jpeg_path = "tests/fixtures/one-pixel.jpg";
char archived_path[320]{};
assert(archive_image(record.id, 0, jpeg_path, archived_path, sizeof(archived_path)));
written[0].image = archived_path;
assert(save(&record, written, 2));
assert(load(record.id, &loaded, read, 4));
assert(read[0].image == archived_path);
assert(read_file(archived_path).starts_with("\x89PNG\r\n\x1a\n"));
```

- [ ] **Step 2: Run the session-store test and confirm the expected failure**

Run: `python3 tests/test_session_store.py -v`
Expected: FAIL because messages do not yet persist image references and `archive_image` is absent.

- [ ] **Step 3: Persist backward-compatible image metadata**

Write an optional sidecar only when `Message::image` is nonempty; when reading, treat a missing sidecar as empty and reject a malformed path or a path outside `/download0/ProsperoAI/work/image/<conversation-id>/`. Require a leaf of `<message-index>-<decimal-timestamp>.png`. Update `erase()` to remove each `message-NNN.image` sidecar and its `.tmp` sibling while leaving legacy message text files byte-identical. Preserve the path in conversation load/save so same-console sync and export round-trip it without embedding bytes.

```cpp
if (!message.image.empty()) {
    if (!write_optional_image_sidecar(id, index, message.image.c_str()))
        return false;
} else if (!remove_optional_image_sidecar(id, index)) {
    return false;
}
if (!read_optional_image_sidecar(id, index, &message.image)) // Missing sidecar returns empty.
    return false;
```

- [ ] **Step 4: Normalize and archive source pixels as PNG**

Implement `archive_image()` using the shared codec and existing `open_output()` helper, which creates a unique file exclusively beneath `work/image/<conversation-id>/`. The codec writes into the returned `std::FILE *`; close it, release decoded pixels, then atomically persist the generated local path using `write_optional_image_sidecar()`. On any encode, close, or sidecar failure, remove only the exclusively created PNG and leave the previous sidecar unchanged. Reuse the existing conversation deletion boundary rather than adding a global media cleanup rule.

```cpp
prospero_image::DecodedImage decoded{};
if (!prospero_image::decode_file(source, &decoded))
    return false;
std::FILE *file = open_output(id, message_index, "image", ".png", output, output_capacity);
const bool encoded = file && prospero_image::write_png(file, decoded);
const bool closed = !file || std::fclose(file) == 0;
prospero_image::free_image(&decoded);
if (!encoded || !closed) {
    if (file) std::remove(output); // Only this invocation's exclusive file is removed.
    return false;
}
if (!write_optional_image_sidecar(id, message_index, output)) {
    std::remove(output);
    return false;
}
return true;
```

- [ ] **Step 5: Verify persistence and deletion**

Run: `python3 tests/test_session_store.py -v`
Expected: PASS for legacy records, optional image path round-trip, JPEG-to-PNG archival, and deletion of the archived copy.

### Task 5: Validate and orchestrate image-conditioned chat requests

**Files:**
- Modify: `vulkan/http_server.cpp` (`handle_openai_chat` and conversation routes)
- Modify: `include/gpt_runtime.hpp` and `inference/router.cpp` for Qwen model identification
- Modify: `tests/http_api_harness.cpp`
- Modify: `tests/test_http_api.py`

**Interfaces:**
- `/v1/chat/completions` accepts optional top-level `image_id` and `conversation_id`; a supplied image ID requires a final user message and an installed Qwen Image 2.1 model.
- With `conversation_id`, the server loads and validates the existing image conversation, archives the source as PNG against the final user message, persists its `image` path, and returns that path in `choices[0].message.image`.
- Without `conversation_id`, successful generation removes the upload without durable conversation media.
- Generation failure keeps the upload retryable until expiry; archive failure leaves the generated result and upload intact and returns an explicit archive error.
- `archive_image(id, message_index, source, output, capacity)` writes the normalized PNG and atomically stores the path in the same message sidecar; the conversation API accepts image fields only when they match that conversation's local PNG path format.
- The upload file is resolved from its ID and survives separate HTTP connections, so the one-request-per-process host harness can upload and then submit the returned ID in a second request without a test-only seed path.

- [ ] **Step 1: Add failing chat tests for pre-inference validation and cleanup**

Test unknown and expired IDs, an ID used with a non-Qwen model, a request whose final message is not from the user, a missing or non-image conversation ID, a failed generation followed by a successful retry with the same ID, and successful no-conversation cleanup. Assert rejected requests do not call the runtime stub. Ensure the web's existing pre-submit `persistConversation()` creates or updates the server-side image conversation before sending its `conversation_id`.

```python
upload_head, upload_body = self.request_raw(
    "/api/image/upload", self.image_fixture.read_bytes(), key="secret",
    headers=["Authorization: Bearer secret"], method="POST",
)
self.assertIn("200 OK", upload_head)
image_id = json.loads(upload_body)["id"]
head, body = self.request(
    body={"model": "qwen-image-test", "image_id": image_id,
          "messages": [{"role": "user", "content": "edit this"}]},
    key="secret", environment={"PROSPERO_TEST_IMAGE_PATH_FILE": str(self.image_path_file)},
)
self.assertIn("200 OK", head)
self.assertEqual(self.image_path_file.read_text(), str(self.image_fixture))
```

- [ ] **Step 2: Add failing conversation archival tests**

With a pre-saved image conversation, submit a successful edit and assert the response contains a PNG media path, the final user message sidecar stores that path, a later `/api/conversations/<id>` load returns it, and authenticated `GET` of the path returns the PNG bytes and image content type. Assert the upload is removed only after archival. Inject archive-write and sidecar-write failures and assert the response reports the archive failure, the generated output still exists, no orphan PNG remains, and the upload can be retried. Add an HTTP conversation sync round-trip assertion for `Message::image`; reject a path outside the conversation-owned PNG directory.

```python
message = json.loads(body)["choices"][0]["message"]
self.assertEqual(message["image"], self.expected_archived_png)
self.assertTrue(Path(self.expected_archived_png).read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
self.assertFalse(Path(self.upload_path).exists())
```

- [ ] **Step 3: Run focused chat and persistence tests before implementation**

Run: `python3 tests/test_http_api.py -v && python3 tests/test_session_store.py -v`
Expected: new image-ID, conversation, and archival cases fail before the route changes.

- [ ] **Step 4: Implement validation-before-inference and lease handling**

Parse only a string `image_id`; reject malformed, missing, expired, non-Qwen, non-user, and invalid conversation cases before calling `run_generation`. Acquire the upload lease, pass only the stored server path through Task 3's runtime parameter, and release it with `remove=false` on inference failure.

```cpp
if (has_image_id &&
    (!gpt_runtime_model_is_qwen_image_2_1(model) ||
     std::strcmp(roles[message_count - 1], "user") != 0)) {
    openai_error(api, sock, "400 Bad Request", "invalid_image_request",
                 "Image input requires a Qwen Image 2.1 user prompt.");
    return;
}
ImageUploadLease upload{}; // Its destructor releases the active lease on every early return.
if (has_image_id && !acquire_image_upload(image_id, &upload)) {
    openai_error(api, sock, "400 Bad Request", "invalid_image_id",
                 "The image upload is unknown or expired.");
    return;
}
const char *image_path = has_image_id ? upload.path.c_str() : nullptr;
```

- [ ] **Step 5: Archive on success and preserve retry semantics**

After successful generation, call `archive_image()` before removing the upload. If image encoding, sidecar persistence, or conversation persistence fails, retain the generated response/output and upload, remove only the source PNG created by this request, and return a structured explicit error. Remove the upload after successful archival, or immediately after successful generation when no conversation ID was sent. Return the path as `message.image` and include it in the server-side conversation record; conversation save/load routes must parse and serialize the optional image field.

```cpp
if (!generated) {
    openai_error(api, sock, "500 Internal Server Error", "generation_failed",
                 "Image editing did not complete; the upload remains available for retry.");
    return;
}
if (conversation_id &&
    !prospero_session::archive_image(conversation_id, message_index, image_path,
                                     archived_path, sizeof(archived_path))) {
    openai_error(api, sock, "500 Internal Server Error", "image_archive_failed",
                 "The image was generated, but its source could not be archived.");
    return;
}
finish_image_upload(&upload, true);
```

- [ ] **Step 6: Verify chat/API lifecycle tests**

Run: `python3 tests/test_http_api.py -v && python3 tests/test_session_store.py -v`
Expected: all validation, forwarding, retry, archive-failure, no-conversation cleanup, and persistence tests pass; existing text-to-image API tests remain unchanged.

### Task 6: Add the Qwen-only web attachment flow

**Files:**
- Create: `assets/web/qwen-image-attachment.js`
- Modify: `assets/web/index.html`
- Modify: `assets/web/app.css`
- Modify: `assets/web/app.js`
- Modify: `vulkan/http_server.cpp` static asset whitelist
- Create: `tests/web_qwen_image_attachment_test.mjs`
- Modify: `docs/QWEN_IMAGE_2_1.md`

**Interfaces:**
- `globalThis.ProsperoQwenImageAttachment` exposes `validateFile(file)` (returns an error string or `null`), `createAttachmentState()` (holds one file and its optional `uploadId`; `select(file)` resets the ID and `clear()` resets both), and `async uploadImage(file, api): Promise<string>`; `app.js` owns DOM rendering and calls the existing authenticated `api()` helper.
- Only the installed Qwen Image 2.1 model shows an accessible PNG/JPEG attachment control.
- The web request sends `image_id` and `conversation_id`; after success, the client stores `choices[0].message.image` on the final user message and renders that server media path as an image.

- [ ] **Step 1: Add UI state and submit tests**

In `tests/web_qwen_image_attachment_test.mjs`, verify PNG/JPEG accept, unsupported/over-16-MiB reject, preview state, remove, replace, upload only on submit, and that a failed generation retains both the selected file and its upload ID so retry reuses the same ID without another upload. A successful generation clears both. Assert image metadata is stored on the user message and not concatenated into prompt text; JSON export preserves only that local path and does not include image bytes. Check the file input's accessible name, the remove/replace controls' accessible names, and keyboard activation in the DOM test.

```js
const attachment = createAttachmentState();
assert.equal(validateFile(pngFile), null);
attachment.select(pngFile);
assert.equal(attachment.file, pngFile);
attachment.clear();
assert.equal(attachment.file, null);
assert.match(validateFile({type: 'image/gif', size: 10}), /PNG or JPEG/);
```

- [ ] **Step 2: Run the UI test and confirm the expected failure**

Run: `node --test tests/web_qwen_image_attachment_test.mjs`
Expected: FAIL because the attachment module and composer controls do not exist.

- [ ] **Step 3: Implement the attachment control and accessible preview**

Add a visually hidden `<input type="file" accept="image/png,image/jpeg">` with a visible labeled attach button and a preview region with remove and replace buttons. Hide the control for every model except Qwen Image 2.1. On model change, keep a selected file only if the new model remains Qwen Image 2.1; otherwise clear it and revoke the preview object URL. Update `addMessage()` to render `message.image` as an image for user messages, using the authenticated media fetch path and an accessible alt label.

- [ ] **Step 4: Upload only on submit and preserve retry behavior**

Before chat submission, POST the selected file bytes to `/api/image/upload` with the existing auth/session headers and `Content-Type: application/octet-stream` only when the attachment state has no `uploadId`; cache the returned ID in that state. Send the cached ID and current conversation ID with the normal chat JSON; on failure leave both file and ID selected for retry, and on success add the returned server PNG path to the final user message, render it as media, and clear the attachment state. Keep ordinary text-to-image requests unchanged.

```js
if (!attachment.uploadId) {
    attachment.uploadId = await uploadImage(attachment.file, api);
}
const requestBody = {...chatRequest, image_id: attachment.uploadId, conversation_id: current.id};
```

The existing `api()` helper adds the browser-session bearer token and defaults to JSON; its merged `headers` option must override `Content-Type` for this binary request. Add `/qwen-image-attachment.js` to the static whitelist in `vulkan/http_server.cpp` and load it before `/app.js` in `assets/web/index.html`.

- [ ] **Step 5: Document projector installation and run UI tests**

Update `docs/QWEN_IMAGE_2_1.md` with the official repository filename, optional-for-text-to-image behavior, and image-edit requirement. Run `node --test tests/web_qwen_image_attachment_test.mjs tests/web_model_preparation_test.mjs` and expect all attachment and existing model-preparation tests to pass.

```bash
node --test tests/web_qwen_image_attachment_test.mjs tests/web_model_preparation_test.mjs
```

- [ ] **Step 6: Render and inspect representative web states**

Before capturing, confirm `git rev-parse --git-path info/exclude` contains `/.amp/in/`; preserve existing rules and add only that exact entry if absent. Render the default Qwen composer with no attachment, the attached-image preview with remove/replace available, and a saved user message with its archived source image. Capture screenshots under `.amp/in/artifacts/` and inspect each with `view_media`; the control must be absent for non-Qwen models and usable by keyboard/screen reader.

### Task 7: Run full host checks, validate on PS5, then close verified issues

**Files:**
- Modify only if verification exposes a defect: files from Tasks 1–6
- No commit, push, merge, or main-title update is part of this task.

- [ ] **Step 1: Run focused checks and lint**

Run: `python3 tests/test_image_codec.py -v && python3 tests/test_http_api.py -v && python3 tests/test_session_store.py -v && python3 tests/test_qwen_image_editing.py -v && python3 tests/test_qwen_image_vram_budget.py -v && node --test tests/*.mjs && make lint`
Expected: every focused test and lint passes.

```bash
python3 tests/test_image_codec.py -v
python3 tests/test_http_api.py -v
python3 tests/test_session_store.py -v
python3 tests/test_qwen_image_editing.py -v
python3 tests/test_qwen_image_vram_budget.py -v
node --test tests/*.mjs
make lint
```

- [ ] **Step 2: Run the complete host suite**

Run: `HOST_CXX=clang++-18 HOST_CC=clang-18 make test`
Expected: the complete host test suite passes; report any pre-existing skip distinctly.

```bash
HOST_CXX=clang++-18 HOST_CC=clang-18 make test
```

- [ ] **Step 3: Build and inspect the PS5 app before deployment**

Read `docs/BUILDING.md`, run `make app`, then compare `readelf -d` NEEDED entries and program headers for the new app ELF with the currently verified test build. Do not deploy if a new dependency or program-header difference is unexplained.

- [ ] **Step 4: Resolve the configured test-title gate and deploy**

Run `python3 tools/ps5ctl.py doctor`; if `PS5_HOST` or `PROSPERO_TEST_TITLE` is unset, stop console actions and request that configuration. Otherwise confirm the target is not `PPSA99004`, inspect `agent procs`, deploy only to the configured test title using the documented folder workflow, and wait until `/api/settings/runtime` reports startup complete.

- [ ] **Step 5: Exercise text-to-image and image-to-image on-console**

On the test title, verify normal Qwen text-to-image with the optional projector present and absent; verify a PNG and a JPEG edit; check output and archived source PNG paths; reject a corrupt image and unknown/expired ID without inference; force a missing-projector edit and confirm its distinct error; verify expired cleanup and a failed-generation retry. Capture `/api/models/memory` and `/api/system/memory` before and after text/edit generation, retain the lowest observed flexible-memory and Vulkan-free snapshots available in diagnostics, and check `agent procs`, klog, and app trace to confirm the app remains alive under the measured VRAM budget.

- [ ] **Step 6: Recheck PR #43 comments and close only proven-fixed issues**

Read the latest two PR #43 comments and the linked issue state immediately before acting. Close issue #42 only if the host suite and the PS5 image-to-image case passed; close any other issue only when its reported behavior is reproduced as fixed by this change. Leave unresolved or unrelated issues open and report them.
