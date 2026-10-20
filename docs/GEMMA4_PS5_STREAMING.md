# Gemma 4 26B-A4B bounded-memory inference on PS5

The goal is to run Gemma 4 26B-A4B through ProsperoAI's pinned llama.cpp and
Vulkan runtime with a Vulkan working set around 5 GiB. This is a target, not a
result. It does not cap total process RAM: mmap pages, KV, activations, Vulkan
staging and runtime allocations also consume system memory. The model file
still needs roughly 15 GiB of storage.

TurboFieldfare reaches its Mac memory target by keeping the shared model core
resident, keeping a bounded routed-expert cache, and reading expert misses from
the model file into reusable GPU-visible slots. Its Metal kernels, Swift runtime
and `.gturbo` pack are not portable to PS5. The reusable design is the explicit
expert index, positional reads, bounded slots, cache policy and fence-safe slot
reuse. ProsperoAI should keep GGUF and its Vulkan quantized kernels.

## Implementation gates

1. **S0: size the actual GGUF.** `tools/gguf-stream-budget.py` reports quantized
   bytes per routed expert and an uncached per-token read estimate from the
   tensor extents and `expert_used_count` metadata. This is an I/O lower-bound
   input, not a memory or speed result. Measure actual Gemma 4 routing and
   expert-cache hit rates before choosing a cache size.
2. **S1: lazy expert experiment — blocked on PS5.** The pinned llama.cpp already has
   mmap-backed lazy tensors. Mark Gemma 4 routed expert matrices lazy and run
   them on the CPU backend, while the other tensors stay on Vulkan. This keeps
   full expert matrices out of eager backend allocations and gives us a first
   PS5 memory/correctness measurement. This is an opt-in experiment; it does not
   yet provide a bounded cache or guarantee a physical-memory ceiling.
   Build the test title with `PROSPERO_GEMMA4_STREAMING=1`; this opts that
   build into Gemma 4 tensor planning. The selected model name must include
   `gemma4` or `gemma-4`. The PS5 loader now rejects lazy mappings before mapping
   the GGUF or allocating weight buffers; this build cannot run Gemma inference.
3. **S2: replace the blocked path with explicit bounded expert reads.** Keep embeddings,
   attention, routers, shared experts and KV resident. Read only routed expert
   extents into a fixed number of aligned Vulkan-visible slots. Do not allocate
   full expert tensors. Add slot lifetime tracking so a Vulkan submission cannot
   read a slot while it is being refilled. Keep the resident llama.cpp path as
   the reference.
4. **S3: prove host correctness.** Compare logits and greedy tokens against the
   resident path for short prompts, repeated experts, cache eviction, failures,
   cancellation and model/context teardown. Validate all GGUF quantization
   layouts used by the selected Gemma 4 file.
5. **S4: measure on a PS5 test title.** Report physical memory by category,
   uncached and warm read bandwidth, expert-cache hit rate, prefill, time to first
   token and decode speed. Report Vulkan allocation separately from process RSS
   and system memory. Do not advertise the 5 GiB Vulkan target until these
   measurements pass.

This PR keeps S0's GGUF budget report, S1's opt-in tensor planning diagnostics and
the first part of S2: a bounded expert reader with host parity tests, not yet
connected to the model loader or the ggml scheduler. It does not claim Gemma 4
fits in 5 GiB of Vulkan memory.

## Initial Q4_K_M size probe

The text GGUF `SoAIHQ/gemma-4-26B-A4B-it-GGUF`, file
`gemma-4-26B-A4B-it-Q4_K_M.gguf`, is 16,796,018,080 bytes with SHA-256
`d13ab520dd546f6392bdcdcebba45efa73a43a36ace9a387f223f5d87b906cac`.
The S0 parser found 128 experts, 8 used per token and 30 routed-expert layers.
The uncached expert-read lower bound is 945,635,328 bytes per token, with a
largest single expert extent of about 4.14 MiB. These are file-layout estimates;
they do not measure the OS page cache, CPU residency, Vulkan memory or latency.

## S1 first PS5 attempt — failed, 2026-10-20

The opt-in build discovered the GGUF and entered Gemma 4 lazy mode. A one-token
generation request did not return, and `/api/models/memory` never reported a loaded
model. This does not prove successful model loading or inference.

The test folder also contained an old `dev/request.txt` UI script from a MiniCPM5
test. It switched models, timed out waiting for readiness, then requested app close
while the HTTP generation was still running. The app trace recorded `HUI quit requested`;
klog recorded a fatal user-thread signal and `SCE_SYSCORE_EVENT_APP_CRASH`. The
console became unresponsive and required a restart. The exact fatal signal/backtrace
was not captured, so the close race is a strong lead, not a proven root cause.

The test build now ignores `dev/request.txt` when the Gemma streaming marker is present.
The UI also blocks app close while an HTTP generation is active, and the main loop waits
for active generations before teardown. These are crash-risk guards; they do not make
Gemma's memory use safe, and S1 remains unverified. Retry only on the test title with the
guarded build and monitor title process, Vulkan memory, trace and klog during each stage.

## S1 guarded PS5 retry — loader stalled, 2026-10-20

After restarting the console, the guarded build was deployed to PPSA99023 and
confirmed running. The stale `dev/request.txt` was present on disk, but its
script did not run. Vulkan reported 11,905,269,760 bytes free of 12,280,922,112
before loading, then entered Gemma 4 lazy mmap mode. GGUF token metadata was
read, but the model never reached a loaded state. A one-token request produced
no response within 180 seconds; `/api/models/memory` continued to report
`loaded:false`. The observed title RSS rose from about 570 MiB at idle to about
2.1 GiB during loading; this is process memory, not a Vulkan allocation
measurement. No 5 GiB Vulkan result was reached.

After the stalled load, FTP refused connections, P5 Manager returned 502, and
agent/process and ShadowMount queries stopped responding. The HTTP API still
answered, and `agent kill PPSA99023` returned without output or confirmation.
The title's final process/mount state could not be verified. No reboot was
performed. This attempt still does not prove inference or identify the precise
cause of the control-service failure; further Gemma load attempts need a safer
loading strategy and a way to bound/cancel model loading before retrying.

## Guarded tensor planning — passed, 2026-10-20

Source inspection found that `LLAMA_LAZY_MODE_ON` bypasses
`LLAMA_LOAD_MODE_NONE`: `init_mappings()` maps every model file when lazy tensors
exist. PS5's `prospero_posix_madvise` returns `ENOSYS`, so the lazy-range advice
is unavailable. The earlier VM-size increase of roughly 16,024 MiB matches the
GGUF size. These facts identify an unbounded path; they do not prove the exact
kernel or allocator failure behind the service outage.

The source-preparation hook now rejects PS5 lazy mappings in `init_mappings()`,
before the mmap call and backend weight allocations. The adapter retains INFO
logs during Gemma loading. Previously these were filtered, so the last visible
token warning did not locate the stalled phase.

The guarded build (`eboot.bin` SHA-256
`060eabcd22152a4968ebd678fb3abed344a61cdc08bb2a3cb8895f2bba7271b9`)
was cross-built and deployed to PPSA99023 after verifying it was unmounted.
Its NEEDED dependencies and LOAD/DYNAMIC header layout match the previous build.
On console, vocabulary loading and all 30 routed-expert layers reached tensor
planning. The guard raised its expected error at app uptime 9.091 seconds;
HTTP returned `done:true`, `error:"generation failed"`, with zero generated tokens.
Afterwards, the title had about 680 MiB RSS and FTP, shell and process control
still responded. Closing PPSA99023 returned result `0x00000000`, then process
count zero and `mounted:false`. No agent-initiated reboot was needed.

This verifies rejection and teardown of this load attempt. It does not verify
Gemma inference, a 5 GiB allocation ceiling, or eliminate every crash cause.
Local evidence is under `build/gemma4-load-guard/`.

### Constraints for bounded expert reads

The pinned ggml scheduler already copies selected experts for offloaded
`MUL_MAT_ID` weights, but its copy path directly dereferences
`input->data + expert_offset`. Merely replacing mmap with a small buffer would
therefore be incorrect. A bounded reader must supply the selected file extents,
handle fused gate/up tensors and quantization row strides, and keep upload
storage alive until GPU transfers complete. Unsupported CPU fallbacks must
fail explicitly rather than dereference a tensor whose full weights are absent.
Measure the scheduler's destination buffers as well as the expert cache when
enforcing the Vulkan budget.

## Bounded expert reads — host parity passed, 2026-10-20

`inference/streaming/expert_cache.cpp` reads one routed expert at a time with
`pread` into a fixed set of 64-byte-aligned slots. The budget and slot size are
fixed at construction, so neither a miss nor an eviction can grow the payload
allocation. A slot is handed out as a move-only lease that pins it; eviction
picks the oldest unpinned slot and raises `CacheBusy` when every slot is pinned.
A failed or short read is never published as a hit, cancellation is checked
before and during each read, and a lease keeps its bytes valid after the cache
and the original file descriptor are gone. `pread` leaves the loader's file
cursor untouched.

`inference/streaming/expert_upload.hpp` holds the lease across
`ggml_backend_tensor_set_async` and synchronizes before overwriting the
destination, so neither the source bytes nor earlier GPU work can be lost. This
is the baseline, not the pipelined version: a future overlapped implementation
must keep both lifetimes with backend events.

Host verification (`bash tools/build-vulkan-runtime.sh host`, then
`python3 -m unittest discover -s tests -p test_vulkan_runtime.py`):

- `expert-cache-test`: ranges, pins, eviction order, `CacheBusy`, cancellation,
  partial-EOF rejection, 8-thread concurrency and lease lifetime.
- `expert-upload-test`: routed `MUL_MAT_ID` output matches identical fully
  resident weights for Q4_K, Q6_K and Q4_0 at 32 and 64 rows across five
  routing changes, on the CPU backend and on Vulkan (llvmpipe), with two CPU
  cache slots for eight experts.

This proves the reader and the upload path in isolation. It does not touch the
production loader, so the PS5 mmap guard stays in place. S2 still needs the
scheduler integration described in the constraints above; S3 and S4 remain open.

## Console measurement endpoint

The streaming experiment build (`PROSPERO_GEMMA4_STREAMING=1`) compiles
`inference/streaming/expert_probe.cpp` and answers
`POST /api/debug/expert-stream`. A release build links neither. The body takes
`model` (relative to the model folder), `slots`, `tokens`, `experts_per_token`,
`layers` and `seed`; the reply reports the file's routed layout and what the
reads cost. The probe loads no model and allocates no backend memory, so it runs
while nothing is loaded, and its routing is a deterministic uniform stand-in:
real Gemma 4 routing is skewed, so a measured hit rate here is a lower bound.

### `dup()` is not available to a PS5 title

The first console run crashed the HTTP thread immediately:
`PPRBUG-22859: the process pid=105 directly issued a syscall 41`, then
`exception: 0xa002030a (SYSTEM_ILLEGAL_FUNCTION_CALL)` in thread
`prosperoai-http-conn`. FreeBSD syscall 41 is `dup`, which the clean-room libc
issues directly and the kernel rejects; nothing else in the app had used it.
`ExpertCache` now opens and owns the file when constructed from a path, and the
descriptor constructor borrows without duplicating. The host tests cover both.

## Bounded expert reads on PS5 — measured, 2026-10-20

Test title PPSA99023, `eboot.bin` built with `PROSPERO_GEMMA4_STREAMING=1`,
NEEDED list and PIE program-header shape identical to the verified guard build.
Model `gemma4/gemma-4-26B-A4B-it-Q4_K_M.gguf` on console storage.

The probe found 30 routed layers and 90 per-expert tensors (fused gate/up, down,
and the rank-1 `ffn_down_exps.scale` vectors), 128 experts with 8 used, and a
largest single expert extent of 2,230,272 bytes. Its uncached per-token total,
945,636,288 bytes, matches `tools/gguf-stream-budget.py` on the host byte for
byte, so the console reader and the S0 estimate agree on the layout.

| slots | cache bytes | tokens | reads | evictions | cold | warm |
| --- | --- | --- | --- | --- | --- | --- |
| 16 | 35,684,352 | 3 | 2,160 | 2,144 | 1,117.6 ms, 807 MiB/s | 1,746.8 ms, 1,033 MiB/s |
| 128 | 285,474,816 | 3 | 2,160 | 2,032 | 1,143.6 ms, 789 MiB/s | 1,718.1 ms, 1,050 MiB/s |
| 8 (1 layer) | 17,842,176 | 4 | 96 | 88 | 36.0 ms, 919 MiB/s | 81.7 ms, 1,215 MiB/s |

What this establishes: the reader works on the console, its allocation stays at
`slots x max_expert_bytes` regardless of how many reads and evictions follow,
and PS5 storage serves these extents at roughly 0.8 GiB/s cold and 1.0-1.2 GiB/s
warm. At that rate one token's uncached routed-expert reads cost about 0.9-1.1 s,
so expert I/O alone caps decode near 1 token/s unless the cache hits.

What it does not establish: hit rates under real routing (the stand-in is
uniform, and every run above recorded zero hits), inference correctness, decode
speed, or the 5 GiB Vulkan ceiling. No model was loaded and no expert bytes
reached the GPU on the console. The title stayed alive and responsive through
every run, and was stopped and unmounted afterwards.
