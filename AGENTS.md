# Notes for agents working on ProsperoAI

ProsperoAI is a PS5 homebrew app (title `PPSA99004`) that runs llama.cpp on the console. The
branch `wip/llama-vulkan-migration` is work in progress and is not verified to boot.

## Read order

Read in this order and stop as soon as you have what you need:

1. This file. Stable: the build commands, console talking points and debugging order below
   change only when the workflow itself changes.
2. If the task touches the RADV/Vulkan backend migration: `docs/VK_PLAN.md` (static — gates,
   invariants, what must be proved) once per session, then `docs/VK_ACTIVE.md` (volatile —
   current step, blockers, last verified runs) last, right before starting work.
3. On demand only: `docs/VK_LOG.md` (dated run records — don't read it end to end to answer a
   status question; VK_ACTIVE.md's summary is enough), `docs/BUILDING.md`, and
   `.deps/mihawk-vulkan-review/AGENTS.md` plus its `docs/` (the reference project the RADV link
   recipe and this read-order convention are drawn from).

### Volatility contract

| File | Rule |
| --- | --- |
| This file | Stable. Edit when the workflow itself changes. |
| `docs/VK_PLAN.md` | Static. Edit only when a gate or an invariant changes; never record progress here. |
| `docs/VK_ACTIVE.md` | Volatile. Rewrite in place; keep it short. |
| `docs/VK_LOG.md` | Append-only. Add dated entries at the end; never rewrite an existing one. |

Your task is whatever the user's prompt asks. `VK_ACTIVE.md`'s "Next" list is background on
where things stand, not a queue to work through unprompted — say so if the prompt and that list
disagree about what to do next.

## Codebase graph

- For architecture and code-flow questions, use the Graphify skill and query
  `graphify-out/graph.json` first when it exists.
- Build the project graph with `graphify .` when no graph exists. After changing code or docs,
  update it with `graphify . --update` before using it for another query.
- If full semantic extraction has no configured LLM backend, use `--code-only` and keep working;
  do not ask for or expose API keys.
- Keep generated graph artifacts under `graphify-out/`.

## Build and verify

- Keep generated user-facing outputs in their persistent modality folders: `work/image/`,
  `work/audio/`, and `work/voice/` (sandbox paths `/download0/ProsperoAI/work/<category>/`).
  Do not route generated media to `/tmp` or the legacy `work/tmp/` scratch mapping.
- Keep build trees and generated build outputs under this repository's `build/` directory;
  don't use `/tmp` as a build directory. Use `/tmp` only for task-scoped scratch files when
  needed, and remove only the specific temporary files/directories created for that task.
  Never clear all of `/tmp` or broadly delete another shared scratch location.
- `make deps`, `make app`, and `make test` work on the host. `make app` builds the app folder
  and ZIP; read `docs/BUILDING.md` first.
- A build that links is not a build that runs. Check the NEEDED list (`readelf -d`) and the
  program headers against a working build before deploying.
- For backend feature verification on a running test title, call the app's HTTP API directly
  at `http://<PS5-IP>:11434/`; Bonsai, generation and media checks do not require Remote Play
  input or UI navigation. Use Remote Play only when the task specifically requires visual UI
  verification or the title must be launched through the console UI. Never treat a successful
  HTTP response from P5 Manager as proof that the app backend is running.
- Before polling port 11434, check `agent procs`; a stopped or crashed title has no HTTP listener,
  so `curl: (7) Failed to connect ... port 11434` is expected in that state and is not a backend
  feature result. Launch the authorized test title, confirm it appears in `agent procs`, then
  retry `/api/settings/runtime` until startup completes. If it disappears during a request,
  capture klog and check `agent procs` again before interpreting the HTTP error.
- A user request to implement, debug or benchmark on the console authorizes the necessary
  test deployments, payloads, launches, closing/replacing the relevant test titles and
  cleanup of files created for that task. Continue through repeated test iterations
  without asking for approval at each step; report the result and console state.
- Standing user authorization: upload and install task-related builds, models and
  payloads on the console's designated test titles without asking again. This includes
  FTP updates, stopping the test title, verifying it is unmounted, restoring its mount,
  launching it, running benchmarks and restoring the verified test build. Repeated
  iterations and a resumed conversation do not require another deployment confirmation.
  Use existing approved `tools/ps5ctl.py` execution rules where applicable. If an
  environment approval is still required, request only that execution approval and
  explain its source; do not add a separate conversational confirmation.
- Ask separately before rebooting, deleting unrelated data, or changing the main title
  `PPSA99004`. Sandbox/network execution approvals are still enforced by the environment.

## Talking to the console

Use `tools/ps5ctl.py`. It covers status, payloads, FTP, title removal, klog, shell (shsrv on
port 2323), process list, launch and screenshots.

- App debug trace: `python3 tools/ps5ctl.py trace [--prev] [--lines N] [--grep RE]` prints
  `/data/prosperoai/logs/debug-trace.txt`. It exists only while Settings > Debug log is on.
  The app redirects stdout/stderr of all runtimes (llama.cpp, ggml, audio.cpp) into it, so
  backend messages that never reach klog are here. The file is rotated to
  `debug-trace.prev.txt` at every app start and when the log is toggled in Settings;
  read `--prev` after a crash or relaunch. `[sys]` lines are copies of klog `[prosperoai]` lines.

- Console environment: copy `.env.ps5.example` to ignored `.env.ps5.local`, set `PS5_HOST`
  and `PROSPERO_TEST_TITLE`, then `source tools/ps5-env.sh` before builds or benchmarks.
  `ps5ctl.py` also loads the local file automatically; explicit environment variables win.
  Run `python3 tools/ps5ctl.py doctor` to verify configuration and payload hashes offline.
  `P5_MANAGER` is optional and has no default. Set it to the development host's URL only
  for Remote Play; `P5_SESSION` is needed only for screenshots. Neither is required for
  deployment, launching, klog or direct app HTTP tests. Never assume P5 Manager runs on PS5.
- All standard service ELFs are bundled in `tools/payloads/` with exact sizes, SHA-256,
  upstream sources and licenses in its `manifest.json`. Use
  `python3 tools/ps5ctl.py service restore` after a reboot, or `service ftp`, `service agent`,
  `service klog` and `service shell` individually. These validate the ELF before sending,
  skip services whose port is already available, and confirm the restored listener.
  Do not depend on another checkout, a console-side file or `/tmp` for a standard payload.

- Ports: payload 9021, FTP 2120, klog 3232, shsrv 2323, P5 Manager 3001 (remote play).
- After a console reboot, check each service instead of assuming the previous session survived:
  the payload loader (9021) may be listening while FTP (2120) is not. Restore missing services
  from the repository with `python3 tools/ps5ctl.py service restore`. This starts the FTP
  service before the control, klog and shell services. The loader itself must already be
  available. These resident payloads are separate from the title under test and do not
  survive a reboot.
- If `agent status` / `agent procs` returns connection refused on port 9111, run
  `python3 tools/ps5ctl.py service agent`, then retry the agent command.
  A refused control port alone means the resident control payload is absent; it does not
  prove that the console is frozen or unavailable. Check the payload loader separately.
- Never commit a real console IP. In docs and usage text, write `<PS5-IP>` (e.g.
  `http://<PS5-IP>:11434/`). In code, the address is a variable, not a default: `PS5_HOST` is a
  required environment variable with no fallback value (`tools/ps5ctl.py` exits with a clear
  message if it's unset); don't hardcode any address, real or placeholder, as a code default.
  Set the real address only in your own environment or ignored `.env.ps5.local`, never in a tracked file.
- Screenshot before every confirming press (Cross) in the UI. Keep a screenshot in
  `~/radv_title/` or the scratchpad, and read it before pressing.
- Scope `kill`, `rm`, `launch`, `title rm` and `--yes` to the test titles and temporary
  files relevant to the authorized task. Verify the target before acting. Unrelated
  titles or user data require separate approval.
- shsrv's `kill` does not accept signal options. A process in state `STOP` did not die from
  `SIGCONT` plus `SIGKILL` sent by a payload, so closing it through the UI or a reboot may be
  needed.
- A reboot of the console is not a routine step. Ask first.

## Titles and registration

- Test titles use IDs other than `PPSA99004`, for example `PPSA99014`. Never delete or overwrite
  `PPSA99004` for a test.
- Development and release builds use an app folder (releases also publish a ZIP of that folder).
  Do not replace a registered package image in place: remove its registration in the console UI
  before installing a different image. Folder-backed test titles can be updated after stopping
  the title and verifying it is unmounted. Folder deployment works on this console when the
  uploaded app tree has mode `755`; in particular, `eboot.bin` and PRX modules must be executable.
  Prefer `tools/ps5ctl.py ftp put-dir`, which applies the executable permissions, over repeatedly
  building and registering `.ffpfsc` images during development.
- Deleting files from `/data/homebrew` does not remove the registration.
- To update a folder-backed test title without a reboot, stop it in the console UI, verify
  `shadow info <ID>` reports `mounted:false`, then upload only the changed files with FTP and
  retain mode `755` on the app tree. The first registration still requires the configured title
  registration workflow.
- Icons and metadata come from `sce_sys/`. Use this project's `sce_sys`, not another project's.

## Git and attribution

- Commit only on the WIP branch unless the user asks otherwise. Push only when asked.
- Do not add `Co-Authored-By` or "Generated with Claude Code" lines to commits or PR text.


## Debugging a title that does not start (lessons from PPSA99014-19)

The live status of the RADV investigation (what's ruled out, what's next) is in
`docs/VK_ACTIVE.md`, not here — this section is the stable, reusable procedure.

Read-only checks first, in this order:

1. `agent status` / `agent procs` (control payload, port 9111): is the title running, and is the payload resident? `agent launch <ID>` returns the call-by-call result.
2. `shadow info <ID>` (ShadowMountPlus API, port 10101): is the title registered with shadowmount? 404 means it is not.
3. `cat /data/ps5vkctl.log --tail 2000` (payload log, one line per call) and `cat /data/shadowmount/debug.log --tail 2000` (install log).
4. `cat /download0/<marker>.txt` for markers the title writes itself. Use `/download0`, not `/data`: a title did not write to `/data`.
5. klog (`klog --seconds 60`, port 3232) lines with `[PS5VK]` prefix, as Mihawk's title writes them. An empty or banner-only capture means klog is not streaming.
6. `trace --prev` / `trace` (app debug trace, see above) for runtime stderr, ggml/audio.cpp errors and the app's own steps. A crash backtrace is in klog (`signal: 11`); map its addresses to symbols with `nm -n build/vulkan-runtime/llvm-pie.elf` after subtracting the `0x400000` load base.

Launch result codes seen on this console (FW 12.70):
- `0x0000a018`, `0x00008018`, `0x0000e018`, `0x0000c018`: launched; the title may still crash right after.
- `0x80940031` (Resource temporarily unavailable): title is not ready or not registered (or another app is running). Check `shadow info`.
- `0x80a40010`: not enough free space. Free space in Settings > Storage, or delete an unfinished `.part` download.
- Title gone right after launch with `procs` empty and the sentinel toast `crashed before KStuff was paused`: the crash is before `main` (loader, relocations, TLS, init).

Working rules that came out of this:
- One title ID per test slot; do not create a new ID per attempt (each one leaves a registration record).
- Replace a package only after deleting the old registration in the UI (Options > Delete) or, for shadowmount-only titles, through `shadow delete <ID> --yes`.
- Check the minimal case before the feature case: a title that only writes a marker from a constructor separates a link or packaging problem from a Vulkan problem.
- Compare the link with the one that works (Mihawk: `tools/radv-link.sh`, linker script `tooling/psbc/ps5-pie-unwind.ld`, `-ffunction-sections`, CRT from `tooling/native`); our own link used a different script and no platform wrappers.
