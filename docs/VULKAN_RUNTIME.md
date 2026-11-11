# vulkan-runtime

The PS5 runtime pins upstream llama.cpp `cb44fdb1892eebca072e0949875a60c11245a8bb`
and one shared upstream GGML `cd6d2b75f5d3e4b2f3fe8e8fbc6c8b84ac80e74a` ABI
(`GGML_MAX_NAME=160`). Text, source-built Stable Diffusion, audio.cpp and Kokoro link to the
same upstream GGML/Vulkan target through model adapters. The production app keeps the RADV
link recipe and console AGC platform-driver imports; AGC inference is removed. Prism's private
PTQ1_0/PQ2_0 quant types are not part of upstream GGML and need a compatibility adapter before
the Bonsai preset can be enabled on this runtime.

## Build and compatibility

`make vulkan-runtime` produces `build/vulkan-runtime/<TEST-TITLE-ID>`; `make app` packages
the configured release title. Neither command deploys. `tools/build-vulkan-runtime.sh` builds the libraries and
`tools/build-vulkan-release.sh` packages the app. The CMake project, CI job and test
binaries use the same `vulkan-runtime` name. Historical make targets remain aliases. All previous app Vulkan/Bonsai
folder aliases now use this runtime. The pinned Bonsai PTQ1_0 preset is included.

Standard GGUF models use the upstream llama.cpp adapter. A legacy preset ID can alias its installed GGUF;
the router forwards the actual GGUF ID to llama. Old `.ps5lm` data is preserved but is
not executed: download the matching GGUF preset before using that legacy ID. Console
regressions cover Mistral, Qwen 3.5, MiniCPM5 and exact Bonsai 2 27B.

## Evidence and limits

The common-ABI console Bonsai benchmark generates 94 identical greedy tokens on three
warm runs: **11.202328 tokens/s median**. Thinking, 4K/16K context changes, older GGUFs
and return to Bonsai pass. The retained PS5 summary is the [final three-round model record](performance/2026-10-14/pr27-all-models-3x-final.json).

The [AGC/media comparison](MEDIA_VULKAN_COMPARISON.md) records earlier comparison
builds: SD repeat median 46.048 → 1.001 seconds; Stable Audio 368.103 → 124.106;
Kokoro 84.740 → 85.152. This is a complete-implementation comparison, not an isolated
GPU-driver test. Kokoro is approximately 0.5% slower. The detailed historical comparison
records were removed from this PR; the retained current model summary is linked above.

Host common-ABI tests perform actual Vulkan llama decode and media numerical checks.
The bridge parity fixture makes 30 Vulkan calls and zero CPU-kernel fallback calls.
ASan/UBSan real-model prefix checks verify cached/cold equivalence and error recovery.
The signed ELF has entry 0x120, 14 program headers and the working 15-library NEEDED
list; its symbol table contains one `ggml_init` and no AGC inference/private ggml symbols.

Peak memory, thermals, extended switching, voice optimization and AirLLM/Strata S1
are follow-up work. No streaming, universal perceptual parity or real-time audio claim.
