# Building ProsperoAI

ProsperoAI builds on Linux (tested on Debian 12; Ubuntu and WSL should work). One command
fetches the pinned inputs and builds the app folder:

```bash
make deps        # fetch missing dependencies at their pinned revisions
make app         # build dist/PPSA99004 and dist/PPSA99004.zip
```

> **Work in progress.** The Vulkan folder build and HTTP generation are verified on PS5 FW 12.70. See [benchmark results](VK_BENCHMARK_2026-10-08.md) and [current status](VK_ACTIVE.md).

## Make targets

| Target | What it does |
|---|---|
| `make` / `make app` | Build `dist/PPSA99004` and a ZIP of that app folder |
| `make deps` | Fetch missing dependencies (SDK, zlib, pinned git inputs from `tools/deps.json`) |
| `make deps-status` | List each pinned input, where it lives and whether it matches its pin |
| `make doctor` | Check the host tools |
| `make libc` | Rebuild and verify `runtime/libc.prx` (the clean-room runtime shim) |
| `make deploy PS5_HOST=<address>` | Build and FTP-deploy to `/data/homebrew/PPSA99004` |
| `make undeploy PS5_HOST=<address>` | Remove this title from `/data/homebrew` |
| `make test` | Host integration tests of the tooling |
| `HOST_CXX=clang++-18 HOST_CC=clang-18 make test` | The same, when the default `clang++` has no sanitizer runtime |
| `make lint` | Format, static-analysis and metadata checks |
| `make clean` / `make distclean` | Remove `build/` and `dist/` (`distclean` also removes `.deps/`) |

Several test harnesses build with `-fsanitize=address,undefined`. They use `HOST_CXX` and
`HOST_CC` when set, and `clang++` otherwise; if that default clang has no
`libclang_rt.asan-x86_64.a` the link fails with "cannot find", which is a missing runtime
package and not a test failure. Point both variables at a toolchain that has it. The
browser-interface tests under `tests/*.mjs` run through `tests/test_web_ui.py` and need
`node`.

Useful variables: `USE_CCACHE=0` (ccache is optional), `FTP_PORT` (default `2121`), and
`DEPLOY_DRY_RUN=1` (print the folder-deploy plan without sending it).

## Dependencies

Every input is pinned in `tools/deps.json` and fetched by `make deps` (`tools/deps.py`) only when
it is missing. Archives are checked against their SHA-256 before use; git repositories are fetched
at their pinned commit. Nothing that already exists is modified.

Inside this repository, in `.deps/`:

- **PS5 Payload SDK v0.42** (archive, SHA-256 pinned), with `prospero-clang` and the import stubs.
- **llama.cpp** at the pinned commit (the Vulkan backend migration is developed against it).
- **Khronos Vulkan-Headers** at the pinned commit (newer than most distributions ship; ggml-vulkan
  needs `VK_EXT_layer_settings`).

Inside this repository (`.deps/`), as git checkouts:

- **Mihawk's PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK** (`.deps/mihawk-*-review`): the RADV driver and
  its build recipe. The same pins are used by ProsperoEden.

The clean-room runtime shim (`runtime/libc.prx`) is generated from `tooling/native/` by `make libc`.

## Host tools

`make doctor` lists what is missing. For the current build you need `clang-18`, `lld-18`, `cmake`,
`ninja`, `make`, `python3` (3.11+), `curl`, `unzip`, `sha256sum`. For the Vulkan backend work you
also need `glslc` (shaderc), `spirv-headers`, and `glslangValidator`. The speech runtime
(kokoro.cpp) links espeak-ng directly, so the host build needs `libespeak-ng-dev`; the PS5 build
uses the pinned headers and `vendor/lib/libespeak-ng.a` instead. `ccache` is optional.

Ubuntu/Debian example:

```bash
sudo apt-get install -y clang-18 lld-18 cmake ninja-build ccache glslc spirv-headers glslang-tools python3-mako libespeak-ng-dev
```

## vulkan-runtime

Set `PROSPERO_TEST_TITLE=<TEST-TITLE-ID>` before using the test-app targets. For example,
`PROSPERO_TEST_TITLE=<TEST-TITLE-ID> make vulkan-runtime` builds the test folder
`build/vulkan-runtime/<TEST-TITLE-ID>` without uploading or launching it. `make app`
packages the same runtime for the configured
release title. The app links upstream llama.cpp `cb44fdb` and one shared GGML
`cd6d2b7`, `GGML_MAX_NAME=160`, source-built SD, audio and Kokoro adapters on Vulkan.
AGC inference and private SD/TTS ggml archives are removed;
RADV still requires the console's AGC platform driver. Only one modality runtime stays
resident at a time; after two minutes without inference activity by default, the active
runtime is released and prepared again when the next request arrives. The timeout is
configurable from one to 60 minutes in console or web Settings. See
[runtime details](VULKAN_RUNTIME.md).

## Vulkan backend development helpers

These steps need no console:

```bash
make deps             # pinned llama.cpp and Vulkan-Headers next to the SDK
make llama-vulkan     # host llama-cli with the Vulkan backend, and a PS5 syntax check of ggml-vulkan
make radv             # RADV for the PS5 (see below for the host tools)
```

`tools/build-llama-vulkan.sh` writes its build to `build/llama-vulkan/`.

Building RADV for the PS5 (`make radv`) uses Mihawk's pinned recipe in `.deps/mihawk-vulkan-review`
and needs these host tools, all built into `~/.local` (no system packages are changed beyond the
apt packages listed):

```bash
sudo apt-get install -y llvm-19-dev libclc-19-dev libclang-19-dev libclang-cpp19-dev clang-19 \
    libllvmspirvlib-19-dev ninja-build ccache
python3 -m venv ~/.venvs/meson && ~/.venvs/meson/bin/pip install 'meson>=1.4' ninja
```

Three components are built from source because Debian 12 does not ship them at the versions RADV
requires (LLVMSPIRVLib 19.1, SPIRV-Tools 2024.1+, glslang 12.2+):

- `SPIRV-LLVM-Translator` branch `llvm_release_190` into `~/.local/spirv-llvm-19`
- `SPIRV-Tools` tag `v2024.4` (with `python3 utils/git-sync-deps`) into `~/.local/spirv-tools-2024`
- `glslang` tag `12.2.0` (with `python3 update_glslang_sources.py`, then `-DENABLE_GLSLANG_BINARIES=ON`)
  into `~/.local/glslang-12.2`. Meson reads the first line of `glslangValidator --version`, so
  `~/.local/glslang-12.2/bin/glslangValidator` is a small wrapper that prints only that line.

The clang static libraries (`clangBasic`, `clangAST`, ...) are built from the LLVM 19.1.7 sources
(`clang` with `-DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm` and tests off) into `~/.local/clang-19`.

`make radv` checks the pinned Mihawk checkouts, sets the paths above and runs Mihawk's
`build-radv.sh release`. The output is
`.deps/mihawk-vulkan-review/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a`.

## Console environment and services

Copy `.env.ps5.example` to `.env.ps5.local` and fill `PS5_HOST` with the console address
and `PROSPERO_TEST_TITLE` with the designated test title. The local file is ignored by Git.
Never use `PPSA99004` as a test slot. The console address has no code fallback.

```bash
cp .env.ps5.example .env.ps5.local
# Edit .env.ps5.local; keep real addresses in this local file only.
source tools/ps5-env.sh
python3 tools/ps5ctl.py doctor
python3 tools/ps5ctl.py status
```

`source tools/ps5-env.sh` exports the settings for builds, benchmarks and other commands
in the current shell. `ps5ctl.py` automatically reads the same local file, with explicit
environment variables taking precedence. `PS5_ENV_FILE` can select a different local
configuration file. `doctor` performs no network calls and checks the bundled payloads'
ELF headers, byte counts and SHA-256 hashes. `PS5_HOST` is required for console commands;
`PROSPERO_TEST_TITLE` is required for test-build and benchmark workflows.

P5 Manager is **optional**. Direct console commands, deployments and backend HTTP tests
work without it. Set `P5_MANAGER` to its URL on the development computer only when using
Remote Play. There is no assumed Manager address. For screenshots, also set `P5_SESSION`
to the active Remote Play session ID. For a locally installed Manager, the URL may be
`http://127.0.0.1:3001`; it is not the PS5 app server on port 11434.

The default service ports in the example are payload 9021, FTP 2120, control 9111,
ShadowMount 10101, kernel log 3232 and shell 2323. `PS5_FTP_PORT` configures `ps5ctl.py`;
`FTP_PORT` configures the older deployment scripts. Custom ports must match the services
actually configured on the console; setting a client variable does not change a payload's
listening port.

After a reboot, the loader must already be available. Restore only missing services:

```bash
python3 tools/ps5ctl.py service restore
python3 tools/ps5ctl.py agent procs
python3 tools/ps5ctl.py shadow info "$PROSPERO_TEST_TITLE"
python3 tools/ps5ctl.py klog --seconds 5
```

The repository bundles zftpd, ps5vkctl, klogsrv and shsrv in `tools/payloads/`, with pinned
hashes, source links and licenses. No `/tmp`, other checkout or console-side payload file
is needed. Individual services can be restored with `service ftp`, `service agent`,
`service klog` or `service shell`. See [payload provenance and rebuilds](../tools/payloads/README.md).
A listener proves that a service is available, not that the application backend is running.

## Deploying a test build

Build the test folder using the sourced environment:

```bash
PROSPERO_APP_TITLE="$PROSPERO_TEST_TITLE" bash tools/build-vulkan-native.sh
```

Before updating a registered folder title, stop it and verify it is unmounted. `agent kill`
expects a title ID, not a PID. Wait until ShadowMount reports `mounted:false`; do not upload
while mounted. Then upload and launch:

```bash
python3 tools/ps5ctl.py agent kill "$PROSPERO_TEST_TITLE"
python3 tools/ps5ctl.py agent procs
python3 tools/ps5ctl.py shadow info "$PROSPERO_TEST_TITLE"
# Continue only after mounted:false:
python3 tools/ps5ctl.py ftp put-dir "build/vulkan-runtime/$PROSPERO_TEST_TITLE"   "/data/homebrew/$PROSPERO_TEST_TITLE"
python3 tools/ps5ctl.py agent launch "$PROSPERO_TEST_TITLE"
python3 tools/ps5ctl.py agent procs
# Confirm the title appears before polling its backend:
curl --fail "http://$PS5_HOST:11434/api/settings/runtime"
```

`ftp put-dir` applies executable mode 755, including eboot and PRX files. First registration
still uses the configured ShadowMount installation workflow. A launch can complete before
HTTP startup; retry the runtime endpoint while the title remains present. If it vanishes,
capture klog and recheck processes. Do not interpret a failed connection to a stopped title
as a model result. Backend generation/media checks use this direct HTTP API; Remote Play
is needed only for visual UI verification. Read a screenshot before every automated Cross
press. Never reboot the console or overwrite the production title without separate approval.

To measure every installed model with a model switch on each request (the path the UI
uses), run against the deployed test title:

```bash
python3 tools/benchmark-all-models-ps5.py --rounds 3 \
    --output docs/performance/<date>/all-models.json --artifacts build/benchmarks/all-models
```

Text models get a correctness check and a 128-token decode measurement; media outputs are
downloaded and validated. A title crash is recorded and the title is relaunched.

## Status

The Vulkan folder title and network API have been verified on PS5 FW 12.70. Mistral 7B Q4_0 reaches a median 69.72 decode tokens/s versus 47.96 for the official upstream AGC release. The optimized isolated loader prepares a raw folder GGUF in a median 2.84 seconds across three fresh process launches; AGC was rechecked at 1.53 seconds. Compressed ffpfsc model reads remain much slower. See [loading measurements](VK_LOAD_BENCHMARK_2026-10-09.md). See [current status](VK_ACTIVE.md) and the [benchmark report](VK_BENCHMARK_2026-10-08.md) for protocol and limitations.

## PS5 llama.cpp Vulkan smoke tests

The smoke title links llama.cpp and GGML from the shared runtime build; it no longer compiles a
second llama/GGML tree. `make llama-ps5` remains a compatibility alias for the common runtime
library build. The WIP console build uses the reviewed RADV archive and SDK under
`.deps/mihawk-vulkan-review/`. Build the host tools and host shader generator first, then:

```bash
make llama-ps5           # compatibility alias for shared GGML/llama/SD/audio/Kokoro libraries
PROSPERO_TEST_TITLE=<TEST-TITLE-ID> make vulkan-smoke       # instance and device discovery
PROSPERO_TEST_TITLE=<TEST-TITLE-ID> make llama-vulkan-title # tiny GGUF Vulkan/CPU validation
PROSPERO_TEST_TITLE=<TEST-TITLE-ID> make app-vulkan         # complete UI/HTTP test app
```

Model tests expect `build/vulkan-models/stories260K.gguf`. Set `MODEL_GGUF=/absolute/path/model.gguf` to package another model. Production Vulkan and AGC runtimes use the `/data/prosperoai/models` directory: Vulkan discovers GGUF files and AGC discovers prepared model folders. The isolated loading benchmark still packages its fixture inside `/app0/models`. The Vulkan Models screen can browse public Hugging Face repositories and download verified GGUF files to the shared directory. Downloads use file-size memory estimates: below 8 GiB is marked fits, up to about 10.7 GiB is tight, and larger files are excluded. These estimates do not guarantee every model/context combination fits. Context length is selectable or Auto; KV precision is automatic (f16 up to 8K, q8_0 above). These new runtime choices await console verification. The app reaches that directory with the filesystem access Lapy gives it at launch (`vulkan/storage.cpp`); the test title <TEST-TITLE-ID> gets a Lapy helper built for its own title ID. Outputs are in `build/prospero-vulkan/`, `build/llama-vulkan-title/`, or `build/vulkan-smoke/`. These targets do not deploy. Never overwrite PPSA99004 for testing.

Build jobs default to available CPUs (four on this host). Compiler cache defaults to `build/ccache`, including host tools. Model copies and multi-gigabyte image compression still take time. Host iGPU is not used for compilation.

For development, `make app-vulkan-folder` skips image compression. Upload the folder with `python3 tools/ps5ctl.py ftp put-dir build/prospero-vulkan/<TEST-TITLE-ID> /data/homebrew/<TEST-TITLE-ID>`, then rescan with ShadowMount. The FTP helper sets executable permissions on eboot.bin and PRX modules. An earlier folder launch refusal (0x80aa001a, errno 13) was fixed by SITE CHMOD 755 on these files. Once registered and stopped, update only changed files; immutable model files need not be uploaded again. Keep the same title metadata and do not replace an image while its mount is still live.

## OpenAI API and OpenCode

To build the isolated model-loading benchmark in a dedicated test slot:

```bash
make llama-ps5           # compatibility alias for the shared PS5 runtime build
PROSPERO_TEST_TITLE=<TEST-TITLE-ID> PROSPERO_LOAD_TEST_TITLE=<LOAD-TEST-TITLE-ID> \
  LLAMA_SMOKE=1 LOAD_BENCHMARK=1 MODEL_GGUF=/absolute/path/model.gguf \
  bash tools/build-vulkan-smoke.sh
```

It measures three alternating serial/parallel pairs, separating weight loading, context
creation and first evaluation, and compares the first greedy token. The Vulkan device
and process are shared across rounds; these are repeated loads, not independent cold
launches. The GGUF must be packaged in `/app0/models` because the title sandbox cannot
read another title's model directory. Read the report through FTP at
`/mnt/sandbox/<LOAD-TEST-TITLE-ID>_000/download0/prospero-load-benchmark.txt` while the test is running.
Installation and repeated launches within a console-testing task are covered by the authorization rules in `AGENTS.md`.

The Vulkan build also exposes `/v1/models` and `/v1/chat/completions`, including SSE and portable function calls. See [OpenCode configuration](OPENCODE.md) for bearer keys, context sizing and protocol limits. The native Vulkan title links the existing AGC media backend alongside Vulkan text inference. Voice generation and switching back to Vulkan text have been verified on console; Image and Audio generation in this combined build have not yet been exercised.

Default Vulkan backend logging forwards WARN/ERROR. Create `/app0/vulkan_verbose_logging.txt` only for detailed diagnosis; synchronous full debug output materially slows loading. Create `/app0/vulkan_profile.txt` in a benchmark package to enable ggml's per-operator GPU timestamp report; profiling waits for query results and must not be used for throughput claims. The HTTP generation endpoint accepts benchmark-only `options.num_batch` and `options.num_ubatch` overrides (32–2048); changing either recreates the context. For first-load-only benchmark runs, add `/app0/load_parallel_only.txt` (three optimized loads per launch).

## Native UI Vulkan presentation probe

`PROSPERO_TEST_TITLE=<TEST-TITLE-ID> PROSPERO_UI_TEST_TITLE=<UI-TEST-TITLE-ID> UI_PROBE=1 bash tools/build-vulkan-smoke.sh`
builds an independent display probe in its configured test title. This verifies graphics
presentation without OpenGL or a model;
upstream UI design remains unchanged. See [migration stages](VK_UI_MIGRATION.md)
and [current evidence](VK_ACTIVE.md).

### Download presets

The native Models library includes pinned Text (Mistral 7B Q4_0, Qwen3.5 9B Q4_0,
Granite 4.0 H-Tiny Q4_K_M),
Image (SD-Turbo FP16), experimental Audio (Stable Audio 3 Small SFX Q8_0 and ACE-Step 1.5 Turbo Q8_0), and Voice (Kokoro 82M Vulkan)
presets. Cross downloads the selected preset; L2/R2 change category and arrows select
cards. The progress bar shows bytes and percentage for the entire bundle. Search typing
uses Triangle for Space and R2 for Done.

Text presets download standalone GGUF files for Vulkan. Media presets download pinned
model files for their Vulkan adapters. Only one inference model is active at a time. Every
file is pinned to a repository commit and checked against its size and SHA-256. Media
bundles are staged in hidden directories until all files pass verification. Failed
staging files are overwritten on retry and never appear as usable model bundles.

`model-tools/presets.json` records the pinned curated model manifests; the original
Text presets also use source entries in the conversion recipes. Run `python3 tools/generate-model-presets.py`
after changing these records to regenerate `include/model_presets.hpp` without network
access. Refreshing manifests requires fetching every repository tree page and computing
SHA-256 for small files whose Hub entry has no LFS hash. Never use a truncated tree as
a complete bundle.

Storage errors include errno and distinguish missing storage access from disk write
failures. Downloads go through libcurl (`vulkan/net/`): the console's own HTTPS client
refuses every public site once the app has filesystem access (`0x8095F00C`, it no longer
finds the certificate authorities). libcurl, OpenSSL, libpsl and zstd come from the pinned
PacBrew prefix and are linked into one object with `console_curl.c`, whose stand-ins for
libc get names of their own there. Each socket asks for a receive buffer of up to 4 MB
before it connects: with the console's 64 KB default a distant server delivers about
1 MB/s whatever the line could carry.

The HTTP UI exposes the same presets through `/api/models/presets` and shows download
progress from `/api/models/download`; `DELETE /api/models/download` cancels the running
download and removes its partial files. Workspace category buttons start a conversation
with an installed model of that type, or open its Models category when none is installed.
Back to Workspace preserves browser history; each history row has a confirmed delete action.

## Hybrid release packaging

`make app-release` builds the standard dependencies, Vulkan llama.cpp archives and
the native Vulkan/AGC app, then packages `dist/PPSA99004.zip` using the identity and
version from `sce_sys/param.json`. It packages Lapy's one-request helper built for
PPSA99004 (`lapy.elf`, `lapy-manifest.json`, `licenses/Lapy-MIT.txt`) and `INSTALL.txt`;
no models are bundled. At launch the app asks a running Lapy service for filesystem
access and otherwise sends the helper to the console-local ELF loader on port 9021; the
loader must be available in the configured homebrew environment, manual payload upload
is not needed. Without access the app keeps its data in its sandbox. A production release build is never
automatically deployed to the console.

The package also carries `self-updater.elf`, the helper of the in-place update
(`third_party/self-update-helper`, built with the PS5 Payload SDK the Lapy helper uses), and
`licenses/miniz-MIT.txt`. The app asks the homebrew.page catalog once per launch
(`vulkan/update_ps5.cpp`, `third_party/update-check/README.md`). To try the update before a
release is listed, build with `UPDATE_DEV_OFFER=1` and put an `update-offer.txt` in the app's
folder: such a build takes the offer from that file and skips the catalog's signature, so it
is never shipped. `tests/console/update.txt` is a scripted run of the question and its notes.

### Launch metadata

`sce_sys/param.json` carries two values that decide whether the console changes the
television's video mode when the app opens and closes. A change blanks the picture for a
second or two before the launch picture appears.

- `attribute` is `0x62000000` (`1644167168`), the value the other Prospero apps use: the
  console lists the title as HDR-capable (`HDR:o`) and leaves an HDR output as it is. With
  `0` a console set to use HDR only when supported switched the output to SDR at launch.
- `attribute3` is `0x100000` (`1048576`), "VRR off". With `0` a console set to apply VRR to
  unsupported games switched the output into VRR at launch (`VRR:Boost`). System software
  6.02 does not know the bit, says so in its log (`unknown vrr parameter`) and leaves VRR
  off, which is the wanted result.

The console's log shows what it decided, and how long the change took:

```text
[AvControl] -- app[0](appid=... attr=...)(GAME RUNNING)(HDR:o HFR:x VR:x VRR:x)
[AvControl] App Event[POST_BEGIN]: elapse 0[ms]
```

Measured on a PS5 with system software 6.02 on a VRR and HDR television: about 1,790 ms
with `0` and `0`, 0 ms with these values. The console reads the values from the installed
folder's `param.json` at every launch, so an update is enough. `make lint` refuses other
values.

The GitHub workflow uses Ubuntu 24.04 LLVM 19 host libraries with the pinned Mesa
and SDK sources via `tools/setup-vulkan-ci.sh`, then builds the application with
Clang 18. The existing developer `make radv` path remains available on Debian.
Tag releases are built and attested in CI; published ZIPs are never overwritten.

## Experimental shared Vulkan media builds

`make app-vulkan-media-folder` builds/signs `build/prospero-vulkan-media-native/<TEST-TITLE-ID>`
with the shared Vulkan audio/TTS bridge and legacy SD. `make app-vulkan-shared-folder`
builds/signs `build/prospero-vulkan-shared-native/<TEST-TITLE-ID>` with source-built SD, llama and
Vulkan against one 160-name ggml ABI, without private SD/tts ggml archives. Legacy AGC text
still exists for comparison; CPU media kernels remain. Neither target deploys automatically.
Default packaging retains the comparison runtime until console correctness/speed gates pass.
Lapy source/SDK/protocol remain those of release 01.003.000 with title-specific helper identity.

`make sd-vulkan-check` now builds that same shared llama/SD stack. The original 64-vs-160
ABI mismatch is resolved by rebuilding all consumers consistently, not by overriding SD alone.
For host ABI/device/decoder/parallel/parser and real-Vulkan numerical tests, run:

```sh
CC=clang-18 CXX=clang++-18 bash tools/build-vulkan-runtime.sh host
TMPDIR="$PWD/build/context-validation-tmp" HOST_CXX=clang++-18 make test
```

The Vulkan tests use llvmpipe when present and do not benchmark the PS5 GPU. Shared tests
skip when the optional host build is absent; the tiny GGUF decode runs when the existing
`build/vulkan-models/stories260K.gguf` fixture is present. [Integration details](VK_MEDIA_MIGRATION.md).

## MiniCPM5 optional preset

See [MINICPM5.md](MINICPM5.md) for its pinned model and template tests.

## Bonsai 2 and existing text models

`make app-bonsai-folder` is the legacy isolated Prism build for the Ternary Bonsai 2
27B PTQ1_0 preset. The shared upstream runtime does not yet support Prism's private
PTQ1_0/PQ2_0 quant types; keep Bonsai disabled there until its compatibility adapter
is ported and validated. This experimental folder build does not deploy or change the
production title.
See [BONSAI2_PS5.md](BONSAI2_PS5.md) for inputs, console measurements and limitations.
