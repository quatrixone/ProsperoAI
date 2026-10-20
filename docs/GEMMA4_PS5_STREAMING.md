# Gemma 4 26B-A4B bounded-memory inference on PS5

The goal is to run Gemma 4 26B-A4B through ProsperoAI's pinned llama.cpp and
Vulkan runtime with about 2 GB of measured model memory. This is a target, not
a result. Model files still need roughly 15 GB of storage, and total process
memory also includes KV, activations, Vulkan staging and runtime allocations.

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
2. **S1: prove lazy expert residency first.** The pinned llama.cpp already has
   mmap-backed lazy tensors. Mark Gemma 4 routed expert matrices lazy and run
   them on the CPU backend, while the other tensors stay on Vulkan. This keeps
   full expert matrices out of eager backend allocations and gives us a first
   PS5 memory/correctness measurement. This is an opt-in experiment; it does not
   yet provide a bounded cache or guarantee a physical-memory ceiling.
   Build the test title with `PROSPERO_GEMMA4_STREAMING=1`; this opts that
   build into Gemma 4 lazy mmap mode. The selected model name must include
   `gemma4` or `gemma-4`, and the loader fails if mmap is unavailable.
3. **S2: add an explicit bounded expert cache if S1 needs it.** Keep embeddings,
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
   token and decode speed. A 2 GB claim requires total measured inference memory,
   not only the expert cache or Vulkan allocation. Do not advertise the target
   until these measurements pass.

This PR adds S1 as an opt-in PS5 experiment and keeps S0's GGUF budget report.
It does not yet add the bounded expert cache or claim Gemma 4 runs in 2 GB.

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
Gemma's memory use safe, and S1 remains unverified. Do not retry Gemma loading until the
console has restarted and the guarded build is installed on the test title.
