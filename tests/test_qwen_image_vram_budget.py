import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QwenImageVramBudgetTests(unittest.TestCase):
    def test_budget_file_values_are_strictly_bounded_mib(self):
        (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / "build/tests") as work:
            binary = Path(work) / "qwen-image-vram-budget"
            subprocess.run([
                os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-I" + str(ROOT / "include"),
                str(ROOT / "tests/qwen_image_vram_budget_test.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary), work], check=True)
