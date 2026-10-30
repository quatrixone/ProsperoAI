# Unified Workspace and Audio Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put Text, Audio, SD, and TTS in Workspace on web and native PS5 UI, preserve all Music Studio audio workflows, and show live Vulkan allocation in both headers.

**Architecture:** Add a narrow audio-workflow domain module for the audio library and the three runtime operations (music generation, Cover-to-ABC, and stems); retain existing HTTP routes as transport adapters. Web and native remain separate renderers and queue controllers over that shared module. Add one live Vulkan memory snapshot API and present its device-wide allocated/total values in both headers.

**Tech Stack:** C++20, embedded HTTP server, PS5 Vulkan runtime, native UI kit, browser JavaScript/HTML/CSS, Python unittest, Node.js test runner.

**Spec:** `docs/superpowers/specs/2026-10-28-unified-workspace-audio-design.md`

## Global Constraints

- Workspace exposes four modes: **Text**, **Audio**, **SD** (the existing image capability), and **TTS** (the existing voice capability).
- Each mode lists only installed models with the matching capability: `text-to-text`, `text-to-audio`, `text-to-image`, and `text-to-speech`, respectively.
- Changing mode filters choices but does not silently select or load a different model. Model choice and generation-option edits remain staged in UI state; request submission selects/loads the chosen model and applies the final options.
- Web Audio mode may upload into the shared library; native may only choose existing uploaded/generated audio-library files.
- Keep existing `/api/music/*` and generation endpoints; redirect `/music` to Workspace's Audio mode.
- Keep long-running GPU work serialized. Queue stop takes effect after the current item; a failed item is recorded and is not silently resubmitted.
- Cover and stems source validation must complete before model execution; runtime memory refusal must not crash or restart the title.
- Display live Vulkan allocation as device-wide `total - free`, not as a per-model estimate; omit the quantity when unavailable rather than displaying zero.
- Do not test or deploy on the console until all implementation and local validation are complete; ask the user before any console action. After approval, use only `PPSA99023`; do not reboot and never modify or deploy to `PPSA99004`.
- Preserve `work/audio/` files, metadata, ABC files, stems, and conversation history.

## Review Focus

- **A mode has no installed matching model:** show an empty state and do not select or prepare a different model. Pin this in both web mode-filter tests and native controller tests (Tasks 3 and 4).
- **A Cover/stems source is missing, unsupported, or contains traversal syntax:** refuse it before calling the runtime and leave all library files unchanged. Pin this in the shared workflow tests (Task 1).
- **A model load or generation is refused for memory pressure:** show a retryable error, release only the operation-owned runtime resources, and keep the title alive without requesting restart. Pin the service result and test-title run (Tasks 1 and 6).
- **A queue is stopped or one item fails:** finish the active item, preserve later pending items, and never automatically submit the failed item again. Pin this in web and native queue tests (Tasks 3 and 4).
- **Vulkan memory measurement is unavailable or the model unloads:** show unavailable/no-model state, not a fabricated `0 GiB` allocation; pin this in API and native state tests (Task 5).

---

### Task 1: Shared audio library and runtime operations

**Files:**
- Create: `include/audio_workflows.hpp`
- Create: `src/audio_workflows.cpp`
- Create: `tests/audio_workflows_test.cpp`
- Create: `tests/test_audio_workflows.py`

**Interfaces:**
- Produces `prospero::audio_workflows::LibraryEntry { std::string name; std::uint64_t size; std::int64_t modified; }`.
- Produces `prospero::audio_workflows::MusicRequest { std::string model_id, style, lyrics, abc_name, abc_content, track_name; unsigned seed, duration_seconds; }`.
- Produces `prospero::audio_workflows::Result { bool ok; bool retryable; std::string response; std::vector<std::string> artifacts; }` and `using ProgressCallback = std::function<void(std::string_view)>`.
- Produces `list_library(const std::string& root)`, `read_library_text(const std::string& root, const std::string& name, std::string& contents)`, `save_abc(const std::string& root, const std::string& name, const std::string& contents)`, `create_music(const MusicRequest&, ProgressCallback)`, `cover_to_abc(const std::string& model_id, const std::string& source_name, ProgressCallback)`, and `separate_stems(const std::string& model_id, const std::string& source_name, ProgressCallback)`.
- Runtime operations resolve the installed model ID to its current index/purpose, validate its capability, and use the existing runtime adapter. File operations accept a root so host tests use a temporary directory. The shared operations own runtime admission/serialization on `prospero::model_manager::runtime_mutex()` and atomically select the submitted model after source validation and pre-admission; callers must not preselect or hold the mutex. They return the adapter response plus validated artifact paths. Progress callbacks report validation, preparation, inference, and artifact-publication phases to either HTTP job status or native `State`. No queue manager or generic workflow framework is added; each client runs its queue sequentially.

- [ ] **Step 1: Add failing host tests for library validation and persistence**

Add tests that create a temporary audio root, list only regular supported audio/ABC files, reject `../escape.wav`, reject a missing source, round-trip a saved ABC file, and verify that a rejected write leaves an existing file byte-for-byte unchanged. The initial compile should fail because the shared header and functions do not exist.

```cpp
const auto files = prospero::audio_workflows::list_library(root.string());
assert(files.size() == 2);
assert(!prospero::audio_workflows::read_library_text(root.string(), "../escape.wav", text));
assert(!prospero::audio_workflows::save_abc(root.string(), "../escape.abc", "x"));
assert(read_bytes(root / "existing.wav") == original);
```

- [ ] **Step 2: Run the focused test and confirm it fails for the missing interface**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_audio_workflows -v`

Expected: FAIL at compile time because `audio_workflows.hpp` and the new operations have not yet been added.

- [ ] **Step 3: Implement the narrow domain API and runtime-operation tests**

Implement strict basename/extension validation, regular-file checks, bounded ABC text reads/writes, and the three direct runtime operations. Use existing `gpt_runtime_generate`, `gpt_runtime_model_id`, `gpt_runtime_model_purpose`, `runtime_settings.hpp`, and `model_manager::runtime_mutex`; preserve prior runtime settings after each operation. Mode/model choice is staged only in the UI. On submit, validate the source and perform applicable memory pre-admission before selecting the requested model under the same runtime lock that protects generation; a preflight refusal must not change the current selection. If the adapter refuses after submit-time selection, return a retryable failure, clean up only requested-operation resources, and never request an application restart. Add fake-runtime tests for preflight refusal without selection, selection/generation serialization, selected model ID, prompt/options, progress phases, and ordinary adapter failure.

```cpp
namespace prospero::audio_workflows {
struct LibraryEntry {
    std::string name;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};
struct MusicRequest {
    std::string model_id, style, lyrics, abc_name, abc_content, track_name;
    unsigned seed = 42, duration_seconds = 0;
};
struct Result {
    bool ok = false;
    bool retryable = false;
    std::string response;
    std::vector<std::string> artifacts;
};
using ProgressCallback = std::function<void(std::string_view)>;
std::vector<LibraryEntry> list_library(const std::string& root);
bool read_library_text(const std::string& root, const std::string& name, std::string& contents);
bool save_abc(const std::string& root, const std::string& name, const std::string& contents);
Result create_music(const MusicRequest& request, ProgressCallback progress);
Result cover_to_abc(const std::string& model_id, const std::string& source_name,
                    ProgressCallback progress);
Result separate_stems(const std::string& model_id, const std::string& source_name,
                      ProgressCallback progress);
}
```

- [ ] **Step 4: Run the focused service tests and confirm they pass**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_audio_workflows -v`

Expected: PASS, including path rejection without runtime invocation and the fake-runtime success/failure cases.

- [ ] **Step 5: Run formatting and stage the service as an independently reviewable change**

Run: `make format-check`

Expected: PASS. Review the diff for duplicated filesystem/runtime behavior before continuing; commit this task only after its tests and diff review pass.

### Task 2: Preserve HTTP compatibility and redirect the old page

**Files:**
- Modify: `vulkan/http_server.cpp` (`serve_ui_asset`, `handle_generate`, `/api/music/*` dispatch)
- Modify: `tests/test_http_api.py` to compile/link the shared operation source into the harness
- Modify: `tests/http_api_harness.cpp` only to supply deterministic audio-model and runtime fixtures
- Modify: `tools/build-vulkan-native.sh` if the new shared source is not already included in the native target

**Interfaces:**
- Consumes the exact `prospero::audio_workflows` interface from Task 1.
- Keeps `GET /api/music/files`, `GET /api/music/stems.zip`, `POST /api/music/abc`, `POST /api/music/meta`, `POST /api/music/upload`, `POST /api/music/uploadbin`, `POST /api/music/rename`, `POST /api/music/master`, and `DELETE /api/music/file`.
- `/api/generate` accepts an optional audio workflow discriminator and typed audio request fields for `create`, `cover`, or `stems`; requests without that discriminator retain the current Ollama-compatible behavior. Async requests still return a `job_id` and use `/api/jobs` for polling.
- Audio model choice is staged in the client until submission. The `/api/generate` execution path invokes the shared operation only at submission; that operation atomically selects the requested model after validation/admission, while mode/model choice UI handlers never select or prepare it.
- `GET /music` redirects to `/?mode=audio`; it no longer serves `music.html`.

- [ ] **Step 1: Add failing HTTP tests for compatibility, shared operation dispatch, and redirect**

Extend the actual handler harness to assert that `/music` returns a redirect with `Location: /?mode=audio`, the legacy `/api/music/files` and upload/ABC routes still work, and audio `POST /api/generate` routes to the fake shared operation exactly once. Create `uploaded.wav` under `self.audio_dir` for the valid source case. Instrument the harness runtime stub with a test-only file-backed call counter; initialize `self.audio_calls_file` in `setUp()` under the existing temporary test directory, set `PROSPERO_TEST_AUDIO_CALLS_FILE` for each HTTP subprocess, add `HttpApiTests.audio_generate_calls()` to read the counter (returning zero before the file exists), and pass the environment variable through `request(..., environment=...)`. Test a valid synchronous source increments the counter once and a traversal source does not increment it. Also submit an async audio job, assert it returns a `job_id`, poll to a terminal state using the existing job helper, and assert the fake runtime was called exactly once.

```python
head, _ = self.request_raw("/music", b"", method="GET")
assert "302 Found" in head
assert "Location: /?mode=audio" in head

initial_calls = self.audio_generate_calls()
head, body = self.request("/api/generate", {
    "async": False, "audio_workflow": "cover", "model": "sheetsage-test",
    "source": "uploaded.wav",
}, environment={"PROSPERO_TEST_AUDIO_CALLS_FILE": str(self.audio_calls_file)})
assert "200 OK" in head
assert self.audio_generate_calls() == initial_calls + 1

calls_before = self.audio_generate_calls()
head, body = self.request("/api/generate", {
    "async": False, "audio_workflow": "cover", "model": "sheetsage-test",
    "source": "../outside.wav",
}, environment={"PROSPERO_TEST_AUDIO_CALLS_FILE": str(self.audio_calls_file)})
assert "400 Bad Request" in head
assert self.audio_generate_calls() == calls_before
```

- [ ] **Step 2: Run the focused HTTP tests and confirm the new cases fail**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api.HttpApiTests.test_music_route_redirects_to_workspace_audio test_http_api.HttpApiTests.test_audio_workflow_dispatch_uses_shared_operations -v`

Expected: FAIL because the old `/music` asset route and generic-only generation handler are still active.

- [ ] **Step 3: Adapt handlers to the shared module without removing routes**

Branch only on a recognized `audio_workflow` string, validate typed request fields, and dispatch to the corresponding Task 1 function on the request execution path; the shared operation performs submit-time model selection atomically after source validation and pre-admission. Map its `Result` into the existing synchronous/async job JSON. Route existing library listing and ABC read/write through shared file operations. Keep legacy endpoints and response shapes intact. Remove only the `/music`, `/music.css`, and `/music.js` page asset entries; add an explicit redirect response for `/music`.

```cpp
if (workflow == "cover")
    result = prospero::audio_workflows::cover_to_abc(model_id, source_name, progress);
else if (workflow == "stems")
    result = prospero::audio_workflows::separate_stems(model_id, source_name, progress);
else if (workflow == "create")
    result = prospero::audio_workflows::create_music(music_request, progress);
else {
    send_status(api, sock, "400 Bad Request", "application/json",
                "{\"error\":\"Unsupported audio workflow.\"}");
    return;
}
```

- [ ] **Step 4: Run the complete HTTP API tests**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api -v`

Expected: PASS; legacy upload collision/interruption tests, mastering correctness, new redirect, and new dispatch tests all pass.

- [ ] **Step 5: Build the HTTP/native target and review the adapter diff**

Run: `source tools/ps5-env.sh && make vulkan-runtime`

Expected: PASS with `audio_workflows.cpp` linked into the PS5 target and no removed public route. Review the handler diff for unchanged auth, size limits, and job ownership.

### Task 3: Move the web workflows into Workspace

**Files:**
- Modify: `assets/web/index.html`
- Modify: `assets/web/app.js`
- Modify: `assets/web/app.css`
- Create: `assets/web/workspace-audio.js`
- Create: `tests/web_workspace_audio_test.mjs`
- Delete: `assets/web/music.html`
- Delete: `assets/web/music.css`
- Delete: `assets/web/music.js`

**Interfaces:**
- Consumes Task 2's existing `/api/music/*` library routes and async `/api/generate` + `/api/jobs` transport.
- `app.js` owns workspace mode and model selection. It maps `Text → text-to-text`, `Audio → text-to-audio`, `SD → text-to-image`, `TTS → text-to-speech` using `model.purpose` only.
- `workspace-audio.js` owns the Audio workspace form, library chooser/upload, Cover, stems, ABC editor, and queue display; it receives the existing authenticated `api(path, options)` function and a completion callback from `app.js`.
- The Audio page uses the existing audio directory and API routes. It does not create or retain a separate Music Studio page.

- [ ] **Step 1: Add failing Node tests for capability filtering, staged selection/options, and queue boundaries**

Test the extracted workspace-mode helper with one model for each `purpose`, an empty capability list, and a mode or model choice while another runtime model is loaded. Assert neither action calls runtime selection/preparation; option edits remain staged. On submit, assert the selected model is selected/prepared exactly then and the final options reach the request. Test queue logic with a fake operation: queued variants use consecutive seeds; the first item fails, later items remain pending, and Stop prevents submission of the next item but lets the active promise settle.

```js
assert.deepEqual(filterModels(models, 'audio').map(model => model.id), ['audio-1']);
assert.deepEqual(filterModels(models, 'sd'), []);
assert.equal(selectModelCalls, 0);
assert.equal(prepareModelCalls, 0); // model choice only stages UI state
assert.deepEqual(queue.map(item => item.status), ['failed', 'pending']);
```

- [ ] **Step 2: Run the focused web test and confirm it fails**

Run: `node --test tests/web_workspace_audio_test.mjs`

Expected: FAIL because the workspace-mode/audio controller module does not exist.

- [ ] **Step 3: Add four modes and mount the existing audio workflows inside Workspace**

Add accessible Text/Audio/SD/TTS controls and filter the Workspace model selector by `purpose`. Preserve the current model and conversation; mode/model choices only update local state. Do not call the model-selection/preparation API or change runtime settings until submit. At submit, select/prepare the staged model and send its final options with the request. Move the old audio controls and their existing feature set into the Workspace Audio panel, using `workspace-audio.js` and Workspace styling. Keep local audio upload web-only. Use Task 2 shared operations for music/Cover/stems; preserve local queue persistence and update queue state before/after each operation. On queue stop, do not abort the active server job. Remove the nav link and old page assets. On load, consume `?mode=audio` from the `/music` redirect and open Audio mode.

```js
const MODE_PURPOSE = {
    text: 'text-to-text', audio: 'text-to-audio',
    sd: 'text-to-image', tts: 'text-to-speech',
};
function filterModels(models, mode) {
    return models.filter(model => model.purpose === MODE_PURPOSE[mode]);
}
```

- [ ] **Step 4: Run web tests and render default, Audio, and empty-model states**

Run: `node --test tests/*.mjs`

Expected: PASS. Start the repository's web preview, open Workspace in a browser, capture the default Workspace and Audio mode, and capture Audio with no installed audio model. Inspect all three captures with `view_media`; verify the four modes are readable, Audio includes upload/Cover/stems/ABC/queue controls, no standalone Music Studio navigation remains, and the empty state does not suggest a different model was selected.

- [ ] **Step 5: Run lint and review web assets**

Run: `make lint`

Expected: PASS. Search `assets/web` and the embedded asset map for stale `music.html`, `music.css`, and `music.js` references; the `/api/music/*` API names are expected to remain.

### Task 4: Add matching native Workspace modes and audio workflows

**Files:**
- Modify: `include/native_app.hpp`
- Modify: `src/native_app.cpp`
- Modify: `src/native_ui_impl.hpp`
- Modify: `src/native_ui.cpp`
- Modify: `src/native_ui_screens.cpp`
- Modify: `tests/native_controller_test.cpp`
- Modify: `tests/test_native_controller.py` to link the shared operation source into the controller harness
- Modify: `tools/build-vulkan-native.sh` only if not already updated in Task 2

**Interfaces:**
- Consumes the Task 1 audio workflow API and existing `App` single-worker serialization.
- Adds `std::vector<unsigned> App::model_indices_for(Capability) const`, `start_music(const prospero::audio_workflows::MusicRequest&)`, `start_cover(const std::string& model_id, const std::string& source_name)`, and `start_stems(const std::string& model_id, const std::string& source_name)`. Long operations run on the existing App worker; `App::busy()` prevents a second heavy operation.
- Native mode and model choices update staged UI state only; neither calls runtime selection/preparation. The App worker selects/prepares the staged model and applies its final options only when the user submits a request.
- Native queue state is transient UI state. It runs one item at a time, records `pending/running/done/failed`, and Stop takes effect after the current worker operation.

- [ ] **Step 1: Add failing native controller tests for mode filtering and audio worker actions**

Extend deterministic runtime fixtures to expose text/audio/image/voice models. Assert `model_indices_for()` returns only matching models, changing mode or staging a model does not call `gpt_runtime_select_model` or prepare the runtime, submitting selects/prepares the staged model and passes final options, a selected library source reaches the shared fake operation, a failed operation publishes an error, and a busy worker rejects a second operation. Add cases for each capability and for an empty matching set.

```cpp
assert(app.model_indices_for(Capability::Audio) == std::vector<unsigned>{audio_index});
assert(runtime_select_calls == 0); // mode/model choice is staged, not loaded
assert(runtime_prepare_calls == 0);
assert(app.start_cover("sheetsage-test", "uploaded.wav"));
settle(app);
assert(last_cover_source == "uploaded.wav");
const auto runtime_calls = generation_calls;
assert(app.start_stems("htdemucs-test", "missing.wav"));
settle(app);
assert(generation_calls == runtime_calls); // missing source fails before runtime execution
```

- [ ] **Step 2: Run the focused controller test and confirm it fails**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_native_controller.NativeController.test_workspace_modes_and_audio_actions -v`

Expected: FAIL because the App mode and audio-workflow actions are not defined.

- [ ] **Step 3: Add App worker actions and the controller-first native UI**

Add audio actions as worker jobs in `App::Job`; call the shared Task 1 operations only from `App::work()`. Publish progress, stable operation status, and a result to `State`, and return model/runtime errors without a restart path. Add Text/Audio/SD/TTS mode selection and matching capability-filtered model choices. Mode/model choice updates staged App state only; in the request worker, select/prepare the staged model and apply its final options immediately before generation. In Audio mode, add Create music, Cover to ABC, Split into stems, and Queue variants panels; queue variants use consecutive seeds. Reuse the current controller navigation, `ui::Form`, `ui::Keyboard`, and audio-library entries. Native audio source and ABC choices come exclusively from `work/audio/`; do not add a file upload or general filesystem browser.

```cpp
enum class WorkspaceMode : std::uint8_t { Text, Audio, SD, TTS };
static constexpr Capability capability_for(WorkspaceMode mode)
{
    switch (mode) {
    case WorkspaceMode::Audio: return Capability::Audio;
    case WorkspaceMode::SD: return Capability::Image;
    case WorkspaceMode::TTS: return Capability::Voice;
    default: return Capability::Text;
    }
}
```

- [ ] **Step 4: Run controller tests and inspect native UI states**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_native_controller.NativeController.test_workspace_modes_and_audio_actions -v`

Expected: PASS under the existing address/undefined-behavior sanitizers. Confirm the controller tests prove model choice and option edits do not select/prepare a model, while submit does. Do not deploy or interact with the console during this task; native visual verification is deferred to Task 6 after the user approves console testing.

- [ ] **Step 5: Review native input, queue, and worker lifecycle**

Run: `make format-check`

Expected: PASS. Review that UI code never calls a runtime operation synchronously, queue stop does not cancel the active job, mode selection does not load, and a failed queue entry remains visible with later entries pending.

### Task 5: Publish and display live Vulkan allocation in both headers

**Files:**
- Modify: `include/gpt_runtime.hpp`
- Modify: `include/runtime_vulkan.hpp`
- Modify: `inference/adapters/llama/vulkan.cpp`
- Modify: `inference/router.cpp`
- Modify: `vulkan/http_server.cpp`
- Modify: `include/native_app.hpp`
- Modify: `src/native_app.cpp`
- Modify: `src/native_ui.cpp`
- Modify: `assets/web/app.js`
- Modify: `assets/web/index.html`
- Modify: `tests/http_api_harness.cpp`
- Modify: `tests/test_http_api.py`
- Modify: `tests/native_controller_test.cpp`
- Create: `tests/web_memory_status_test.mjs`

**Interfaces:**
- Adds `gpt_runtime_vulkan_memory(std::uint64_t* free_bytes, std::uint64_t* total_bytes) -> bool`; returns false when the Vulkan device is unavailable or the runtime cannot report a snapshot. Callers calculate allocated bytes as `total_bytes - free_bytes` only when the snapshot is valid and `free_bytes <= total_bytes`.
- Extends `/api/models/memory` with `vulkan_free_bytes`, `vulkan_allocated_bytes`, `vulkan_memory_available`, and existing `vulkan_heap_bytes` (total). Preserve existing fields; set the new free/allocated byte values to JSON `null` when the snapshot is unavailable, and require consumers to check the availability flag before displaying a value.
- Native `State` stores a valid/unavailable snapshot refreshed at most once per second by a lightweight job on the existing serialized App worker (never from rendering); web refresh continues through the existing 2-second memory poll.

- [ ] **Step 1: Add failing API, browser, and native tests for live memory status**

Use fake Vulkan snapshots for `(free=6 GiB,total=10 GiB)`, unavailable, and no resident model. Assert the API reports `allocated=4 GiB` and `null` for unavailable free/allocated fields; browser and native labels show the loaded model plus `4 / 10 GiB` only for a valid resident snapshot; unavailable data never appears as `0 GiB`; unloading restores the no-model message. Add a `test_native_vulkan_memory_snapshot` controller test for the native state/render inputs.

```python
payload = json.loads(self.request("/api/models/memory")[1])
assert payload["vulkan_memory_available"] is True
assert payload["vulkan_allocated_bytes"] == 4 * 1024**3
assert payload["vulkan_heap_bytes"] == 10 * 1024**3
```

- [ ] **Step 2: Run the focused memory tests and confirm they fail**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api.HttpApiTests.test_model_memory_status -v && node --test tests/web_memory_status_test.mjs`

Expected: FAIL because the API and headers do not yet expose allocated Vulkan bytes.

- [ ] **Step 3: Add one backend snapshot function and render only valid live values**

Implement the runtime snapshot using one `ggml_backend_dev_memory` call; make `gpt_runtime_vulkan_heap_bytes()` use the same source of truth for total capacity. Add the API fields without removing current memory fields. Cache the native snapshot at one-second intervals by scheduling the query on the existing App worker; keep the current web two-second poll. Format as device-wide Vulkan allocation (for example, `4.0 / 10.0 GiB allocated`) next to the loaded model name and preserve `Memory: no model loaded` when unloaded.

```cpp
bool gpt_runtime_vulkan_memory(std::uint64_t* free_bytes, std::uint64_t* total_bytes)
{
    auto* device = ggml_backend_dev_by_name("Vulkan0");
    if (!device || !free_bytes || !total_bytes) return false;
    size_t free = 0, total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    if (free > total) return false;
    *free_bytes = free;
    *total_bytes = total;
    return total != 0;
}
```

- [ ] **Step 4: Run focused tests and inspect both rendered headers**

Run: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api.HttpApiTests.test_model_memory_status test_native_controller.NativeController.test_native_vulkan_memory_snapshot -v && node --test tests/web_memory_status_test.mjs`

Expected: PASS. Render the web header and native PS5 header with a model loaded, unavailable memory reporting, and after unload. Inspect each capture; ensure labels fit their header areas, name device-wide allocation, and do not show a stale loaded-model allocation after unload.

- [ ] **Step 5: Run formatting and review telemetry semantics**

Run: `make format-check`

Expected: PASS. Verify the displayed value is `total - free`, is not described as model-only, and is omitted when the runtime snapshot is invalid.

### Task 6: Full integration, pinned build, and safe test-title proof

**Files:**
- Modify only failing tests or implementation files from Tasks 1–5.
- Store screenshots/log evidence under `.amp/in/artifacts/` after confirming `/.amp/in/` is present in the repository-local exclude file from `git rev-parse --git-path info/exclude`.

**Interfaces:**
- Integrates web and native renderers with the shared workflow module and the existing PS5 runtime.
- No console testing or deployment occurs until all implementation and local checks are complete and the user has approved it. The only permitted test target after approval is `PPSA99023`; do not reboot or touch `PPSA99004`.

- [ ] **Step 1: Run the targeted and full host checks**

Run:

```bash
HOST_CXX=clang++-18 HOST_CC=clang-18 make test
node --test tests/*.mjs
make lint
```

Expected: all commands pass; preserve and report any unrelated skipped Vulkan-runtime test clearly.

- [ ] **Step 2: Build and check dependency pins and executable ABI**

Before the build, read `docs/BUILDING.md`. Then run: `source tools/ps5-env.sh && make app && make deps-status`

Expected: app build succeeds, llama.cpp/audio.cpp/Stable Diffusion inputs match `tools/deps.json`, and the ELF NEEDED list plus program-header structure match the known working build. Keep all generated build output under `build/`.

- [ ] **Step 3: Prepare web workflow checks; run only after user approval**

The web test procedure is reserved for after the user approves console testing at the end of Task 6. Then verify all four capability filters, staged model/option changes with no runtime load before submit, upload one test WAV, complete music generation, Cover-to-ABC, stems, and a two-item queued generation. Confirm the current item completes after Stop, a failed item is not retried, ABC is editable, stems play/download, artifacts appear in the same audio library, the title remains listed in `agent procs`, and header allocation updates while the model is resident and after unload.

- [ ] **Step 4: Prepare native workflow checks; run only after user approval**

The native test procedure is reserved for after the user approves console testing at the end of Task 6. Then, on native `PPSA99023`, choose the uploaded WAV from the shared library and complete Cover-to-ABC and stems; generate one music track and run a two-item queue. Confirm the same artifacts appear in web and native library views, no native upload exists, model/options remain staged until submit, memory refusal does not crash/restart the title, and the live allocation display returns to no-model after unload.

- [ ] **Step 5: Verify compatibility, persistence, and final UI captures**

Request `/music` locally and confirm it redirects to Workspace Audio. Confirm existing audio files, metadata, ABC, stems, and conversation records remain present through host/API checks. Capture and inspect final local web default/Audio/empty-model/memory states; defer native screenshots to the approved console run. Review `git diff --check` and the complete diff. Only after this work is complete, ask the user whether to run the prepared console tests on `PPSA99023`; do not push or deploy to `PPSA99004`.

---

## Plan self-review

- **Spec coverage:** Tasks 1–2 cover shared operations, validation, serialization, artifacts, HTTP compatibility, and `/music` redirect. Tasks 3–4 cover four-mode filtering, empty states, matching Audio controls, upload-only-on-web, queues, and both UI renderers. Task 5 covers the live device-wide memory display in both headers. Task 6 covers host/build/PS5 verification, persistence, and safe test-title limits.
- **Failure coverage:** Every item in Review Focus has a named test or test-title exercise in its owning task.
- **Dependency ordering:** Task 1 defines the shared API; Task 2 binds HTTP transport; Tasks 3 and 4 consume the stable service and transport contract; Task 5 adds telemetry to both clients; Task 6 verifies the integrated result.
- **Scope boundary:** No model catalog replacement, native upload, general filesystem browser, dependency pin update, generated media format change, conversation migration, or title reboot is planned.
