# Unified Workspace and cross-platform audio workflows

Status: design, approved conversationally on 2026-10-28. Implementation has not
started.

## Goal

Make Workspace the single place to choose Text, Audio, SD (image), or TTS
(voice) and use the selected model. Remove the separate Music Studio page while
preserving its audio-generation workflows in Workspace on both the web and
native PS5 interfaces.

The web and native interfaces should offer nearly the same workflow choices,
inputs, progress, errors, and results. Their interaction details remain
platform-appropriate: browser form controls on the web and controller/keyboard
controls in the native application.

## Approved product behavior

### Workspace modes and models

- Workspace exposes four modes: **Text**, **Audio**, **SD**, and **TTS**.
- Each mode lists only installed models with the matching capability:
  `text-to-text`, `text-to-audio`, `text-to-image`, and `text-to-speech`,
  respectively. The SD label maps to the existing image capability; TTS maps to
  the existing voice capability.
- Changing mode filters choices but does not silently select or load a
  different model. A user explicitly chooses a model before generation.
- The web and native interfaces use the capability metadata already exposed by
  the model runtime; they do not infer model type from its name.
- Existing conversations and the Models page remain available. Conversation
  history is not partitioned or rewritten by the selected mode.

### Audio workflows

Audio mode retains these four user-facing workflows:

1. **Create music:** enter lyrics and style, optionally edit/select ABC, set
   seed and duration, and generate a track.
2. **Cover to ABC:** choose an audio source, transcribe it with SheetSage2, and
   load the resulting ABC into the editor for further generation.
3. **Split into stems:** choose a source track, run HTDemucs, and inspect,
   play, or download its resulting stems.
4. **Queue variants:** queue multiple music generations with consecutive
   seeds, run them sequentially, and see each job's status and result.

Web Audio mode retains local audio upload into the shared audio library. The
native application does not upload files; it chooses an existing uploaded or
generated audio file from that same library for Cover and stems. Uploads,
generated tracks, ABC files, metadata, and stems continue to use the existing
`work/audio/` storage and are not deleted or migrated by this change.

Both interfaces use the same operation semantics and backend validation,
progress, error/result reporting, and artifact paths. Web API handlers and the
native UI are adapters over shared workflow operations rather than separate
implementations of Cover or stems. Long-running work remains serialized with
other GPU work; a queue runs one item at a time and can stop before the next
item without interrupting the current job.

### Navigation and compatibility

- Remove the separate Music Studio navigation link and page.
- Mount the Audio workflow inside Workspace.
- Requests to the existing `/music` page route redirect to Workspace's Audio
  mode; they must not serve a second Music Studio interface.
- Keep existing `/api/music/*` and generation endpoints available as the web
  transport where applicable. This change does not rename or remove public API
  routes.

## Architecture

The web UI and native UI remain separate renderers around one workflow
contract:

```text
Workspace modes (web) ─────┐
                           ├── shared audio workflow operations ── existing adapters/runtime
Workspace modes (native) ──┘                 │
                                             └── audio files, ABC, metadata, stems
```

The shared operations own request validation, source-file checks, GPU/runtime
admission and serialization, progress/status, and result artifacts. The HTTP
layer translates web requests into these operations; the native app invokes
the same operations directly. Neither UI duplicates model execution or
file-write behavior. The implementation should reuse existing model adapters
and storage/API behavior where possible, extracting only the logic needed to
make the contract callable from both clients.

The Audio workspace presents matching workflow names and equivalent controls
in both clients. The native layout may use controller-selectable panels and
the existing on-screen keyboard instead of web inputs, but it must not expose
different operation semantics. The native file chooser is limited to entries
in the shared audio library; adding a native upload or general filesystem
browser is explicitly out of scope.

## Error and lifecycle behavior

- Invalid, missing, or unsupported source files fail before model execution and
  leave existing library files untouched.
- Cover and stems memory-admission failures return a retryable message after
  releasing any resident model they own; they do not request an app restart.
- A failed queue item is recorded as failed and does not silently resubmit.
  Remaining pending items stay visible and can be retried or cleared.
- The shared runtime admits only one heavy operation at a time, respecting
  existing model-selection and model-unload coordination.
- Existing generated outputs and upload files remain intact when navigating
  between modes or leaving Workspace.

## Non-goals

- No native file upload, USB importer, or unrestricted filesystem browser.
- No replacement of the existing model catalog or public model APIs.
- No change to upstream dependency pins or generated media formats.
- No removal of audio model generation, Cover, ABC editing, stems, or queue.
- No redesign of unrelated Settings, Media, or Models pages.

## Acceptance criteria

1. Web and native Workspace both expose Text, Audio, SD, and TTS modes.
2. Each mode filters by model capability, including an empty state when no
   installed model has that capability; changing mode does not load a model.
3. The web Audio workspace can upload a source and complete Cover, stems,
   single-track generation, and a multi-item queue.
4. The native Audio workspace can choose the same uploaded/generated source
   files and complete Cover, stems, single-track generation, and a multi-item
   queue without an upload control.
5. Cover produces editable ABC; stems are playable/downloadable; generated
   tracks and workflow results appear in the shared audio library on both
   clients.
6. Long operations report progress and errors consistently, queue items run
   serially, and a memory refusal does not crash or restart the title.
7. The standalone Music Studio navigation/page is gone; `/music` opens the
   Workspace Audio mode.
8. Existing audio files, metadata, ABC files, and conversation records survive
   the transition unchanged.
9. Host tests cover the shared workflow contract, HTTP adapter, native action
   handling, mode filtering, queue boundaries, and error paths. Web and native
   UI states are rendered/inspected, and each workflow is exercised on test
   title `PPSA99023`; the main title `PPSA99004` is never modified.

## Verification plan

- Run the targeted shared-workflow and UI tests, then `make lint` and the full
  `HOST_CXX=clang++-18 HOST_CC=clang-18 make test` suite.
- Build the PS5 app, confirm the pinned llama.cpp, audio.cpp, and
  stable-diffusion.cpp inputs, and compare the ELF NEEDED list and program
  headers against the working build.
- Deploy only to `PPSA99023`. Exercise mode selection and each Audio workflow
  from web and native UI, using an uploaded audio-library input for native
  Cover/stems. Confirm output artifacts and that the title remains alive.
- Do not reboot the console. Do not alter or deploy to `PPSA99004`.

## Open implementation questions

- The exact shared-operation boundary must follow the existing adapter and
  storage APIs discovered during implementation; keep it narrow and avoid
  adding a generic workflow framework.
- Native queue persistence across an app restart is not part of acceptance;
  preserve the web queue's current reload behavior unless implementation
  evidence shows that doing so would break cross-client operation semantics.
