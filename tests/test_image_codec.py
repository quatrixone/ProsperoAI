"""Host regression tests for bounded image decoding and PNG normalization."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ImageCodecTests(unittest.TestCase):
    def test_png_jpeg_validation_and_normalization(self):
        (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
        compiler = shutil.which(os.environ.get("HOST_CXX", "clang++")) or shutil.which("g++")
        if not compiler:
            self.skipTest("a C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="prospero-image-codec-",
                                         dir=ROOT / "build/tests") as directory:
            binary = Path(directory) / "image-codec-test"
            subprocess.run([
                compiler, "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / ".deps/stable-diffusion-vulkan/thirdparty"),
                str(ROOT / "tests/image_codec_test.cpp"),
                str(ROOT / "src/image_codec.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
