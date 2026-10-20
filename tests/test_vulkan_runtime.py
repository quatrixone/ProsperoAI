# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / 'build/vulkan-runtime-upstream-host'


def host_environment():
    """Runs the host binaries against the software Vulkan driver when one is installed."""
    environment = {**os.environ, 'GGML_NO_BACKTRACE': '1'}
    software = next(iter(sorted(Path('/usr/share/vulkan/icd.d').glob('lvp_icd*.json'))), None)
    if software is not None:
        environment['VK_ICD_FILENAMES'] = str(software)
    if environment.get('VK_ICD_FILENAMES'):
        environment['GGML_VK_VISIBLE_DEVICES'] = '0'
    return environment


class VulkanRuntimeTests(unittest.TestCase):
    def require(self, *names):
        binaries = [DIRECTORY / name for name in names]
        if not all(path.exists() for path in binaries):
            if os.environ.get('PROSPERO_REQUIRE_VULKAN_RUNTIME') == '1':
                self.fail('mandatory vulkan-runtime binaries are missing')
            self.skipTest('bash tools/build-vulkan-runtime.sh host required')
        return binaries

    def test_runtime_abi_and_vulkan_media(self):
        binaries = self.require('vulkan-runtime-abi-test', 'vulkan-runtime-media-test')
        environment = host_environment()
        for binary in binaries:
            with self.subTest(binary=binary.name):
                command = [str(binary)]
                model = ROOT / 'build/vulkan-models/stories260K.gguf'
                if binary.name == 'vulkan-runtime-abi-test':
                    command.extend([str(model) if model.exists() else '',
                                    str(ROOT / 'tests/fixtures/tiny-torch-state.pkl')])
                subprocess.run(command, check=True, env=environment, timeout=120)

    def test_bounded_expert_cache_and_routed_upload(self):
        """Bounded expert reads must match fully resident weights on CPU and Vulkan."""
        cache, upload, probe = self.require('expert-cache-test', 'expert-upload-test',
                                            'expert-probe-test')
        environment = host_environment()
        with tempfile.TemporaryDirectory(prefix='prospero-expert-') as fixtures:
            subprocess.run([str(cache), fixtures], check=True, env=environment, timeout=300)
            subprocess.run([str(probe), fixtures], check=True, env=environment, timeout=300)
            for backend in ('cpu', 'vulkan'):
                with self.subTest(backend=backend):
                    subprocess.run([str(upload), backend, fixtures], check=True,
                                   env=environment, timeout=900)

    def test_required_build_cannot_skip_missing_artifacts(self):
        with mock.patch.dict(os.environ, {'PROSPERO_REQUIRE_VULKAN_RUNTIME': '1'}), \
             mock.patch.object(Path, 'exists', return_value=False):
            with self.assertRaisesRegex(AssertionError, 'mandatory vulkan-runtime binaries are missing'):
                self.test_runtime_abi_and_vulkan_media()
