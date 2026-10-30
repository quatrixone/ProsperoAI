# ProsperoAI HTTP API

ProsperoAI serves its API from the running PS5 title at `http://<PS5-IP>:11434/`.
Replace `<PS5-IP>` with the console's local address. The server uses plain HTTP; use it
only on a trusted network.

## Authentication

By default, requests do not need an API key or browser token. To require a key, place
`api_key.txt` in the deployed app folder and restart the title. External clients send
`Authorization: Bearer <key>` with every request. The first-party web UI asks for the key,
exchanges it through `POST /api/auth/session`, then keeps only the returned 30-minute,
host- and origin-bound session token in tab-scoped `sessionStorage`. The key itself is not
retained by the page. The listener uses plain HTTP, so use this only on a trusted local network.

## OpenAI-compatible API

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/v1/models` | List installed models and their IDs. |
| `POST` | `/v1/chat/completions` | Generate text, optionally streaming Server-Sent Events; supports function/tool calls. |
| `POST` | `/api/auth/session` | Exchange the configured bearer key for a 30-minute same-origin browser session. |
| `GET` | `/api/settings/security` | Read the API authentication mode, HTTP transport, and listener exposure. This status contains no credential and does not require authentication. |

Use a model ID returned by `/v1/models`. See [OpenCode setup and API limits](OPENCODE.md)
for request limits, tool-call behavior, and a client configuration example.

## Runtime settings

`GET /api/settings/runtime` returns the shared runtime settings. `POST` accepts a JSON
object containing one or more settings; omitted settings retain their current values.
Both methods return the complete settings object after the request.

```bash
curl -X POST 'http://<PS5-IP>:11434/api/settings/runtime' \
  -H 'Content-Type: application/json' \
  -d '{"audio_duration_sec":30,"audio_seed":123,"sd_seed":456,"sd_width":512,"sd_height":768}'
```

If authentication is enabled, add `-H 'Authorization: Bearer <key>'`.

| Property | Type and valid values | Effect |
| --- | --- | --- |
| `context` | String: `auto`, `4k`, `8k`, `16k`, `32k`, `64k`, `128k`, `256k` | Shared text model context setting. |
| `output_limit` | Integer: a supported output token limit, including `0` for automatic | Shared text generation limit. |
| `kv` | String: `auto` | KV cache precision is selected automatically. |
| `thinking` | Boolean | Enables thinking for supported text models. |
| `idle_unload_minutes` | Integer: `1`, `2`, `5`, `10`, `30`, or `60` | Unloads an idle text model after this many minutes. |
| `audio_duration_sec` | Integer: `0` to `300`; `0` means each model's default | Requested clip duration for ACE-Step and Stable Audio. The audio planner may clamp it. |
| `audio_seed` | Unsigned 32-bit integer | Seed used by ACE-Step and Stable Audio. Default is `42`. |
| `sd_seed` | Unsigned 32-bit integer; `0` requests a random seed | Seed used by image generation models, including Stable Diffusion. Default is `42`. |
| `sd_width`, `sd_height` | Both `0`, or multiples of `64` from `256` through `768` | Image dimensions for Stable Diffusion based models. Both `0` use the model's default size. Set both in the same request. |
| `sd_upscale` | Boolean | Runs optional Real-ESRGAN 4× post-processing. Requires `models/upscalers/RealESRGAN_x4plus.pth`. |
| `kokoro_voice` | Voice ID string | Default Kokoro voice. |
| `chatterbox_voice` | Voice ID string | Default Chatterbox voice. |
| `chatterbox_language` | Language code string | Default Chatterbox language. |

Audio controls apply to audio-generation models. Image seed applies to image-generation
models. The web workspace shows each model-specific setting when a matching model is
selected. The native Generation menu offers the current seed and common seed choices;
the web UI and API accept any unsigned 32-bit seed.

For Real-ESRGAN, create `models/upscalers/` under the configured model root and place
[`RealESRGAN_x4plus.pth`](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth)
there. Upscaling is off by default. It releases the diffusion model from GPU memory before
loading the upscaler, and processes the image in tiles. If upscaling fails, the original
generated image is saved.

Invalid settings return `400 Bad Request`; a settings-file write failure returns
`500 Internal Server Error`. Settings persist in `prosperoai.cfg` across app restarts.

## ProsperoAI and Ollama-compatible routes

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/tags` | List installed models in Ollama-style format. |
| `POST` | `/api/generate` | Generate from a prompt in Ollama-style format. |
| `POST` | `/api/chat` | Generate from chat messages in Ollama-style format. |
| `GET` | `/api/models/memory` | Report the loaded model and a live snapshot of available CPU/direct and Vulkan memory. A snapshot is not a guarantee that a model will fit. |
| `POST` | `/api/models/estimate` | Return successful historical memory samples for a model and a best-effort current snapshot. Historical readings are not a fit guarantee; no sample returns `estimate_available:false`. Body: `{"id":"model-id"}`. |
| `GET`, `POST` | `/api/models/selection` | Read or change the selected model. |
| `GET`, `POST` | `/api/models/presets` | List curated model presets with required storage; start a download. The current PS5 build reports free space as unknown and does not claim to preflight it. |
| `POST` | `/api/models/search` | Search model repositories by query. |
| `POST` | `/api/models/browse` | Browse downloadable files in a model repository. |
| `GET`, `POST`, `DELETE` | `/api/models/download` | Check, start, or cancel a model download. |
| `DELETE` | `/api/models` | Delete an installed model. |
| `GET`, `DELETE` | `/api/media` | List image/audio files referenced by saved conversations, with source conversation and model metadata; delete an archived conversation-owned copy by sending `{"path":"/download0/ProsperoAI/work/image/<id>/<file>"}` (audio and voice folders are also supported). Shared references and files outside that conversation's output folder are rejected, and the message reference is replaced with `[media deleted]`. |
| `GET`, `POST` | `/api/conversations` | List or create/update a saved conversation. |
| `GET`, `PATCH`, `DELETE` | `/api/conversations/{id}` | Read, rename (`{"title":"..."}`), pin/archive (`{"pinned":true,"archived":false}`), or remove one saved conversation. |
| `GET` | `/api/diagnostics/events` | Export the bounded privacy-aware lifecycle and model event journal. |
| `GET`, `POST` | `/api/voices/chatterbox/custom` | Read or upload the custom Chatterbox voice sample. |
| `GET`, `POST`, `DELETE` | `/api/voices/chatterbox/recording` | Read or manage a Chatterbox recording session. |

Conversation responses include boolean `pinned` and `archived` fields. Existing saved conversations default both to `false`. The model library and conversation routes use JSON. Model download operations can return
`409 Conflict` when another download is active or the request cannot be started. The web
workspace calls these routes directly.

Diagnostic journal rows include a unique `operation_id`, UTC timestamp, operation name, model ID
when available, and a fixed error code. Prompts, generated text, credentials, and runtime error
strings are not stored.

Repository browse results include each GGUF file's repository size and a rough file-size fit
badge. That badge is only a heuristic: GGUF size does not include context/KV
cache or all runtime allocations. `/api/models/estimate` reports the installed file's size,
when available, separately from historical runtime measurements.
