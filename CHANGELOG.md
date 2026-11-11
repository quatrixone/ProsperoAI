# Changelog

## 01.007.000

- Updated the shared Vulkan inference runtime to upstream llama.cpp and GGML, including PS5
  and Ternary Bonsai compatibility for the upstream API changes.
- Image Studio now passes uploaded images and masks reliably, and SDXL ControlNet keeps its
  model weights resident alongside the guide network during sampling.
- Fixed Cover and Stems rejecting valid WAV files on PS5; both workflows now produce their
  expected artifacts.
- Added symlink-safe file checks for image upload leases and audio workflow inputs.
- Verified all 25 installed curated model presets and their applicable workflows on the PS5
  test title. Two optional local GGUFs remain outside the supported catalog because they
  exceed available console Vulkan memory.

## Unreleased

- Search now reaches 41 providers in nine selectable groups from the console itself: the
  general web, code hosting, question sites, reference works, package registries, papers,
  video, music and images. One query asked every provider in about eight seconds on the test
  title, results carry their source and a score that ranks pages several providers agree on
  first, and each provider's outcome is reported so throttling is distinguishable from an
  empty index.
- Added `/fetch <url>` and `POST /api/fetch`, which read one page as bounded plain text so a
  model can follow a result. The command works in both conversations, the MCP bridge for
  OpenCode gained the same tool, and the bridge accepts search scopes.
- Settings now has a checkbox per group of sites, so a search can be narrowed to the groups
  worth asking, and **Test search** reports how many providers answered.
- `POST /api/fetch` now answers 400 for an address it cannot read, keeping 502 for a page
  that was asked for and failed.
- Search results are cached on the console for a day and the cache survives a restart of the
  app, so repeating a query answers in milliseconds and does not ask the sites again.
- Search no longer allocates the model downloader's 4 MB receive buffer: it borrows about
  64 KB per provider and releases it with the request. Resident memory was unchanged across
  328 provider requests, and the search module imports no graphics symbol at all, so a
  search cannot take video memory from a loaded model. The HTTP client gained request headers, a per-session user agent,
  cookies, compressed transfers, POST and a caller-chosen buffer size.
- Replaced the single DuckDuckGo HTML route, which answers HTTP 202 with a throttle page
  after a few queries; that page is now recognised and reported as rate limiting.
- On-screen keyboard: a held direction keeps moving, and a held Square keeps deleting. L2
  switches between letters and symbols and L1 is Shift; R2 is only Done (Send in a
  conversation). R2 used to switch to symbols and press Done at once, with no way back to
  letters.

## 01.006.000

- Text, image, sound and speech generation now use one pinned upstream llama.cpp/GGML Vulkan
  runtime. The release includes guarded PS5 support for Ternary Bonsai 2's ternary weights and
  replaces Kokoro's old prebuilt OpenCL path with a source-built Vulkan runtime.
- Added 12 ready-to-download model presets across all four modalities, including Ternary
  Bonsai 2 27B, MiniCPM5, Granite, BoHF, Stable Audio 3, ACE-Step, Kokoro and Chatterbox.
  See [Supported models](docs/SUPPORTED_MODELS.md) for the complete list.
- Added Chatterbox voice cloning with 19 language choices, custom voice samples, and voice
  recording from DualSense or a web browser.
- PS5 and web now share conversations and runtime settings. Models prepare when a conversation
  starts, both interfaces show actual backend residency, and model switching and memory release
  work across text, image, audio and speech.
- The PS5 Workspace places New conversation after the voice controls, and the default idle
  timeout before releasing a model is now five minutes.
- All 12 curated presets completed three generation rounds each on the test title, including
  checks for model switching and valid generated outputs.

## 01.005.000

- Output and context ceilings are now 131072 and 262144 tokens; response and session
  storage grow dynamically to fit. Leaving output length unset (API) or on its new "Auto"
  default (native and web) fills the remaining context instead of stopping at 512 tokens.
- Generated images and audio now play, preview and download directly in the web chat,
  matching what the native app already did; the model downloader's fit estimate now reads
  the console's actual reported memory instead of a fixed figure.
- Fixed the web Settings page rejecting a 256K context selection, several mobile layout
  issues (the Models search/filters row, the chat header, and the conversation history
  drawer), and conversation drafts left without a message no longer clutter history.
- Raised the HTTP server's concurrent-connection limit so an ordinary page load no longer
  risks "Too many active connections."

## 01.004.000

- Text, image, audio and voice now use one Vulkan runtime. The older AGC inference
  implementations and duplicate runtime copies have been removed.
- Added the pinned Ternary Bonsai 2 27B PTQ1_0 preset. The release also supports the
  optional MiniCPM5 2B Q8_0 model.
- Web and PS5 conversations now use the same saved-session store, so a conversation can
  be continued from either interface. Existing browser-only history is migrated on first
  sync, and deletion removes the shared conversation and its categorized outputs.
- Added larger context and response controls while preserving existing defaults, and
  reused evaluated prompt prefixes when continuing compatible conversations.
- Model downloads now report memory fit, can use parallel transfers, and resume an
  interrupted transfer instead of starting over. Cancelling still removes partial data.
- The app opens without a second or two of black before its launch picture: it now declares
  HDR output and no VRR, so the console no longer changes the television's video mode when
  the app opens and closes.
- The About screen credits Quatrixone.

Contributors to this release: [Quatrixone](https://github.com/quatrixone),
[Bilawal Riaz](https://github.com/bilawalriaz), and
[BlackBearReloaded](https://github.com/blackbearreloaded).

## 01.003.000

- ProsperoAI says when a newer version is listed on homebrew.page, shows its release notes
  and can install it in place: it downloads and checks the release, closes, and a helper
  replaces the app's files. Models, settings and conversations are not touched.

## 01.002.000

- Image generation works in the combined build: the image runtime has a memory region of
  its own again, shared with the GPU, instead of the app's heap.
- Generated voice and sound clips play (Triangle): they go through the interface's mixer
  instead of a second audio output the console refused.
- Prepared `.ps5lm` text bundles (Mistral 7B, Qwen3.5 9B) are found and run on AGC beside
  GGUF models on Vulkan; one model is in memory at a time and its memory is given back
  when another kind of model is chosen.
- The sound model finds its files: its paths are no longer fixed to the app folder.
- Storage access comes from Lapy, as in the other Prospero apps: a running Lapy service or
  the packaged one-request helper. Models, settings, conversations and logs live in
  `/data/prosperoai`; models from `/data/homebrew/prosperoai/models` are moved there at
  the first start. The bundled nullfs mount helper is removed.
- Downloads use libcurl with a socket receive buffer of up to 4 MB instead of the
  console's 64 KB, which held distant servers to about 1 MB/s; they also work with
  storage access, where the console's own HTTPS client fails.
- Saved conversations open again from the list: the store only accepted a time with
  seconds, and the interface writes hours and minutes. A conversation also keeps its
  model's whole ID, so one made with a downloaded GGUF file can be continued and finds its
  model when it is opened again.
- On a USB keyboard, Tab is R1 and Shift+Tab is L1.
- A running model download can be cancelled: Triangle in Models, the Cancel button of the
  browser page, or `DELETE /api/models/download`. What was downloaded so far is removed.
- Settings > Diagnostics > Debug log writes `/data/prosperoai/logs/debug-trace.txt` for
  problem reports.
- New launch picture.

## 01.001.000

- Combined Vulkan llama.cpp text inference with the existing AGC Image, Audio and Voice
  backends in one app. Kokoro Voice generation and returning to Vulkan text are verified
  on PS5; Image/Audio in this combined build remain unverified on console.
- Added pinned downloadable Text, Image, Audio and Voice presets, verified bundle staging,
  aggregate progress bars and Downloading states in both native and HTTP Models.
- Fixed HTTPS/CDN redirects and writes into the sandbox-mounted shared model directory.
  Standalone GGUF downloads are capped at 7 GiB and exclude split/mmproj files.
- Models category navigation uses L2/R2; search typing uses Triangle Space and R2 Done.
- Web Workspace cards select the corresponding installed model or open its preset category.
  Conversations can be deleted with confirmation and Back to Workspace retains history.
- Updated the PS5 icon and background, removed competing launch artwork and lowered
  native splash lettering. README uses the same golden doorway icon.
- Shared model storage is set up automatically by the app using its bundled title-scoped
  helper and the console-local ELF loader. No separate payload upload is needed.
  The model-free release contains no downloaded weights.

- Replaced the RmlUi interface with a native Vulkan one built on ps5-homebrew-ui: Workspace,
  Models and Settings pages, a model library without the eight-model limit, search and
  filters, Midnight and Daylight themes with three accents, an opening sequence, a backdrop
  that follows the model in focus, notices, interface sounds, reduced motion, high contrast
  and a larger reading size.
- The interface kit, its fonts and its sounds are fetched at a pinned commit when the app is
  built; nothing of the kit is kept in this repository. RmlUi, FreeType and the old
  interface's assets are removed.
- Chinese, Japanese and Korean are drawn with scalable Noto Sans faces, loaded when a text
  first needs them; accented Latin, Greek and Cyrillic are part of the interface faces.
- Sending a new message after a failed answer replaces the unanswered one instead of storing
  two user turns in a row.
- The token count, speed and context size of the last answer are shown again, and Settings
  has an About page with the version.
- Moved to ps5-opengl SDK 1.0.1.
- The diagnostic log is off unless the install folder holds `dev/log.txt`.
- A scripted, self-ending run for tests on a console (`dev/request.txt`,
  `tools/console-run.py`, `tests/console/first-run.txt`); inert without the request file.
- Adopted the foundation's parallel incremental Ninja build and ccache-backed
  local/CI compilation workflow.
- Fixed RELRO load-segment alignment for executables whose relocated read-only
  region does not begin at the GOT, with a host regression test.
- Published the deterministic text-model converter, validated Mistral and Qwen
  recipes, recipe-authoring workflow, and model-tool self-test.

## 01.000.000

- Published the complete reproducible ProsperoAI application source.
- Added model-free GitHub Actions builds for the app-folder ZIP.
- Documented curated model installation and measured PS5 performance.
- Added the linked video demonstration.
- Kept the 64 most recent saved sessions ordered by activity.
- Completed application-wide ProsperoAI branding cleanup.

## v0.1.0-alpha.3

- Added curated text, image, general-audio, and speech model support.
- Added persistent multimodal sessions and in-app image/audio presentation.
- Added model switching, loading feedback, USB keyboard input, and conversation scrolling.
