"""Each curated image preset must resolve to the files it installs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ImageCheckpointLayoutTests(unittest.TestCase):
    def test_every_image_preset_resolves_its_installed_files(self):
        compiler = shutil.which(os.environ.get('HOST_CXX', 'clang++')) or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('a host C++ compiler is required')
        with tempfile.TemporaryDirectory(prefix='prospero-image-layout-') as directory:
            binary = Path(directory) / 'check'
            subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(ROOT / 'include'),
                            str(ROOT / 'tests/image_checkpoint_layout_test.cpp'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
