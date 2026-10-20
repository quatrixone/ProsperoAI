#!/usr/bin/env python3
"""Index GGUF weight extents without loading weights or requiring NumPy.

S0 planning only: layer groups are not a streaming execution graph. KV, activations,
Vulkan upload buffers and allocator overhead must be measured separately.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import json
import math
from pathlib import Path
import re
import struct

# Block elements/packed bytes from our pinned ggml/gguf quantization ABI.
QUANTS = {0: (1, 4), 1: (1, 2), 2: (32, 18), 3: (32, 20),
          6: (32, 22), 7: (32, 24), 8: (32, 34), 9: (32, 36),
          10: (256, 84), 11: (256, 110), 12: (256, 144),
          13: (256, 176), 14: (256, 210), 15: (256, 292),
          16: (256, 66), 17: (256, 74), 18: (256, 98),
          19: (256, 50), 20: (32, 18), 21: (256, 110),
          22: (256, 82), 23: (256, 136), 24: (1, 1),
          25: (1, 2), 26: (1, 4), 27: (1, 8), 28: (1, 8),
          29: (256, 56), 30: (1, 2), 34: (256, 54),
          35: (256, 66), 39: (32, 17), 40: (64, 36),
          41: (128, 18), 42: (64, 18),
          # Prism release e8fd209: private formats, not stock ggml type aliases.
          142: (128, 34), 143: (128, 28)}
SCALARS = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i',
           6: 'f', 7: '?', 10: 'Q', 11: 'q', 12: 'd'}


class Reader:
    def __init__(self, file, size):
        self.file, self.size = file, size

    def unpack(self, fmt):
        length = struct.calcsize('<' + fmt)
        data = self.file.read(length)
        if len(data) != length:
            raise ValueError('truncated GGUF metadata')
        return struct.unpack('<' + fmt, data)[0]

    def skip(self, length):
        if length < 0 or self.file.tell() + length > self.size:
            raise ValueError('metadata extends beyond file')
        self.file.seek(length, 1)

    def string(self, keep=False):
        length = self.unpack('Q')
        if not keep:
            self.skip(length)
            return None
        if length > 65536:
            raise ValueError('oversized metadata name/value')
        data = self.file.read(length)
        if len(data) != length:
            raise ValueError('truncated GGUF string')
        return data.decode('utf-8')

    def value(self, kind, keep=False):
        if kind in SCALARS:
            return self.unpack(SCALARS[kind])
        if kind == 8:
            return self.string(keep)
        if kind == 9:
            subkind, count = self.unpack('I'), self.unpack('Q')
            if subkind in SCALARS:
                self.skip(count * struct.calcsize('<' + SCALARS[subkind]))
            elif subkind == 8:
                if count > self.size // 8:
                    raise ValueError('impossible string array count')
                for _ in range(count):
                    self.string()
            else:
                raise ValueError('unsupported GGUF array type')
            return None
        raise ValueError('unknown GGUF metadata type')


def inspect(path):
    path = Path(path)
    size = path.stat().st_size
    with path.open('rb') as file:
        r = Reader(file, size)
        if file.read(4) != b'GGUF' or r.unpack('I') not in (2, 3):
            raise ValueError('expected little-endian GGUF v2/v3')
        count, fields = r.unpack('Q'), r.unpack('Q')
        if count > size // 24 or fields > size // 12:
            raise ValueError('impossible metadata counts')
        metadata = {}
        for _ in range(fields):
            key = r.string(True)
            keep = (key in ('general.architecture', 'general.alignment')
                    or key.endswith('.expert_count')
                    or key.endswith('.expert_used_count'))
            value = r.value(r.unpack('I'), keep)
            if keep:
                metadata[key] = value
        alignment = metadata.get('general.alignment', 32)
        if (not isinstance(alignment, int) or alignment < 1 or alignment > 65536
                or alignment & (alignment - 1)):
            raise ValueError('invalid GGUF alignment')
        tensors, names = [], set()
        for _ in range(count):
            name, rank = r.string(True), r.unpack('I')
            if not 1 <= rank <= 4 or name in names:
                raise ValueError('invalid tensor rank or duplicate name')
            names.add(name)
            dims = [r.unpack('Q') for _ in range(rank)]
            kind, offset = r.unpack('I'), r.unpack('Q')
            if kind not in QUANTS:
                raise ValueError(f'unsupported tensor quantization type {kind}')
            block, packed = QUANTS[kind]
            if any(d == 0 for d in dims) or dims[0] % block or offset % alignment:
                raise ValueError('invalid quantized row or tensor alignment')
            length = math.prod(dims) // block * packed
            match = re.match(r'^blk\.(\d+)\.', name)
            tensors.append({'name': name, 'dimensions': dims, 'type': kind,
                            'offset': offset, 'bytes': length,
                            'layer': int(match[1]) if match else None})
        data_start = (file.tell() + alignment - 1) // alignment * alignment
    end = data_start
    for tensor in sorted(tensors, key=lambda t: t['offset']):
        tensor['offset'] += data_start
        if tensor['offset'] < end or tensor['offset'] + tensor['bytes'] > size:
            raise ValueError('overlapping or truncated tensor data')
        end = tensor['offset'] + tensor['bytes']
    layers = {}
    routed_layers = {}
    resident = 0
    for tensor in tensors:
        layer = tensor['layer']
        # GGUF stores MoE expert matrices as rank-3 tensors with experts in
        # the final dimension (for example blk.0.ffn_gate_exps.weight).
        # Keep the estimate based on actual quantized extents, not parameter
        # counts or a presumed four-bit representation.
        is_routed_expert = (layer is not None and tensor['name'].startswith('blk.')
                            and '.ffn_' in tensor['name']
                            and '_exps' in tensor['name']
                            and len(tensor['dimensions']) == 3)
        if is_routed_expert:
            expert_count = tensor['dimensions'][-1]
            if tensor['bytes'] % expert_count:
                raise ValueError(f"routed expert tensor has uneven expert extent: {tensor['name']}")
            group = routed_layers.setdefault(layer, {
                'layer': layer, 'experts': expert_count,
                'expert_weight_bytes': 0, 'tensors': 0})
            if group['experts'] != expert_count:
                raise ValueError(f'inconsistent expert count in layer {layer}')
            group['expert_weight_bytes'] += tensor['bytes']
            group['tensors'] += 1
        if layer is None:
            resident += tensor['bytes']
        else:
            group = layers.setdefault(layer, {'layer': layer, 'bytes': 0,
                                      'start': size, 'end': 0, 'tensors': 0})
            group['bytes'] += tensor['bytes']
            group['start'] = min(group['start'], tensor['offset'])
            group['end'] = max(group['end'], tensor['offset'] + tensor['bytes'])
            group['tensors'] += 1
    groups = [dict(v, span_bytes=v['end'] - v['start']) for _, v in sorted(layers.items())]
    largest_span = max((v['span_bytes'] for v in groups), default=0)
    largest_payload = max((v['bytes'] for v in groups), default=0)
    routed = [dict(v, bytes_per_expert=v['expert_weight_bytes'] // v['experts'])
              for _, v in sorted(routed_layers.items())]
    expert_count_metadata = metadata.get(f"{metadata.get('general.architecture', '')}.expert_count")
    expert_used_count = metadata.get(f"{metadata.get('general.architecture', '')}.expert_used_count")
    if routed and isinstance(expert_count_metadata, int):
        if any(layer['experts'] != expert_count_metadata for layer in routed):
            raise ValueError('GGUF expert_count metadata does not match routed tensor dimensions')
    active_expert_bytes = None
    if routed and isinstance(expert_used_count, int):
        if any(expert_used_count < 1 or expert_used_count > layer['experts'] for layer in routed):
            raise ValueError('GGUF expert_used_count is outside routed tensor dimensions')
        active_expert_bytes = sum(layer['bytes_per_expert'] * expert_used_count
                                  for layer in routed)
    return {'model': path.name, 'file_bytes': size, 'architecture': metadata.get('general.architecture'),
            'data_start': data_start, 'weight_bytes': sum(t['bytes'] for t in tensors),
            'non_layer_weight_bytes': resident, 'layer_weight_bytes': sum(v['bytes'] for v in groups),
            'two_layer_packed_slots_bytes': largest_payload * 2,
            'two_layer_span_slots_bytes': largest_span * 2,
            'expert_count': expert_count_metadata,
            'expert_used_count': expert_used_count,
            'routed_expert_bytes_per_token_without_cache': active_expert_bytes,
            'routed_expert_layers': routed,
            'requires_prism_runtime': any(t['type'] in (142, 143) for t in tensors),
            'qualification': ('Weight extents only. Routed bytes/token assumes the same '
                              'expert_used_count in every MoE layer and no cache hits; '
                              'it excludes KV, activations, Vulkan upload and cache residency.'),
            'layers': groups, 'tensors': tensors}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.model)
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(1, f'{error}\n')
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')
