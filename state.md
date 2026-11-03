# PS5SD Image Studio — handoff state

Last updated: 2026-11-03

## Goal and approved scope

Continue the approved plan in
`docs/superpowers/plans/2026-11-02-ps5sd-verified-image-models.md` and its approved design spec
in `docs/superpowers/specs/2026-11-01-ps5sd-verified-image-models-design.md`: remove Qwen Image as a
product model, add the curated PS5SD image presets, support model-capability-driven text-to-image,
image-to-image, inpainting and ControlNet workflows, and provide the Image Studio on web only.
Native UI remains unchanged. The owner also explicitly requested new presets for the added models;
those were included in Task 1.

## Completed implementation

- Tasks 1–4 are implemented and committed on branch `fix/pr43-must-fix`, currently at
  `9bd6f81` (`fix(web): address image workflow review findings`). The commits add curated preset
  metadata and defaults, remove Qwen Image product support while preserving safe deletion of old
  installs, add generic typed image workflows/API handling, and build the web Image Studio.
- Review reports and task evidence are in
  `.superpowers/sdd/2026-11-02-ps5sd-verified-image-models/`. Reviews for Tasks 1–4 were reported
  clean after their documented fixes. Existing host and browser checks for those tasks are
  recorded in the task reports; do not treat them as Task 5 or console evidence.
- The web picker is driven by server metadata and covers the current 14 image presets. New PS5SD
  models are still marked experimental until ProsperoAI behavior is demonstrated on the console.
- No push, deployment, GitHub issue closure, or console generation run is recorded as completed.

## Current checkout and work to preserve

- Checkout: `/home/dietpi/Projects/console/ProsperoAI-pr43`
- Branch: `fix/pr43-must-fix`; HEAD: `9bd6f81`; it tracks `origin/pr43` and was 24 commits ahead
  at the last local status check. Do not push blindly: an earlier remote check reported PR #43
  merged, so refresh the PR/branch status and ask the owner whether a new PR is needed before
  attempting delivery.
- Preserve the existing uncommitted changes: `tests/http_api_harness.cpp`,
  `tests/vulkan_router_test.cpp`, and the approved plan/spec files under `docs/superpowers/`.
  They were present before this handoff request; do not discard or silently fold them into an
  unrelated change.
- Do not assume the final suite or current diff is verified based on the earlier task reports.

## Pending work

Task 5 in the approved plan has not been completed:

1. When resumed, run the plan's format/lint/full host checks and image/web regressions; build the
   PS5 app and inspect ELF NEEDED entries/program headers before any deployment.
2. Only use test title `PPSA99023`. The last known console snapshot reported unrelated title
   `PPSA03671` running (PID 483, about 9.6 GiB); this may be stale, so recheck `agent procs` and
   `shadow info PPSA99023` before any launch or deployment. Never stop an unrelated title, reboot,
   or modify `PPSA99004` without separate approval.
3. Test the presets/workflows on-console only after the host build and ELF checks pass. Record
   model, workflow/settings, generated output, switching behavior, and memory/errors; mark a model
   ProsperoAI-verified only with successful console evidence. Otherwise keep it experimental or
   blocked.
4. Recheck current PR #43 comments and issues #42/#47 before acting. Earlier review found #42
   relevant to image workflows and #47 unrelated (Bonsai KV-cache); close neither without fresh
   status and verified acceptance evidence. No issue was closed in this work.
5. Update `docs/SUPPORTED_MODELS.md` and the plan/progress evidence only from actual verification
   results. Follow repository rules: commit only when requested, push only when requested, and
   never treat a linked build as proof it runs on PS5.

## Owner instructions and verification boundary

The owner first asked to defer testing and write this handoff for the next agent, then asked to
commit these documentation changes on the PR #43 branch. No tests, builds, or console actions were
run for these documentation requests. Task 5 remains pending; the deferral does not claim tests
passed or cancel the previously approved verification scope.
