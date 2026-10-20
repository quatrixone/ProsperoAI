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
2. **S1: build a bounded expert-I/O prototype.** Keep embeddings, attention,
   routers, shared experts and KV resident. Read only routed expert extents into
   a fixed number of aligned Vulkan-visible slots. Do not allocate full expert
   tensors. Add slot lifetime tracking so a Vulkan submission cannot read a slot
   while it is being refilled. Keep the resident llama.cpp path as the reference.
3. **S2: prove host correctness.** Compare logits and greedy tokens against the
   resident path for short prompts, repeated experts, cache eviction, failures,
   cancellation and model/context teardown. Validate all GGUF quantization
   layouts used by the selected Gemma 4 file.
4. **S3: measure on a PS5 test title.** Report physical memory by category,
   uncached and warm read bandwidth, expert-cache hit rate, prefill, time to first
   token and decode speed. A 2 GB claim requires total measured inference memory,
   not only the expert cache or Vulkan allocation. Do not advertise the target
   until these measurements pass.

The first PR only adds S0 expert extent accounting. It does not change model
loading or inference and does not claim Gemma 4 runs in 2 GB.
