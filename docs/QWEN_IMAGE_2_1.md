# Qwen-Image 2.1 on PS5

The image runtime supports Qwen-Image 2.1 text-to-image with these quantized weights. Q2 is
chosen for memory headroom on PS5; it trades image quality for a lower peak. Create
`/download0/ProsperoAI/models/qwen-image-2.1/`, keep the official `model_index.json` from
`Qwen/Qwen-Image-2.1`, and place these files in that directory:

| File | Source |
| --- | --- |
| `qwen_image_2.1-Q2_K.gguf` | [leejet/Qwen-Image-2.1-GGUF](https://huggingface.co/leejet/Qwen-Image-2.1-GGUF) |
| `Qwen3VL-8B-Instruct-Q4_K_M.gguf` | [Qwen/Qwen3-VL-8B-Instruct-GGUF](https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-GGUF) |
| `qwen_image_2.1_vae_bf16.safetensors` | [Comfy-Org/Qwen-Image-2.1](https://huggingface.co/Comfy-Org/Qwen-Image-2.1/tree/main/vae) |

Use **Refresh installed models** in Models after copying the files. The app uses 512×512,
40-step generation by default for this model, limits managed Vulkan memory to 10 GiB and
stores the prompt prefix cache as Q8 to reduce peak memory. Higher resolutions need more
working memory and are not validated on the PS5 yet.

The current UI exposes text-to-image. Image-conditioned editing needs an image-upload path
through the API and is not included in this integration yet.

The model weights use the [Qwen Research License](https://huggingface.co/Qwen/Qwen-Image-2.1/blob/main/LICENSE).
This project does not bundle or redistribute them; review the license before use or distribution.
