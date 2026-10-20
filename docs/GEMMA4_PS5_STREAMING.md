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

Test title PPSA99023, `eboot.bin` built with `PROSPERO_GEMMA4_STREAMING=1`, NEEDED
list and PIE program-header shape identical to the verified guard build. Model
`gemma4/gemma-4-26B-A4B-it-Q4_K_M.gguf` on console storage.

The probe found 30 routed layers and 90 per-expert tensors (fused gate/up, down,
and the rank-1 `ffn_down_exps.scale` vectors), 128 experts with 8 used, and a
largest single expert extent of 2,230,272 bytes. Its uncached per-token total,
945,636,288 bytes (902 MiB), matches `tools/gguf-stream-budget.py` byte for byte.

### What the first console measurements changed in the design

A single pool of uniformly wide slots measured 0% hits at every size up to 1 GiB,
and two reasons showed up in the numbers:

- Every slot was as wide as the widest tensor, so a 4-byte `scale` expert cost
  2.23 MB of cache.
- One pool of LRU slots is swept clean while the other 29 layers are read: the
  reuse distance is one token's whole working set, 720 entries, so nothing
  survived until the next token.

The cache now keeps **one pool per routed tensor, each slot exactly one expert
wide**, which is what TurboFieldfare's per-layer cache does. `slots` is resident
experts per tensor. Reads of one expert are split across a fixed set of reader
threads (`read_workers`).

### Real routing instead of a uniform stand-in

`tools/gemma4-route-trace.cpp` (host target `gemma4-route-trace`) records the
router's own top-8 per layer per step through llama.cpp's eval callback on the
`ffn_moe_topk` nodes, writing `step layer id id ...`. The probe replays that
trace with `"trace": "<file in the model folder>"`.

A 12-token prompt plus 16 greedy decode steps gave 44.1% expert overlap between
consecutive steps and 1,165 distinct (layer, expert) pairs of 3,840. Replaying it
on the console reproduced a host simulation of the same trace to within rounding
(decode hit rate by bytes 31.7% / 57.6% / 71.5% at 8 / 16 / 32 experts per
tensor, against 32.0% / 58.1% / 71.8% simulated).

### Measured on the console

Decode figures are per step, averaged over the trace's 16 decode steps. The hit
rate is by bytes, so it is the share of the 902 MiB per token that was not read.

| experts/tensor | cache | workers | decode hit | ms/token | MiB/token | MiB/s | I/O ceiling |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | 0.88 GiB | 1 | 31.7% | 389 | 382 | 981 | 2.57 tok/s |
| 16 | 1.76 GiB | 1 | 57.6% | 389 | 382 | 981 | 2.57 tok/s |
| 16 | 1.76 GiB | 2 | 57.6% | 321 | 382 | 1,190 | 3.12 tok/s |
| 16 | 1.76 GiB | 4 | 57.6% | 344 | 382 | 1,110 | 2.91 tok/s |
| 32 | 3.52 GiB | 2 | 71.5% | 216 | 257 | 1,187 | 4.62 tok/s |
| 48 | 5.28 GiB | 2 | 76.5% | 181 | 212 | 1,170 | 5.52 tok/s |
| 64 | 7.05 GiB | 2 | 81.5% | 146 | 166 | 1,144 | 6.87 tok/s |

Two reader threads are worth about 21% (981 to 1,190 MiB/s); four and eight are
slower than two, so PS5 storage saturates near 1.2 GiB/s for these extents.
Prefill of the 12-token prompt reads 10.8 GiB in 9.1-11.3 s, because the replay
refetches per token; chunked prefill, where one fetched expert serves many rows,
is not implemented.

The 7.05 GiB allocation succeeded and the title stayed alive and responsive
through every run. Precise title RSS was not captured (klog was not streaming at
the time), so the full-system footprint with core weights and KV is still unmeasured.

### How this compares, and what is still missing

TurboFieldfare reports 5.1-6.3 tok/s on an 8 GB M2 MacBook Air. Our 6.87 tok/s at
64 experts per tensor is the same ballpark, but it is **not the same claim**: it is
an I/O ceiling for expert reads replayed from a trace, not a decode rate. Missing
for an honest comparison:

1. **The ggml scheduler integration.** Nothing streams during real inference yet.
   The pinned scheduler dereferences `input->data + expert_offset` for offloaded
   `MUL_MAT_ID` weights, so the reader has to be wired into that path before a
   token can be produced this way. The PS5 lazy mmap guard stays until then.
2. **Read/compute overlap.** `upload_expert` synchronizes around the copy.
   TurboFieldfare computes the resident shared-expert branch while its reads run;
   we do not, so the measured I/O time is fully exposed.
3. **Chunked prefill**, which would stop prefill from refetching per token.
4. **Core residency and KV**, measured together with the cache, against the
   console's real memory limit rather than the cache allocation alone.
5. **A longer, more varied trace.** One 12-token prompt and 16 decode steps is a
   thin sample for a cache-policy decision.

## Gemma 4 running on PS5 with experts on disk — 2026-10-20

The reader is now wired into inference. Three pieces make it work:

1. **A ggml buffer type that allocates nothing**
   (`inference/streaming/expert_stream.cpp`). A tensor placed in it gets address
   space at a recognizable address and no memory, so a stray dereference faults
   instead of reading something plausible.
2. **One hook in the scheduler's expert copy**
   (`tools/prepare-ggml-expert-stream.py`). The pinned scheduler already copies
   only the experts a token uses out of an offloaded MoE weight tensor; the hook
   lets the bounded cache supply those bytes. With no hook installed ggml is
   unchanged.
3. **A loader that leaves those tensors unread**
   (`tools/prepare-llama-ps5-expert-stream.py`), and llama.cpp's own
   `tensor_buft_overrides` to place `*_exps.weight` in the streaming buffer type.
   No change to llama's buffer selection was needed.

The app enables this in the `PROSPERO_GEMMA4_STREAMING=1` build; a release build
links neither the reader nor the probe. `/app0/expert_cache.txt` holds
"slots[ workers]" — resident experts per tensor, and reader threads.

### What it looks like on the console

`load_tensors: Prospero-ExpertStream model buffer size = 14429.25 MiB` next to
`Vulkan0 1573.59 MiB` and `Vulkan_Host 577.50 MiB`: the 14.4 GiB of routed
experts are never allocated. After weights, 7,101 MiB of the Vulkan heap is
still free, and a 4096-token f16 KV cache takes 660 MiB. The model reaches
"ready" in 3.5 s.

Decode, prompt "The capital of France is", 16-24 greedy tokens, two reader
threads:

| experts/tensor | expert cache | decode | prefill (6 tokens) |
| --- | --- | --- | --- |
| 16 | 1.76 GiB | 2.53 tok/s | 4.89 s |
| 32 | 3.52 GiB | 3.65 tok/s | 3.18 s |
| 48 | 5.28 GiB | 3.89 tok/s | 3.20 s |
| 64 | 7.05 GiB | 4.54 tok/s | 3.16 s |

Measured decode sits at roughly 70-80% of the replay I/O ceilings above, which
is what an implementation with no read/compute overlap should give.

### Correctness, and a defect that is not ours

Host parity (`expert-stream-test`) compares MUL_MAT_ID through the scheduler
against identical fully resident weights over six routes and matches. On the
console, the streamed run's first tokens (" capital", " of", " France", ".")
are the same as a **fully resident CPU run of the same GGUF on the host**, and
both then drift into nonsense (`" capital of France. cmath-_____"` streamed,
`" capital of France. 맺-"` resident).

So this GGUF produces poor text in the pinned llama.cpp whether or not experts
are streamed. Two known contributors, both independent of streaming:
`llama_chat_apply_template` does not know Gemma 4's template (the streaming
build falls back to the raw prompt and logs a warning), and Gemma 4 support in
the pinned llama.cpp is not verified beyond loading and decoding. Output quality
is therefore still unproven; speed and memory are not.

### Two things the integration depends on

- **`GGML_OP_OFFLOAD_MIN_BATCH=1`.** The Vulkan device offloads MUL_MAT_ID only
  from batch 32, so single-token decode would compute routed experts on the CPU,
  where the weights are not. The device reads this once, when the backend
  registry is built, so the streaming build sets it before `main` runs.
- **A device that will run MUL_MAT_ID for these experts.** Before loading, the
  app asks the device for every streamed tensor's type and shape and refuses
  streaming with a clear message otherwise. llvmpipe refuses (32 KiB of shared
  memory cannot hold the row ids and the result tile), which is why end-to-end
  verification had to happen on the console; RADV on PS5 accepts.

### Still missing against TurboFieldfare

Read/compute overlap (they compute the resident shared-expert branch while
their reads run), chunked prefill (6 tokens still cost 3.2 s because every
token refetches), and a Gemma 4 that produces sensible text at all.

## Where this stands, and how to continue

Verified: the reader, the scheduler integration and the measurements above.
Unverified: output quality (broken for reasons outside streaming), and the
GPU-side upload skip plus non-blocking copies, which pass their host test but
were never measured on the console — the console went into rest mode first.

### Reproducing the console measurements

```bash
source tools/ps5-env.sh                       # PS5_HOST, PROSPERO_TEST_TITLE
python3 tools/ps5ctl.py status                 # expect 9021 and 2120 open
PROSPERO_GEMMA4_STREAMING=1 make vulkan-runtime
python3 tools/ps5ctl.py agent kill  "$PROSPERO_TEST_TITLE"
python3 tools/ps5ctl.py shadow info "$PROSPERO_TEST_TITLE"   # mounted:false
python3 tools/ps5ctl.py ftp put build/vulkan-runtime/$PROSPERO_TEST_TITLE/eboot.bin \
    /data/homebrew/$PROSPERO_TEST_TITLE/eboot.bin
python3 tools/ps5ctl.py ftp chmod 755 /data/homebrew/$PROSPERO_TEST_TITLE/eboot.bin
printf '64 2\n' > expert_cache.txt            # experts per tensor, reader threads
python3 tools/ps5ctl.py ftp put expert_cache.txt \
    /data/homebrew/$PROSPERO_TEST_TITLE/expert_cache.txt
python3 tools/ps5ctl.py agent launch "$PROSPERO_TEST_TITLE"
curl -s -X POST "http://<PS5-IP>:11434/api/generate" -H 'content-type: application/json' \
  -d '{"model":"gemma4/gemma-4-26B-A4B-it-Q4_K_M.gguf","prompt":"The capital of France is",
       "stream":false,"options":{"num_predict":24}}'
```

`POST /api/debug/expert-stream` measures reads without loading a model; it takes
`model`, `slots`, `tokens`, `experts_per_token`, `layers`, `seed`,
`read_workers` and `trace` (a recording from `gemma4-route-trace`, uploaded
beside the models). `/app0/vulkan_verbose_logging.txt` turns on full backend
logging in the trace, which is how the scheduler's assignment was read.

### Next, in the order the numbers justify

1. **Measure the upload skip and non-blocking copies on the console** (commit
   "Stop re-uploading experts the GPU already holds"). The expectation from the
   measured decomposition — I/O 146 ms, upload and compute 74 ms per token at 64
   experts per tensor — is 5.5-6.8 tok/s against 4.54 today. `ExpertStream::Stats`
   separates `skipped`, `stalls` and `events`, so a miss tells which half failed.
2. **Chunked prefill.** Six prompt tokens still cost 3.2 s because each token
   refetches its experts; one fetch should serve a whole chunk.
3. **Fewer bytes per expert.** The file is a mix: gate/up is q4_K, down is q5_0
   or q8_0 by layer. Requantizing the routed experts alone is about 1.2x, 3-bit
   about 1.6x, 2-bit about 2x, and it shrinks the upload as well as the read.
   Quality cannot be judged until the item below is fixed, so this is blind for now.
4. **Gemma 4 text quality**, which is not a streaming problem: the pinned
   llama.cpp does not know this model's chat template, and support beyond
   loading and decoding is unverified.

### What the streaming path cannot reach

With a perfect cache (no reads at all) the per-token upload of about 900 MiB
still caps decode near 13.5 tok/s, and the measured path tops out around 5-7.
Numbers in the 30s need the model resident, which means fitting under about
11 GB of the 12.28 GB Vulkan heap: Q4_K_M (16.8 GB) and Q3_K_M (12.6 GB) do not,
a 2-bit quantization would. That is a different experiment, not an optimization
of this one.
