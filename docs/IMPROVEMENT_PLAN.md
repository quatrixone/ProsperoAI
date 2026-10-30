# ProsperoAI improvement plan

This plan records ten focused improvements for the native PS5 app, hosted web UI,
and HTTP API. It is a roadmap; it does not claim that the work below has been
implemented.

## 1. One model manager for every interface

**Problem.** The native app serializes model work through its `App` worker, while
`/api/models/selection` currently calls the runtime directly. That gives the native
state and API separate paths for selecting and preparing a model. Requests from the
web UI, API clients, and controller can therefore disagree about which model is
selected or whether it is ready, and can race with other runtime work.

**Plan.** Add one model manager that owns model selection, preparation, unload, and
the published state (`selected`, `loading`, `ready`, or `failed`). Make native UI,
web UI, and API submit requests to it. Serialize transitions with inference and
downloads, and return the same status and error details to all callers. Keep
conversation model identity separate from the currently loaded runtime so switching
the runtime does not silently change an existing conversation.

**Acceptance criteria.** Selecting Chatterbox from any interface produces the same
selected and ready state in all interfaces. Concurrent selection during generation
returns a clear busy response or queues safely. A failed preparation leaves the
previous conversation readable and gives every interface the same actionable error.

## 2. Make model selection easier to use

**Problem.** The native Workspace model picker is a long list, and a selection uses
confirmation followed by a potentially slow load. It is easy to mistake the
confirmation or preparation step for a failed selection.

**Plan.** Add search and capability filters to the Workspace picker. Show distinct
selected, loading, ready, and failed states, with progress where the runtime can
report it. Keep the current model usable until the replacement is ready, and explain
whether retrying will reuse an already selected model. Apply the same state labels in
the web UI.

**Acceptance criteria.** A user can find a model by name or capability, confirm it
once, see that it is loading, and tell whether it became ready or failed. The picker
does not imply that a model is ready merely because it is selected.

## 3. Warn about model memory needs before loading

**Problem.** Text, image, audio, and speech workloads share limited console memory.
Model changes and larger settings can fail during preparation or put stability at
risk.

**Plan.** Track measured memory use and preparation outcomes by model and settings.
Combine those measurements with current free-memory information to show a
conservative estimate before loading or downloading. If reliable estimates are not
available, say so rather than presenting a guessed number. On a failed allocation,
release partial resources and preserve the last usable app state.

**Acceptance criteria.** Estimates are based on recorded console measurements and
identify their confidence. Low-memory cases warn before preparation when possible;
failed preparation returns the app to a usable state and records the failure.

## 4. Protect the API without disabling the browser UI

**Problem.** The API has optional bearer-key authentication, but the hosted browser
UI does not send that key. Enabling API authentication therefore also blocks the
first-party web interface. The API is plain HTTP and is documented for trusted local
networks.

**Plan.** Add a browser-specific authorization flow, such as a short-lived,
origin-scoped session established from the console UI, while retaining bearer keys
for external clients. Avoid exposing long-lived API keys to browser storage. Make
the listener's network exposure and authentication state visible in Settings, and
document the trusted-network limitation until transport encryption is available.

**Acceptance criteria.** With API authentication enabled, the first-party browser
UI can connect through a short-lived session and external clients still require the
configured bearer key. Unauthorized API requests remain rejected.

## 5. Improve crash and failure diagnostics

**Problem.** Detailed debug tracing is opt-in. When a title crashes or model
preparation fails, the information needed to distinguish a model error, memory
pressure, or runtime failure may be missing.

**Plan.** Keep a small bounded diagnostic journal for app lifecycle, model
transitions, allocation failures, and API errors. Record timestamps, operation IDs,
model IDs, and error codes, but no prompts, generated content, API keys, or voice
recordings. Preserve the journal across restarts and expose an export action in
Diagnostics. Keep verbose runtime tracing opt-in.

**Acceptance criteria.** A restart after a crash retains the last completed model
transition and relevant error code. The journal is size-bounded, contains no user
content or secrets, and can be exported from the app.

## 6. Make native generation settings easier to understand

**Problem.** The native Generation settings expose controls across multiple model
types, but terms such as context, KV cache, seed, dimensions, duration, and output
limit are not equally clear to every user. Model-specific settings are easier to
misconfigure when their effect is not explained in place.

**Plan.** Group settings by the selected model's capability and use plain-language
labels with short explanations and useful defaults. Separate common controls from an
Advanced section, keep valid ranges visible, and explain when a setting applies only
to image, audio, speech, or text generation. Preserve the existing API's precise
values and validation.

**Acceptance criteria.** A user can tell what each visible setting changes, which
model types use it, and how to return to the default. Irrelevant controls are hidden
or clearly marked as inactive, and saved/API values are reflected consistently.

## 7. Reduce app GPU memory use and build time

**Problem.** The application shares a constrained address space and GPU memory with
its inference runtimes. Separately, native builds include large runtime and UI
components; slow rebuilds make iteration and release work take longer.

**Plan.** Measure app-owned GPU allocations and peak memory by screen and workload.
Release unused UI textures and intermediate buffers promptly, reduce retained
resource sizes where quality is unaffected, and avoid duplicating assets in memory.
For build speed, measure clean and incremental builds, then improve dependency
tracking, caching, and parallel compilation so unchanged components are not rebuilt.
Keep reproducible output and correctness checks intact.

**Acceptance criteria.** Before/after measurements show lower app-owned GPU memory
use on representative screens and workloads, without changing generated output
quality or causing regressions. Clean and incremental build timings are recorded;
incremental changes rebuild only affected components and produce the same verified
app artifacts.

## 8. Make model downloads storage-aware

**Problem.** Curated model downloads can be large, and a download can fail late if
the console does not have enough free storage. Users need a clear view of the space
required and the models already taking space.

**Plan.** Show the expected download size and available space before starting. Keep
the existing resumable-download behavior, explain when a partial download can be
resumed, and provide clear retry and cleanup actions. Verify downloaded files before
making a model available, and never remove unrelated model data automatically.

**Acceptance criteria.** A user sees a storage preflight before a large download,
can resume a supported interrupted transfer, and receives a specific error when
space or integrity checks fail. Cleanup always names the exact files it will remove.

## 9. Improve conversation and media management

**Problem.** Conversations can accumulate generated text, images, and audio. As the
history grows, finding a specific result or organizing work becomes harder, and
generated media is not as easy to manage independently from its conversation.

**Plan.** Add conversation search and practical organization actions such as rename,
pin/archive, and export/import. Provide a media view for browsing image and audio
outputs with their source conversation and model, plus safe delete and export
actions. Keep saved conversation data readable if an associated model is removed.

**Acceptance criteria.** Users can find conversations by title or content, export
and restore a conversation, and browse or export generated media without losing its
conversation link. Deleting one item does not silently delete unrelated history or
outputs.

## 10. Expand accessibility and localization

**Problem.** The interface has reduced motion, high contrast, and a larger text
setting, but the experience can still be difficult for users who need more
controller guidance, clearer focus, or a UI language other than English. Some
scripts also need shaping and right-to-left layout in addition to glyph coverage.

**Plan.** Review every page and dialog for visible controller focus, readable
contrast, scalable layouts, and clear action labels. Add selectable UI languages
with translated core flows, then support text shaping and right-to-left layout for
languages that require them. Keep the existing reduced-motion, high-contrast, and
text-size settings available throughout.

**Acceptance criteria.** Core tasks can be completed with controller-only
navigation at supported text sizes and contrast settings. Translated pages do not
clip labels, and supported right-to-left scripts render in the correct order with
shaping.

## Current implementation snapshot (PR #39)

The first implementation pass adds foundations across all ten areas. It does not yet satisfy
every acceptance criterion above. In particular, safe console storage telemetry, per-model fit
warnings, the short-lived browser authorization flow, complete crash lifecycle coverage, media
organization, and translated/right-to-left flows remain open.

| # | Implemented in this pass | Remaining acceptance work |
| --- | --- | --- |
| 1 | Shared native/API model selection coordinator, runtime lock, and published state. Busy selection returns a retryable conflict. Host coverage verifies native recovery after failed preparation and API prepare-error/busy responses; a PS5 Mistral generation returned HTTP 200 while a concurrent selection returned HTTP 409 busy. | All planned acceptance checks are verified. |
| 2 | Native Workspace picker supports name search and L1/R1 capability filtering, with selected/loading/ready distinction. Web model cards poll the same status. | Percentage progress (the runtime currently exposes stage only) and native visual verification. |
| 3 | Live Vulkan/direct/flexible-memory snapshot plus persistent model/context memory samples after successful preparation. Native and web model confirmation dialogs show only samples matching context, output limit, and thinking; missing matches explicitly report that a reliable estimate is unavailable. Web shows the installed GGUF size; repository browsing shows remote GGUF size with an explicitly heuristic fit badge. PS5 API verification returned the 4K match and no estimate for 8K. | Visually verify the dialog on PS5. Measurements remain historical observations, not a guarantee that a model will fit. |
| 4 | First-party UI exchanges the configured bearer key for a 30-minute host-bound session and stores only that token in tab-scoped storage. API responses disable caching; external clients continue to use the bearer key. Native and web Settings show API key state, unencrypted HTTP, and network reachability; `GET /api/settings/security` exposes only that non-secret status. Console verification confirms valid host/origin acceptance and rejection of mismatched host/origin. | Verify actual expiry after 30 minutes and the first-party browser flow. Add listener binding controls only with a safe PS5 network configuration path. Plain HTTP remains trusted-network-only. |
| 5 | Bounded persistent journal records app start/clean stop, detects an unclean previous exit, records unique operation IDs and classifies model preparation, native/API generation, storage save, archive, and request rejection outcomes. PS5 journal verification showed app start, unclean prior exit, model preparation, generation completion, and the concurrent busy rejection. The API privacy notice confirms prompts, responses, keys, and recordings are excluded. | Verify retention after an actual crash and improve runtime-specific allocation classification where stable error codes are available. |
| 6 | Native generation settings hide irrelevant text controls for media models, separate response, advanced, memory, image, and audio groups, and show the current media defaults and supported choices. | Verify layout and wording at each supported text size on console. |
| 7 | Four-texture LRU cap; ccache for native builds; preparation scripts avoid rewriting unchanged upstream sources; unchanged UI shaders skip SPIR-V compilation; unchanged Vulkan CMake options skip a full configure. CI's host Vulkan job now restores and saves a separate compiler cache. On this host, an unchanged Vulkan runtime build step dropped from 13.8 s to 0.62 s. | Before/after GPU memory measurements and a clean full-build timing. |
| 8 | Required download sizes are shown in both UIs, and each download asks the user to confirm that they have enough space when the PS5 storage API cannot report free space safely. Verified/resumable download handling remains active. | Safe free-space preflight; current PS5 storage availability is explicitly reported as unknown. |
| 9 | Web conversation rename, JSON export/import, search, media gallery with source conversation/model links and file download, synced pin/archive state, and guarded deletion of archived media copies. `/api/media` rejects shared references and files outside the owning conversation's output folder. Console API smoke coverage verified conversation create/read/rename/pin/archive/delete and media-gallery listing. | Verify browser JSON import/export and actual generated-media export/delete flows on PS5. |
| 10 | Visible web focus rings with a stronger high-contrast style. | Translated core flows and shaping/right-to-left support. |

## Suggested order

1. Build the shared model manager and common state transitions.
2. Use that state to improve the native and web model pickers.
3. Improve the native generation settings using the model capability metadata.
4. Add bounded diagnostics for transitions and failures.
5. Measure console memory and build time, then optimize against those baselines.
6. Collect model memory measurements and add conservative preparation warnings.
7. Design and implement browser authorization alongside the API security review.
8. Add model download storage preflight and clear recovery actions.
9. Improve conversation and generated-media management.
10. Expand accessibility and localization after the core interaction patterns are
    reviewed across all pages and dialogs.

Each item should be implemented and verified as a separate change, with console
verification for runtime, memory, and crash-handling behavior.
