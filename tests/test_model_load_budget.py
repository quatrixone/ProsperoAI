import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class ModelLoadBudgetTests(unittest.TestCase):
    def test_resident_admission_boundaries(self):
        (ROOT / 'build/tests').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/tests') as work:
            binary = Path(work) / 'budget'
            subprocess.run([os.environ.get('HOST_CXX', 'clang++'), '-std=c++20',
                '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT / 'include'),
                str(ROOT / 'tests/model_load_budget_test.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
