# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock
ROOT = Path(__file__).resolve().parents[1]
class VulkanRuntimeTests(unittest.TestCase):
    def test_model_prepare_uses_persisted_kv_cache_setting(self):
        source = (ROOT / 'inference/adapters/llama/vulkan.cpp').read_text()
        prepare = source.split('int gpt_runtime_prepare() {', 1)[1].split('\n}', 1)[0]
        self.assertRegex(
            prepare,
            r'configure_context\s*\(\s*settings\.context\s*,\s*'
            r'prospero_settings::kv_name\(settings\.kv\)\s*\)',
        )

    def test_sheetsage_unloads_models_before_retryable_memory_failure(self):
        source = (ROOT / 'inference/adapters/media/sheetsage_ps5.cpp').read_text()
        self.assertIn('EngineStatus ensure_engine(', source)
        ensure = source.split('EngineStatus ensure_engine(', 1)[1].split(
            '\n// audio_options keys', 1)[0]
        load = ensure.index('audiocpp_model_load(')
        measurement = ensure.index('prospero_memory::free_total()')
        gate = ensure.index('backend != "cpu" && graph_free < kSheetSageGraphMinimumFree')
        session = ensure.index('audiocpp_session_create(')
        self.assertLess(load, gate)
        self.assertLess(measurement, gate)
        self.assertLess(gate, session)
        cover = source.split('extern "C" int ps5_sheetsage_transcribe(', 1)[1]
        self.assertIn('release_engine();', cover)
        release = cover.index('release_engine();')
        self.assertIn('prospero_memory::free_total()', cover)
        budget = cover.index('prospero_memory::free_total()')
        self.assertLess(release, budget)
        self.assertIn('EngineStatus::low_memory', cover)
        self.assertIn('was unloaded', cover)
        self.assertNotIn('NEEDS_RESTART', source)

    def test_sheetsage_checks_largest_block_before_creating_vulkan_session(self):
        source = (ROOT / 'inference/adapters/media/sheetsage_ps5.cpp').read_text()
        ensure = source.split('EngineStatus ensure_engine(', 1)[1].split(
            '\n// audio_options keys', 1)[0]
        load = ensure.index('audiocpp_model_load(')
        largest_gate = ensure.index(
            'backend != "cpu" && largest_free < kSheetSageMinimumLargestFree')
        session = ensure.index('audiocpp_session_create(')
        self.assertIn('kSheetSageMinimumLargestFree = 3600ull << 20', source)
        self.assertLess(load, largest_gate)
        self.assertLess(largest_gate, session)

    def test_runtime_abi_and_vulkan_media(self):
        directory = ROOT / 'build/vulkan-runtime-upstream-host'
        binaries = [directory / name for name in ('vulkan-runtime-abi-test', 'vulkan-runtime-media-test')]
        if not all(path.exists() for path in binaries):
            if os.environ.get('PROSPERO_REQUIRE_VULKAN_RUNTIME') == '1':
                self.fail('mandatory vulkan-runtime binaries are missing')
            self.skipTest('bash tools/build-vulkan-runtime.sh host required')
        environment = {**os.environ, 'GGML_NO_BACKTRACE': '1'}
        software = next(iter(sorted(Path('/usr/share/vulkan/icd.d').glob('lvp_icd*.json'))), None)
        if software is not None:
            environment['VK_ICD_FILENAMES'] = str(software)
        if environment.get('VK_ICD_FILENAMES'):
            environment['GGML_VK_VISIBLE_DEVICES'] = '0'
        for binary in binaries:
            with self.subTest(binary=binary.name):
                command = [str(binary)]
                model = ROOT / 'build/vulkan-models/stories260K.gguf'
                if binary.name == 'vulkan-runtime-abi-test':
                    command.extend([str(model) if model.exists() else '',
                                    str(ROOT / 'tests/fixtures/tiny-torch-state.pkl')])
                subprocess.run(command, check=True, env=environment, timeout=120)

    def test_required_build_cannot_skip_missing_artifacts(self):
        with mock.patch.dict(os.environ, {'PROSPERO_REQUIRE_VULKAN_RUNTIME': '1'}), \
             mock.patch.object(Path, 'exists', return_value=False):
            with self.assertRaisesRegex(AssertionError, 'mandatory vulkan-runtime binaries are missing'):
                self.test_runtime_abi_and_vulkan_media()
