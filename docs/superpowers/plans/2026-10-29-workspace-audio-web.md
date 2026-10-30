# Workspace Audio Studio (web) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put the existing web Music Studio and TTS workflow inside a three-mode Workspace, with `/audio` as the canonical URL.

**Architecture:** Keep the current Music Studio workflow and backend endpoints, mounting its controls inside Workspace and routing its requests through the Workspace's authenticated API helper. Workspace owns Text/Image/Audio navigation; Audio owns Music/TTS sections, with purpose-based model filtering. `/audio` serves the Workspace shell in Audio mode and `/music` redirects to it. The native PS5 UI is not changed.

**Tech Stack:** C++20 HTTP server, browser JavaScript/HTML/CSS, Node.js `node:test`, Python `unittest`.

**Spec:** `docs/superpowers/specs/2026-10-29-workspace-audio-web-design.md`

## Global Constraints

- Workspace has exactly three top-level modes: Text, Image, and Audio.
- Image uses `text-to-image`; Music uses `text-to-audio`; TTS uses `text-to-speech` model purpose metadata.
- Music and TTS model/option changes are staged and do not load or unload a model until submission.
- Preserve the existing Music Studio workflows, backend API routes, audio artifacts, ABC files, metadata, stems, and conversation history.
- `/audio` opens Workspace Audio with Music selected; `/music` redirects to `/audio`.
- Reuse the Workspace authenticated API/session path; do not retain a second Music Studio API-key path.
- Change web assets and web route/tests only. Leave native PS5 UI and console state untouched.
- Preserve all unrelated dirty Qwen, audio, and application changes in the working tree; edit only the relevant hunks.
- The current branch is `fix/pr43-must-fix`; do not commit on this branch unless the user asks or the repository's branch constraint changes.

## Review Focus

- No installed `text-to-audio` or `text-to-speech` model: show an empty state and never select a different capability as fallback; test both filters and the UI state.
- Unsupported or missing upload/source: show the existing error and do not overwrite a library file; retain the current HTTP upload/path validation tests.
- Expired browser session while using an embedded Music action: show the Workspace connection flow, not a second Music key prompt; test the shared API adapter and remove the `prospero-key` dependency from the Music controller.
- A request is running or a queue is stopped: preserve the active job and do not start the next item after Stop; retain and update the existing queue regression tests.
- Direct route entry: `/audio` must load the Workspace Audio mode and `/music` must redirect to it; cover both with HTTP tests.

---

### Task 1: Add three Workspace modes, Audio sections, and canonical routes

**Files:**
- Modify: `assets/web/workspace.js`
- Modify: `assets/web/index.html`
- Modify: `assets/web/app.js`
- Modify: `assets/web/app.css`
- Modify: `vulkan/http_server.cpp`
- Modify: `tests/web_workspace_audio_test.mjs`
- Modify: `tests/test_http_api.py`

**Interfaces:**
- `ProsperoWorkspace.filterModels(models, mode, audioSection = 'music')` returns models whose declared `purpose` matches the selected view: `text-to-text`, `text-to-image`, `text-to-audio`, or `text-to-speech`.
- `ProsperoWorkspace.createWorkspaceMode(...)` accepts `text`, `image`, and `audio`; its `setAudioSection('music' | 'tts')` changes the Audio capability filter without selecting or preparing a model.
- `GET /audio` serves `index.html`; `GET /music` returns a `302 Found` response with `Location: /audio`.

- [ ] **Step 1: Add failing tests for mode/subsection filtering and routes**

In `tests/web_workspace_audio_test.mjs`, update the mode test to assert one matching model for each capability and no matches when a capability is absent. Add a test that changing from Audio/Music to Audio/TTS leaves `selectModel` and `prepareModel` call counts at zero. In `tests/test_http_api.py`, replace the standalone Music page expectation with assertions that `/audio` serves the Workspace shell and `/music` returns a redirect to `/audio`.

```js
assert.deepEqual(Array.from(filterModels(models, 'audio', 'music'), model => model.id), ['music-1']);
assert.deepEqual(Array.from(filterModels(models, 'audio', 'tts'), model => model.id), ['tts-1']);
assert.deepEqual(Array.from(filterModels(models, 'image'), model => model.id), []);
workspace.setMode('audio');
workspace.setAudioSection('tts');
assert.deepEqual(calls, []); // section changes only stage UI state
```

Run: `node --test tests/web_workspace_audio_test.mjs`

Expected: FAIL because the current controller has separate `sd`/`tts` modes, no Audio subsection, and the HTTP test still expects `/music` to serve a second page.

- [ ] **Step 2: Add the mode and section capability mapping**

In `assets/web/workspace.js`, make the top-level mode map cover Text and Image, add the Audio subsection map, and validate section names in `setAudioSection`. Keep `submit()` as the only place that calls `selectModel` and `prepareModel`.

```js
const MODE_PURPOSE = {text: 'text-to-text', image: 'text-to-image'};
const AUDIO_PURPOSE = {music: 'text-to-audio', tts: 'text-to-speech'};

function purposeFor(mode, section) {
    return mode === 'audio' ? AUDIO_PURPOSE[section] : MODE_PURPOSE[mode];
}
```

Use `purposeFor` in both `filterModels` and `createWorkspaceMode.submit`; keep an unknown mode or section from silently selecting a fallback model.

- [ ] **Step 3: Mount the three-mode Workspace navigation and route state**

In `assets/web/index.html`, change the top-level tabs to Text/Image/Audio, add accessible Music/TTS tabs inside the Workspace Audio panel, and remove the separate Music Studio navigation link and standalone Speak shortcut. In `assets/web/app.js`, initialize Audio when `location.pathname === '/audio'`, make tab clicks switch panels and stage the URL with `history.pushState`, and handle `popstate` so browser Back/Forward restores the selected view. Keep the TTS chat/model path available as the Audio/TTS section; do not call runtime APIs from mode or subsection handlers. In `assets/web/app.css`, set the responsive top-level mode grid to three columns and reuse Workspace theme tokens.

```js
let workspaceModeName = location.pathname === '/audio' ? 'audio' : 'text';
let audioSectionName = 'music';

function setAudioSection(section) {
    audioSectionName = section;
    workspaceMode.setAudioSection(section);
    renderWorkspaceModels();
    renderWorkspaceAudio();
}
```

Keep `kindNames` and the Models page's Audio/Voice filters where they describe model purposes; only Workspace navigation becomes three modes.

- [ ] **Step 4: Route `/audio` to the shell and redirect `/music`**

In `vulkan/http_server.cpp`, map `/audio` to the same `index.html` asset as `/`. Add a small redirect response helper that emits `302 Found`, `Location: /audio`, `Cache-Control: no-store`, and `Connection: close` for `/music`. Remove the standalone `/music` and `/music.css` page mappings, but keep the `/music.js` script mapping until the embedded controller is loaded by the Workspace shell. Do not change any `/api/music/*` routes.

```cpp
if (std::strcmp(path, "/music") == 0) {
    send_redirect(api, sock, "/audio");
    return true;
}
```

- [ ] **Step 5: Run the focused mode and route tests**

Run: `node --test tests/web_workspace_audio_test.mjs` and `HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api.HttpApiTests.test_audio_routes_use_workspace -v`.

Expected: PASS; `/audio` serves Workspace markup, `/music` redirects, Audio section changes filter by purpose, and neither mode nor section changes select/prepare a model.

### Task 2: Mount Music Studio workflows and share Workspace authentication

**Files:**
- Modify: `assets/web/index.html`
- Modify: `assets/web/app.js`
- Modify: `assets/web/music.js`
- Modify: `assets/web/app.css`
- Delete: `assets/web/music.html`
- Delete: `assets/web/music.css`
- Modify: `vulkan/http_server.cpp`
- Modify: `tests/web_workspace_audio_test.mjs`
- Modify: `tests/test_http_api.py`

**Interfaces:**
- `window.ProsperoWorkspaceApi(path, options)` is the authenticated Workspace `api()` function and is loaded before `music.js`.
- The existing Music Studio DOM IDs and workflow handlers are mounted once inside `#workspace-audio-music`; there is no nested `<main>` or second page shell.
- The Music model selector is populated from installed models whose `purpose === 'text-to-audio'`; the separate Enhance selector remains text-model-only and changing it does not call `/api/models/selection`.

- [ ] **Step 1: Add failing integration tests for the embedded Music view**

Update `tests/web_workspace_audio_test.mjs` to assert that Workspace contains the existing Music controls (lyrics, style, ABC editor, upload, Cover, stems, queue, and library), loads `music.js` from the Workspace shell, has no standalone Music navigation/page, and uses purpose metadata rather than a model-name regular expression for the Music model selector. Add an assertion that the Music request wrapper calls the Workspace API adapter rather than reading `localStorage['prospero-key']`.

```js
assert.match(htmlSource, /id="workspace-audio-music"/);
assert.match(htmlSource, /id="lyrics"/);
assert.match(htmlSource, /id="cover-go"/);
assert.match(htmlSource, /id="q-run"/);
assert.doesNotMatch(musicSource, /MUSIC_RE/);
assert.doesNotMatch(musicSource, /localStorage\.getItem\(KEY\)/);
```

Run: `node --test tests/web_workspace_audio_test.mjs`

Expected: FAIL because the Music markup is still in `music.html`, and `music.js` has its own key and name-based model filter.

- [ ] **Step 2: Embed the existing Music markup in Workspace**

Move the useful Studio controls from `assets/web/music.html` into the `#workspace-audio-music` section in `assets/web/index.html`; do not copy its header, separate model/chat header, or nested `<main>`. Keep existing functional element IDs so `music.js` can continue to own playback, ABC, upload, Cover, stems, queue, and library behavior. Load `music.js` after `app.js` and keep `/lame.min.js`, `/mp3-worker.js`, and `/stems-worker.js` available.

- [ ] **Step 3: Adapt Music requests to the shared session and capability list**

Expose `api` from `assets/web/app.js` as `window.ProsperoWorkspaceApi` before loading `music.js`. Change the Music controller's JSON and binary request wrappers to call that adapter, retaining its existing response parsing and timeout behavior. Remove the local key bar and `prospero-key` storage path so a 401 opens the Workspace connection flow. Populate the Music generation model selector from `/v1/models` filtered by `purpose === 'text-to-audio'`; keep the existing text-only Enhance choices, but stage their selection locally instead of posting model selection immediately.

```js
window.ProsperoWorkspaceApi = api;

function api(path, body, timeoutMs, method) {
    const options = body === undefined ? {} : {
        method: method || 'POST',
        body: JSON.stringify(body),
    };
    return window.ProsperoWorkspaceApi(path, options);
}
```

For binary uploads, pass the original `ArrayBuffer` and `Content-Type: application/octet-stream` through the same adapter; do not stringify it.

- [ ] **Step 4: Restyle the mounted Studio and remove the standalone page assets**

Use the existing `.workspace-audio` / `.audio-*` styles in `assets/web/app.css` where they fit; add only scoped rules needed for the preserved controls. Use the Workspace theme tokens so Daylight, accent, contrast, and reduced-motion preferences continue to apply. Remove standalone page CSS/HTML routes and delete `assets/web/music.html` and `assets/web/music.css`; keep `assets/web/music.js` as the controller asset and retain the public `/api/music/*` endpoints.

- [ ] **Step 5: Run focused Music and compatibility tests**

Run: `node --test tests/web_workspace_audio_test.mjs` and `HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api -v`.

Expected: PASS; embedded Music keeps playback, ABC, upload, Cover, stems, metadata, queue, and library behaviors, while existing HTTP upload/ABC/library tests still pass and no second authentication path remains.

### Task 3: Put the existing TTS conversation flow inside Audio

**Files:**
- Modify: `assets/web/index.html`
- Modify: `assets/web/app.js`
- Modify: `assets/web/workspace.js`
- Modify: `assets/web/app.css`
- Modify: `tests/web_workspace_audio_test.mjs`

**Interfaces:**
- Selecting Audio/TTS sets Workspace mode to `audio`, Audio section to `tts`, and filters the Workspace model picker to `text-to-speech`.
- TTS uses the existing conversation composer, voice picker, language controls, recording dialog, model preparation, and conversation persistence. It does not create a second TTS API flow.
- Selecting Audio/Music returns to the Music panel without deleting the current conversation or cancelling an accepted audio job.

- [ ] **Step 1: Add failing tests for TTS as an Audio subsection**

Add tests that the top-level Workspace has no `data-mode="tts"` control, that the inner TTS control exists, and that selecting it filters only `text-to-speech` models while retaining the existing voice controls. Extend the mode-staging test to prove selecting TTS and choosing a model do not call selection/preparation until submit.

```js
assert.match(htmlSource, /data-audio-section="tts"/);
assert.doesNotMatch(htmlSource, /data-mode="tts"/);
assert.deepEqual(Array.from(filterModels(models, 'audio', 'tts'), model => model.id), ['tts-1']);
assert.deepEqual(calls, []); // selecting TTS still does not load a model
```

Run: `node --test tests/web_workspace_audio_test.mjs`.

Expected: FAIL until TTS is moved under Audio and section state drives model filtering.

- [ ] **Step 2: Reuse Workspace chat as the Audio/TTS panel**

In `assets/web/index.html`, place the existing chat/conversation surface in the TTS section state rather than creating a second composer. In `assets/web/app.js`, make the Audio/TTS selector stage a text-to-speech model and show the existing chat, `workspace-voice-group`, and model-specific Chatterbox controls. Keep the active Audio section independent from top-level mode; selecting Music hides the TTS conversation surface but leaves its saved conversation intact. Keep TTS API calls, recording permissions, cancellation, and conversation persistence unchanged.

- [ ] **Step 3: Remove TTS as a top-level Workspace mode**

Remove the Speak welcome shortcut and any top-level TTS tab state. Keep Voice in the Models page and conversation type filters because they still describe existing model/history capabilities. Ensure the TTS section's empty state asks the user to install a compatible voice model and never substitutes a text or music model.

- [ ] **Step 4: Run the focused Workspace and TTS tests**

Run: `node --test tests/web_workspace_audio_test.mjs`.

Expected: PASS; Text/Image/Audio are the only Workspace modes, Music/TTS are internal Audio sections, and TTS requests still use existing staged model preparation and voice controls.

### Task 4: Full web verification and rendered-state review

**Files:**
- Modify only implementation or test files from Tasks 1–3 when a verification failure identifies a concrete defect.
- Store captures under `.amp/in/artifacts/` only after verifying `/.amp/in/` is in the repository-local exclude file from `git rev-parse --git-path info/exclude`.

**Interfaces:**
- Validates the integrated Workspace without deploying or interacting with the PS5 console.
- Leaves native code and all unrelated dirty changes untouched.

- [ ] **Step 1: Run the complete relevant automated checks**

Run:

```bash
node --test tests/*.mjs
HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_http_api -v
make lint
HOST_CXX=clang++-18 HOST_CC=clang-18 make test
```

Expected: all commands pass. If a repository-wide test fails outside changed files, report the exact failing test and distinguish it from failures in the modified web or HTTP paths.

- [ ] **Step 2: Render and inspect representative Workspace states**

Create a task-scoped fixture preview under `.amp/in/` that serves `assets/web/`, maps `/audio` to `index.html`, and returns deterministic responses for `/v1/models`, `/api/models/presets`, `/api/models/selection`, `/api/models/memory`, `/api/settings/runtime`, `/api/settings/security`, and `/api/conversations`. Include one model for each declared purpose and a fixture switch that returns no audio/TTS models. Do not edit the existing Qwen preview scripts. Start Chromium with remote debugging, load the fixture preview, and inspect desktop and mobile renders for Workspace default, Audio/Music, Audio/TTS, and the two empty-model states. Exercise changing tabs, direct `/audio`, redirect `/music`, Music queue stop behavior, and TTS voice controls. Capture the states in `.amp/in/artifacts/` after checking the repository-local exclude file, then inspect every capture with `view_media`; source assertions alone are not visual verification.

- [ ] **Step 3: Review the combined diff and verify scope**

Run `git diff --check` and review the changed hunks in `assets/web/`, `vulkan/http_server.cpp`, and the two web/HTTP test files. Confirm `git status --short` still contains unrelated Qwen/native work without accidental edits, the `/api/music/*` routes remain available, `/music` redirects, and the implementation contains no standalone Music page or duplicate key/session path. Do not commit on the current branch unless the user asks.

### Task 5: Keep Kokoro voice selection with the selected TTS model

**Approved follow-up:** The user clarified that Kokoro voice selection belongs with model selection in both web and native Workspace flows, not in global Settings.

**Files:**
- Modify: `assets/web/app.js`
- Modify: `tests/web_runtime_kv_test.mjs` or another focused web Workspace test
- Inspect: `src/native_ui.cpp` and native Workspace tests; change them only if the native voice control is not already attached to the selected TTS model.

**Requirements:**
- Remove the standalone Kokoro voice control from web Settings.
- Keep voice selection beside the active TTS model in the Workspace conversation flow; selecting a model must not save or load runtime settings by itself.
- Preserve the native Workspace behavior that shows the Kokoro voice control for the selected voice model. The inspected implementation already adds a Kokoro voice row only when the selected model has voice capability; do not duplicate or relocate it into native Settings.
- Add a regression test that proves the web Settings UI does not create or expose a Kokoro setting and that the Workspace TTS voice picker remains available and stages the selected voice for submission.

**Verification:** Run the focused regression test first (confirm it fails before the implementation), then `node --test tests/*.mjs`, `make lint`, and `HOST_CXX=clang++-18 HOST_CC=clang-18 make test`. Do not deploy to the console for this UI placement change.

---

## Plan self-review

- **Spec coverage:** Tasks 1 and 3 cover the three modes, Audio sections, purpose filters, staged model selection, TTS reuse, and URL behavior. Task 2 covers the Music feature set, Workspace authentication, storage/API compatibility, and responsive styling. Task 4 covers tests and desktop/mobile rendered inspection.
- **Placeholder scan:** No TODO/TBD or unspecified validation step is used; each implementation task names its files, behavior, and focused command.
- **Type/interface consistency:** `filterModels(models, mode, audioSection)` and `createWorkspaceMode.setAudioSection(section)` are defined in Task 1 and consumed in Tasks 2–3. `window.ProsperoWorkspaceApi(path, options)` is defined in Task 2 before `music.js` uses it.
- **Review Focus coverage:** Empty capability sets are tested in Task 1 and rendered in Task 4; unsupported uploads remain covered by HTTP regression tests in Task 2; session expiry is tested in Task 2; queue stop behavior is exercised in Task 4; `/audio` and `/music` are tested in Task 1 and exercised again in Task 4.
- **Scope:** This is one cohesive web integration. It excludes native changes, console deployment, public API redesign, and dependency changes.
