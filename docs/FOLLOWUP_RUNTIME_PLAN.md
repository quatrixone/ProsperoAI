# Follow-up: capacity and performance after vulkan-runtime

Depends on ProsperoAI PR #15. This draft is deliberately separate from the verified
single-runtime migration. It implements the S0 indexing/probe tools and carries the
remaining plan; it does not implement streamed inference.

## S0 evidence and decision

Exact Bonsai 2 PTQ1_0 has 5.379 GB of layer weights; a packed largest-layer pair needs
169.822 MB before alignment, upload, KV and activation allocations. Console disk
probes measured 1.384/1.778 GB/s serial/four workers with uncontrolled cache and no GPU
upload. A full-layer disk-only bound is approximately 0.33 tokens/s. Resident Vulkan
inference remains the speed and output reference; bounded streaming is a capacity
feature, not an assumed throughput improvement. See [research](AIRLLM_PS5_RESEARCH.md).
The detailed historical streaming probe files are retained under `docs/performance/2026-10-11/`.

## S1: native bounded residency

1. Design quantized layer slots with a strict byte budget and aligned buffers. Retain
   architecture-specific non-layer tensors, recurrent state and KV explicitly.
2. Add asynchronous reads/prefetch and Vulkan upload fences. Never recycle a layer
   slot until its GPU readers finish; propagate read/upload failures safely.
3. Integrate one dense model first and compare greedy output with resident inference.
   Exercise cancellation, errors, context recreation and model/media switching.
4. Measure actual physical-memory reduction, disk/cache conditions, GPU upload time,
   initial load and decode latency. Gate wider model support on useful measured capacity.

## Media and voice optimization

Profile Kokoro CPU convolution/LSTM/waveform work, per-operation Vulkan allocations,
uploads and readbacks. Compare fixed fixtures against PR #15; preserve correct output.
The earlier AGC comparison shows SD/Audio speedups but voice approximately 0.5% slower.
Measure peak physical memory, thermals and longer repeated text/image/audio/voice
switching. Do not promise real-time audio synthesis from the current approximately
124-second warm generation of 11.889 seconds of audio.

## Subsequent gates

Expert caching requires an actual MoE routing/coverage experiment; dense Bonsai is not
that proof. Speculative decoding requires supported upstream execution and separate
output/latency validation. Larger context and longer output limits require console
memory/long-generation evidence, rather than extrapolating the verified 4K/16K checks.
Resolve source-backed Pocket TTS support separately if added as an active feature.

All deployments use an authorized test title and keep PPSA99004 untouched. Each
milestone needs host tests, valid outputs on console and published measured results.
