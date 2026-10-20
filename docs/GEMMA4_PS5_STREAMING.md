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
