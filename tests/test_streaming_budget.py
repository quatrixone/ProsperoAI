"""GGUF extent validation and bounded native disk probe regression checks."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stream_budget', ROOT / 'tools/gguf-stream-budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def string(text):
    data = text.encode()
    return struct.pack('<Q', len(data)) + data


def fixture(path, kind=2, offset=32, dimension=32):
    data = b'GGUF' + struct.pack('<IQQ', 3, 3, 2)
    data += string('general.architecture') + struct.pack('<I', 8) + string('llama')
    data += string('tokenizer.ggml.tokens') + struct.pack('<IIQ', 9, 8, 2)
    data += string('first') + string('second')
    for name, tensor_kind, location in [('token_embd.weight', 1, 0),
                                      ('blk.0.ffn.weight', kind, offset),
                                      ('blk.1.ffn.weight', 2, 64)]:
        data += string(name) + struct.pack('<IQIQ', 1, dimension if tensor_kind != 1 else 8,
                                         tensor_kind, location)
    data += bytes((-len(data)) % 32)
    data += bytes(64 + dimension // 32 * 18)
    path.write_bytes(data)


def routed_fixture(path):
    # Each expert matrix has 32x64 Q4_0 values per expert, with four
    # experts packed in dimension 2. Three tensors make up gate/up/down.
    entries = [
        ('general.architecture', 8, string('gemma4')),
        ('gemma4.expert_count', 4, struct.pack('<I', 4)),
        ('gemma4.expert_used_count', 4, struct.pack('<I', 2)),
    ]
    data = b'GGUF' + struct.pack('<IQQ', 3, 3, len(entries))
    for key, kind, value in entries:
        data += string(key) + struct.pack('<I', kind) + value
    tensor_bytes = 32 * 64 * 4 // 32 * 18
    tensors = [('blk.0.ffn_gate_exps.weight', 0),
               ('blk.0.ffn_up_exps.weight', tensor_bytes),
               ('blk.0.ffn_down_exps.weight', tensor_bytes * 2)]
    for name, offset in tensors:
        data += string(name) + struct.pack('<IQQQI', 3, 32, 64, 4, 2)
        data += struct.pack('<Q', offset)
    data += bytes((-len(data)) % 32)
    data += bytes(tensor_bytes * len(tensors))
    path.write_bytes(data)


class StreamingBudgetTests(unittest.TestCase):
    def test_routed_expert_extents_estimate_uncached_bytes_per_token(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'gemma4.gguf'
            routed_fixture(path)
            result = budget.inspect(path)
            self.assertEqual(result['expert_count'], 4)
            self.assertEqual(result['expert_used_count'], 2)
            self.assertEqual(len(result['routed_expert_layers']), 1)
            layer = result['routed_expert_layers'][0]
            self.assertEqual(layer['experts'], 4)
            self.assertEqual(layer['tensors'], 3)
            self.assertEqual(layer['bytes_per_expert'], 32 * 64 // 32 * 18 * 3)
            self.assertEqual(result['routed_expert_bytes_per_token_without_cache'],
                             layer['bytes_per_expert'] * 2)

    def test_prism_private_type_is_not_a_stock_quantization_alias(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'model.gguf'
            fixture(path, kind=143, dimension=128)
            result = budget.inspect(path)
            self.assertTrue(result['requires_prism_runtime'])
            self.assertEqual(result['tensors'][1]['bytes'], 28)
            self.assertEqual(result['tensors'][1]['type'], 143)

    def test_quantized_layers_and_resident_weights(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'model.gguf'
            fixture(path)
            result = budget.inspect(path)
            self.assertEqual(result['architecture'], 'llama')
            self.assertEqual(result['weight_bytes'], 52)
            self.assertEqual(result['non_layer_weight_bytes'], 16)
            self.assertEqual(result['two_layer_packed_slots_bytes'], 36)
            self.assertEqual(result['layers'][0]['span_bytes'], 18)
            self.assertEqual(result['tensors'][1]['offset'], result['data_start'] + 32)

    def test_truncated_and_overlapping_extents_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'model.gguf'
            fixture(path)
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(ValueError):
                budget.inspect(path)
            fixture(path, offset=0)
            with self.assertRaises(ValueError):
                budget.inspect(path)

    def test_unknown_quantization_and_bad_header_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'model.gguf'
            fixture(path, kind=999)
            with self.assertRaises(ValueError):
                budget.inspect(path)
            path.write_bytes(b'GGUF')
            with self.assertRaises(ValueError):
                budget.inspect(path)

    def test_probe_parallel_offset_tail_and_invalid_range(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'probe'
            compiler = os.environ.get('HOST_CC', os.environ.get('HOST_CXX', 'clang++').replace('clang++', 'clang'))
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-pthread',
                            str(ROOT / 'tools/stream-read-probe.c'), '-o', str(binary)], check=True)
            path = Path(directory) / 'bytes.bin'
            # More than one four-worker window, with nonzero offset and partial tail.
            size = 17 * 1024 * 1024 + 37
            with path.open('wb') as file:
                for _ in range(17):
                    file.write(bytes(range(256)) * 4096)
                file.write(bytes(range(37)))
            records = [json.loads(line) for line in subprocess.check_output(
                [str(binary), str(path), '11', str(size - 11)], text=True).splitlines()]
            self.assertEqual(len(records), 6)
            self.assertEqual({r['workers'] for r in records}, {1, 4})
            self.assertEqual(len({r['sample'] for r in records}), 1)
            self.assertTrue(all(r['staging_bytes'] == 16 * 1024 * 1024 for r in records))
            for offset, length in [('0', str(size + 1)), ('-1', '1'), ('0', '0')]:
                self.assertNotEqual(subprocess.run([str(binary), str(path), offset, length],
                                    capture_output=True).returncode, 0)
