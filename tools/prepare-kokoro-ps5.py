#!/usr/bin/env python3
"""Apply guarded PS5 scheduler sizing to the pinned Kokoro adapter source."""
from pathlib import Path
from prepare_build import write_text_if_changed
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / '.deps/kokoro.cpp')
source = parser.parse_args().source / 'src/kokoro.cpp'
text = source.read_text()
before_comment = '''        // 256k nodes — predictor has 4 bidir LSTMs at T_frames up to ~1500
        // (~54k LSTM nodes) plus the decoder graph; the upper bound is
        // generous to accommodate AdainResBlk1d + AdaIN expansion.'''
after_comment = '''        // 128k nodes — sufficient for the predictor's four bidirectional LSTMs
        // (about 54k nodes at T_frames=1500) plus decoder and AdaIN expansion.'''
if after_comment not in text:
    if text.count(before_comment) != 1:
        raise SystemExit(f'{source}: expected one pinned Kokoro scheduler sizing comment')
    text = text.replace(before_comment, after_comment)

before = '/*graph_size=*/262144'
after = '/*graph_size=*/131072'
if after not in text:
    if text.count(before) != 1:
        raise SystemExit(f'{source}: expected one pinned Kokoro scheduler graph-size setting')
    text = text.replace(before, after)

before_meta = 'ggml_tensor_overhead() * 262144 + ggml_graph_overhead_custom(262144, false)'
after_meta = 'ggml_tensor_overhead() * 131072 + ggml_graph_overhead_custom(131072, false)'
if after_meta not in text:
    if text.count(before_meta) != 1:
        raise SystemExit(f'{source}: expected one pinned Kokoro compute-metadata sizing expression')
    text = text.replace(before_meta, after_meta)

write_text_if_changed(source, text)
