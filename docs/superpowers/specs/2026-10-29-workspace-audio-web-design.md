# Workspace Audio Studio (web) design

Status: approved design; implementation plan in progress.

## Goal

Move the web Music Studio into Workspace and make Audio a first-class
Workspace mode. Workspace has three top-level modes: **Text**, **Image**, and
**Audio**. Audio contains the existing music workflows and text-to-speech
(TTS), so users can create music or speech without leaving Workspace.

This phase changes the web interface only. The native PS5 interface is
explicitly deferred and remains unchanged.

## Proposed product behavior

### Workspace modes and Audio sections

- Workspace exposes exactly three top-level modes: **Text**, **Image**, and
  **Audio**. Image maps to the existing `text-to-image` model capability; the
  model purpose and backend APIs are not renamed.
- Selecting Audio opens its Studio view inside Workspace. It contains two
  sections, **Music** and **TTS**, with Music selected by default. TTS is not a
  fourth top-level mode.
- The Music section preserves existing web Studio workflows: create a track
  from lyrics and style, edit or choose ABC notation, set duration and seed,
  upload source audio, convert a source to ABC, split a source into stems,
  manage the audio library, and queue music variants.
- The TTS section reuses the existing text-to-speech generation flow and voice
  controls, including supported voice/language selection and voice recording
  where the selected model supports it. It filters models by
  `text-to-speech`; Music filters by `text-to-audio`. Neither section infers a
  capability from a model's name.
- Audio model and options remain staged in UI state until the user submits a
  generation or workflow request. Changing sections or models does not
  silently load or unload a model. Submission uses the existing model/runtime
  path and applies the final options for that request.
- Existing Workspace conversations, the Media page, the Models page, and
  conversation history remain available. The three-mode switch does not
  rewrite or partition conversation history.

### Navigation and URL behavior

- `/audio` is the canonical URL for the Workspace Audio mode. It serves the
  normal Workspace application shell and opens Audio, with Music selected by
  default.
- `/music` redirects to `/audio` so existing bookmarks continue to work.
- Remove the separate Music Studio navigation link and the standalone Music
  Studio page; there is one web application shell and one Workspace Audio
  experience.

### Data and compatibility

- Continue using the existing `/api/music/*`, `/api/generate`, `/api/jobs`,
  model-selection, media, and voice endpoints. Do not add or rename backend
  routes for this UI move.
- Generated audio, uploaded sources, ABC files, metadata, queue state that is
  currently persisted, and stems remain in their existing storage locations.
  Navigating between Workspace modes or opening `/audio` must not delete or
  migrate them.
- Reuse the current Music Studio behavior and error handling where possible;
  this is an embedding/integration task, not a rewrite of audio generation.

## Design and layout

Keep the existing Workspace visual language rather than the standalone Music
Studio palette. Continue using its theme tokens, responsive spacing, visible
focus treatment, and Inter/Montserrat typography. The current midnight palette
is anchored by top `#06080d`, lower background `#0c1018`, ink `#f4f1ea`, muted
`#9ca4b0`, and amber accent `#f0be7d`; existing daylight and user-selected
accent themes must continue to work.

Keep the three top-level mode controls in the Workspace toolbar. When Audio is
active, show a compact Music/TTS section selector beneath the toolbar, then
mount only the selected section. Music keeps its generation form and library
visibly distinct but within the same Workspace layout; TTS presents the
existing voice-generation controls in that same shell. On narrow screens,
stack the form and library and keep both selectors reachable without
horizontal page overflow.

```text
┌ Workspace toolbar ─────────────────────────────────────────┐
│  Text       Image       Audio                 Model…        │
└─────────────────────────────────────────────────────────────┘
                  ┌ Music ─── TTS ┐
                  │ selected view │
                  │ form / output │
                  │ library       │
                  └───────────────┘
```

## Components and data flow

- The Workspace mode controller owns the three top-level modes and filters
  models using declared `purpose` metadata.
- A small Audio section selector chooses Music or TTS. It updates the model
  purpose filter and the visible Audio panel without navigating to another
  standalone application.
- The Music panel reuses the existing web Music Studio controls and their
  existing API calls. Any extracted controller must receive the Workspace API
  helper rather than create a second authentication/session path.
- The TTS panel reuses the existing Workspace TTS request and voice-control
  logic. Existing recording permissions, model-specific controls, and error
  messages remain intact.
- The HTTP server maps `/audio` to the Workspace shell and redirects `/music`
  to `/audio`. Audio generation and file storage remain on the existing backend
  paths.

```text
/audio ──┐
/music ──┴─ redirect ──> Workspace shell
                              │
                    Text / Image / Audio
                              │
                       Music / TTS
                         ┌────┴────┐
                    existing web APIs
                         └────┬────┘
                   current runtime/storage
```

## Errors and lifecycle

- Missing or unsupported audio sources continue to show an actionable error
  and must not overwrite existing library files.
- Long-running generation and analysis continue to report progress, success,
  and failure using the existing job and status behavior. Controls that would
  submit conflicting work remain disabled while a request is active.
- Queue items continue to run serially. Stopping a queue prevents the next
  item from starting but does not falsely report the active backend job as
  cancelled. A failed item remains visible and is not silently resubmitted.
- Navigating between Music and TTS or away from Audio does not cancel an
  already accepted backend job or remove its artifacts.
- A missing model capability produces an explicit empty state; the UI does not
  select or load a different-purpose model as a fallback.

## Non-goals

- No native PS5 UI changes in this phase.
- No new audio-generation engine, public API, media format, or model catalog.
- No native upload, general filesystem browser, or storage migration.
- No unrelated redesign of Settings, Media, Models, or Text Workspace.

## Acceptance criteria

1. Workspace shows exactly Text, Image, and Audio as top-level modes.
2. Audio opens the in-Workspace Studio with Music and TTS sections; Music is
   selected by default and TTS is not a top-level mode.
3. Music and TTS model lists filter by `text-to-audio` and
   `text-to-speech`, respectively, including correct empty states.
4. The Music section preserves track generation, ABC editing, upload,
   Cover-to-ABC, stems, library management, and sequential queue behavior.
5. The TTS section preserves the existing speech generation, voice selection,
   language selection, and model-specific recording controls.
6. Model and option changes remain staged until submission; the active runtime
   is not changed merely by switching Workspace or Audio sections.
7. `/audio` opens Workspace Audio with Music selected. `/music` redirects to
   `/audio`. The standalone Music Studio link and page are gone.
8. Existing audio files, metadata, ABC files, stems, and conversation records
   remain intact.
9. The native UI and native model selection behavior are unchanged.
10. Desktop and mobile Audio Music, Audio TTS, and empty-model states are
    rendered and visually inspected; web tests, lint, and the relevant HTTP
    route tests pass.

## Verification plan

- Add/update focused browser-controller tests for the three Workspace modes,
  Music/TTS model filtering, staged selection, and section navigation.
- Update HTTP tests for `/audio` and the `/music` redirect while retaining
  existing API compatibility coverage.
- Run `node --test tests/*.mjs`, focused HTTP API tests, `make lint`, and the
  applicable full host test suite.
- Run the web preview and visually inspect desktop and mobile captures for
  Workspace default, Audio Music, Audio TTS, and an empty model capability
  state. Also exercise section changes and verify the existing audio workflow
  controls remain usable.
- Do not deploy to or interact with the PS5 console for this web-only phase.

## Implementation boundary

Prefer mounting the existing Music Studio workflow code into Workspace over
recreating the features. Keep changes limited to web assets, web route
handling, and corresponding web/HTTP tests. Update or replace the existing
cross-platform audio plan only after this spec is approved; do not use that
plan to change the native interface in this phase.
