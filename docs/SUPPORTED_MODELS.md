# Supported models

The Models screen can download each curated preset below. These presets use the runtimes and
adapters already included in ProsperoAI; adding one does not require a model-specific code
change. Presets are pinned to revisions and checked against their expected file sizes and
SHA-256 hashes during download.

| Model | Type | Download | PS5 status |
| --- | --- | --- | --- |
| [Mistral 7B Instruct v0.3 Q4_0](https://huggingface.co/QuantFactory/Mistral-7B-Instruct-v0.3-GGUF) | Text, GGUF | Curated preset | Verified; three correct benchmark runs |
| [Qwen3.8 9B Distill Q4_K_M](https://huggingface.co/empero-ai/Qwen3.8-9B-Distill-GGUF) | Text, GGUF | Curated preset | Verified; three correct benchmark runs |
| [MiniCPM5 2B Q8_0](https://huggingface.co/openbmb/MiniCPM5-2B-GGUF) | Text, GGUF | Curated preset | Verified, including generation at the 32K context boundary |
| [Ternary Bonsai 2 27B PTQ1_0](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf) | Text, GGUF | Curated preset | Verified; three correct benchmark runs |
| [Granite 4.0 H-Tiny Q4_K_M](https://huggingface.co/ibm-granite/granite-4.0-h-tiny-GGUF) | Text, GGUF | Curated preset | Verified; three correct benchmark runs |
| [BoHF 12B MoE 3A Q3_K_M](https://huggingface.co/theprint/Bohf-12B-MoE-3A-GGUF) | Text, GGUF | Curated preset | Verified; experimental model |
| [SD-Turbo FP16](https://huggingface.co/blackbearreloaded/ProsperoAI-SD-Turbo-FP16-PS5) | Image | Curated preset | Verified; three valid 512 × 512 images |
| [Realistic Vision v5.1 FP16](https://huggingface.co/SG161222/Realistic_Vision_V5.1_noVAE) | Image | Curated preset | Verified; three valid 512 × 512 images |
| [Stable Audio 3 Small SFX Q8_0](https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/main/Stable-Audio-3-Small-SFX-GGUF) | Sound effects | Curated preset | Verified; three valid stereo WAV generations |
| [ACE-Step 1.5 Turbo Q8_0](https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/main/ACE-Step1.5-GGUF/turbo) | Music | Curated preset | Verified; three valid stereo WAV generations |
| [Kokoro 82M Vulkan](https://huggingface.co/blackbearreloaded/ProsperoAI-Kokoro-82M-FP16-PS5) | Speech | Curated preset | Verified; three valid WAV generations |
| [Chatterbox 500M Q8_0](https://huggingface.co/audio-cpp/audio.cpp-gguf) | Speech, voice cloning | Curated preset | Verified; three valid WAV generations; 19 selectable languages |

The table is the complete list of curated presets in this build. Curated media presets include
their required model files and use the image, audio, or speech runtime already in the app.

Music and sound generation uses a default clip length (8 s for ACE-Step, 4 s for Stable Audio 3).
For supported audio generation models, choose a duration and seed in the native Generation menu
or the web workspace's Generation settings. The UI offers model default, 8, 16, 30, 60, 120 and
300 seconds; the web UI and API accept audio seeds from 0 to 4294967295. The
`audio_duration_sec` runtime setting also accepts 1-300 through `POST /api/settings/runtime` or
the settings file (`0` restores per-model defaults); audio.cpp's planner may clamp a request the
loaded model cannot serve. The API's `audio_seed` default is 42.
Vocal clarity has not been validated for every language. Romanian and other non-English lyrics
may sound unclear; changing the duration or seed does not guarantee pronunciation or select a
specific lyric section.

Image generation uses the `sd_seed` runtime setting. Its default is 42; `0` requests a random
seed. Set it through `POST /api/settings/runtime` or the web workspace. The native Generation
menu can retain the current seed or choose a common seed value. Image size can use the model's
default or one of the available sizes in the UI. The API accepts paired `sd_width` and `sd_height`
values that are multiples of 64 from 256 through 768; larger sizes need more memory. Model
defaults are 512×512 for most models and 768×768 for SD3 and FLUX.2 Klein.
Optionally enable Real-ESRGAN 4× post-processing in the native Generation menu or web settings.
Place `RealESRGAN_x4plus.pth` in `models/upscalers/` under the configured model root; see the
[API reference](API.md) for the model download and API setting. Upscaling is off by default and
runs after releasing the diffusion model from GPU memory.

## Other GGUF text models

ProsperoAI can also download and discover standalone `.gguf` text models. The pinned
[llama.cpp revision](https://github.com/ggml-org/llama.cpp/tree/cb44fdb1892eebca072e0949875a60c11245a8bb)
includes loaders for the Llama, Qwen, Gemma, Phi, DeepSeek, Granite and other architectures,
plus chat-template handling. This means many GGUF models from those families can be tried
without adding model-specific app code. In Models, search for the model name or repository,
open its GGUF files, and download one file. The app will discover it in the installed-model
list. Examples include:

| Model example | GGUF repository | Starting quantization |
| --- | --- | --- |
| Llama 3.1 8B Instruct | [bartowski/Meta-Llama-3.1-8B-Instruct-GGUF](https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF) | Q4_K_M |
| Qwen3 8B | [Qwen/Qwen3-8B-GGUF](https://huggingface.co/Qwen/Qwen3-8B-GGUF) | Q4_K_M |
| Gemma 3 4B IT | [ggml-org/gemma-3-4b-it-GGUF](https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF) | Q4_K_M; text input only |
| Phi-4 Mini Instruct | [bartowski/microsoft_Phi-4-mini-instruct-GGUF](https://huggingface.co/bartowski/microsoft_Phi-4-mini-instruct-GGUF) | Q4_K_M |
| DeepSeek R1 Distill Qwen 7B | [bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF](https://huggingface.co/bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF) | Q4_K_M |

These are compatibility candidates, not ProsperoAI-curated or PS5-verified presets. Start with a
single-file Q4 GGUF and a moderate context; the model's architecture, tokenizer and chat template
must be supported, its tensor types must have Vulkan kernels, and its weights plus context cache
must fit the console's available memory. Very large models, multimodal GGUFs, embedding models,
unsupported templates or quantizations can fail even when llama.cpp recognizes the architecture.
The list is representative rather than exhaustive; many fine-tunes of a supported family may
also work. See the pinned [architecture list](https://github.com/ggml-org/llama.cpp/blob/cb44fdb1892eebca072e0949875a60c11245a8bb/src/llama-arch.cpp)
and [chat-template list](https://github.com/ggml-org/llama.cpp/blob/cb44fdb1892eebca072e0949875a60c11245a8bb/src/llama-chat.cpp).

The curated presets were each exercised for three generation rounds on the test title; their
results are in [the retained 3× test record](performance/2026-10-14/pr27-all-models-3x-final.json).
That validation does not extend to the additional GGUF examples above.

Image generation currently writes both PNG and TGA: PNG is the shareable/downloadable output,
while the native conversation preview and archived image use TGA. Both formats are needed by
the current app flow.
