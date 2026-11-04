tiny-torch-state.pkl is a generated protocol-2 torch state-dict metadata fixture.
It describes one 2x2 float32 weight in storage 0 (16 bytes); it contains no actual weights
or executable test dependency. It was produced with Python pickle persistent_id and
_rebuild_tensor_v2 reduction, using placeholder torch globals without importing PyTorch.
The SD parser reads the fixture as metadata and never unpickles or executes its globals.

# MiniCPM5 template fixture

`minicpm5-chat.jinja` is the embedded `tokenizer.chat_template` extracted from
OpenBMB's MiniCPM5-2B Q8_0 GGUF, revision
`2079a22f3beaa4e306449978533478fe0522f4b3`, retrieved 2026-10-10:
https://huggingface.co/openbmb/MiniCPM5-2B-GGUF/blob/2079a22f3beaa4e306449978533478fe0522f4b3/MiniCPM5-2B-Q8_0.gguf

OpenBMB publishes the model under Apache-2.0. The Apache license text is in
[LICENSE](../../third_party/minicpm5/LICENSE). Only trailing whitespace was removed from the fixture.
Tests render it with Jinja2 3.1.6 and compare the native text-chat formatter.

# Image codec fixtures

`one-pixel.png`, `one-pixel.jpg`, `gray-alpha.png`, `wide-2049.png`, and
`bmp-as-png.png` are small generated fixtures; the tests consume the committed
bytes and do not require Pillow. `gray-alpha.png` is a 1×1 grayscale-plus-alpha
PNG. `truncated.png` is a truncated copy of the valid PNG, and `zero-width.png`
has a zero-width IHDR with its CRC recomputed.

`minicpm5-chat-cases.json` records eight outputs rendered by that template with
Jinja2 3.1.6. The ordinary test suite checks these cases without a Jinja dependency.
