import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QwenImageEditingTests(unittest.TestCase):
    def test_projector_is_optional_for_text_generation_and_required_for_edits(self):
        (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / "build/tests") as work:
            binary = Path(work) / "qwen-image-editing"
            subprocess.run([
                os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-I" + str(ROOT / "include"),
                str(ROOT / "tests/qwen_image_editing_test.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary), work], check=True)


if __name__ == "__main__":
    unittest.main()
