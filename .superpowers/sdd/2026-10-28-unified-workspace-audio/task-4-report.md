# Task 4 implementation report — Native Workspace

## Result

Implemented native Workspace mode/model behavior and the Audio workflows using the existing App
worker and shared `audio_workflows` operations. The native build script already linked
`src/audio_workflows.cpp`, so no build-script change was required.

Workspace mode selection offers Text, Audio, SD, and TTS and filters the model chooser by the
runtime-reported capability. Staging a model or generation preferences is runtime-neutral;
generation submission selects/prepares a staged model and applies the staged options in the worker.
Switching models for an existing conversation now also updates that conversation's model
attribution at submit time, so subsequent sends remain associated with the selected model.

Audio actions (music, Cover to ABC, stems) execute on the App's serialized worker, report progress
and stable result/error state, and reject a second operation while busy. The native library chooser
only lists existing WAV or ABC entries from `work/audio/`. The transient controller queue runs
variants sequentially with consecutive seeds, supports stopping after the active operation, and
leaves failed and later pending entries visible for retry or clearing.

Controller regressions cover all four capability categories, empty capability sets, staged model
and option edits without runtime selection/preparation or settings mutation, submit-time selection
and final options, submit-time model switching, source forwarding through the shared operation,
pre-runtime source rejection, worker/runtime failures, and busy-operation rejection.

## TDD and verification

- The controller regression was first run against the missing APIs and failed as expected (the
  initial red run occurred before the worker/UI implementation).
- A later submit-time model-switch regression initially failed because the session's model
  attribution stayed on the prior model; the worker now updates the attribution, and the focused
  regression passes.
- `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_native_controller.NativeController.test_workspace_modes_and_audio_actions -v` — PASS.
- `HOST_CXX=clang++-18 HOST_CC=clang-18 make test` — PASS: 110 tests, 2 skipped (optional real-model cache and host Vulkan parity tests).
- `make format-check` — PASS.
- `PROSPERO_APP_TITLE=PPSA99023 bash tools/build-vulkan-native.sh` — PASS; local native app folder built under `build/vulkan-runtime/PPSA99023`.
- `git diff --cached --check` — PASS before final report/staging review.

## Scope and limitations

- No console command, deployment, launch, or hardware test was run. Native UI verification on
  hardware remains deferred for user approval as requested.
- No push was performed.
- Existing unrelated KV-cache and web/SheetSage recovery changes remain outside the Task 4 commit;
  the KV-cache hunks in overlapping native files must remain unstaged. Other unrelated dirty files
  are likewise preserved.
- The current branch is `fix/pr43-must-fix`; the requested Task 4 commit will be made here, without
  pushing.

## Fix round — queue Stop interleaving and music-model eligibility

The queue's next-item dispatch now passes through a small tested decision helper. If the queue is
active with no current item while another App worker is busy, a Stop request is consumed once that
worker clears; the queue becomes inactive without changing pending item states or starting another
variant. The regression exercises the busy → Stop → idle sequence.

The native Create music and Queue variants actions now check the shared workflow's music-model
eligibility before prompting. The guard follows the local shared-operation contract: SheetSage and
HTDemucs IDs are reserved for Cover and stems, while the runtime registry routes ACE-Step and Yue2
to their dedicated generation adapters and other audio IDs through Stable Audio. Ineligible
selections receive an explanatory message without starting a job.

- RED: `PYTHONPATH=tests HOST_CXX=clang++-18 HOST_CC=clang-18 python3 -m unittest test_native_controller.NativeController.test_workspace_modes_and_audio_actions -v` failed while compiling the new queue regression because the dispatch helper did not yet exist.
- GREEN: the same focused controller command passed after introducing the decision helper and wiring it into queue dispatch.
- `HOST_CXX=clang++-18 HOST_CC=clang-18 make test` — PASS: 110 tests, 2 skipped (optional real-model cache and host Vulkan parity tests). The run emitted two `ResourceWarning`s for unclosed sockets in existing search MCP tests; both tests passed.
- `make format-check` — initially reported formatting violations in the new UI/test lines; after applying targeted clang-format, rerun passed.
- `PROSPERO_APP_TITLE=PPSA99023 bash tools/build-vulkan-native.sh` — PASS; local native app built under `build/vulkan-runtime/PPSA99023`.
- Eligibility checks in `tests/audio_workflows_test.cpp` cover ACE-Step, Stable Audio and Yue2 as music-eligible, and reject SheetSage, HTDemucs and an empty ID.
- No console commands, deployment, launch, or hardware tests were run; no push was performed.
