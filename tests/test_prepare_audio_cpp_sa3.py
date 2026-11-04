# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "prepare_audio_cpp_sa3", ROOT / "tools/prepare-audio-cpp-sa3.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PrepareAudioCppSa3Tests(unittest.TestCase):
    def test_reapplying_yue2_sources_does_not_rewrite_cmake(self):
        with tempfile.TemporaryDirectory(prefix="prospero-audio-cmake-") as directory:
            cmake = Path(directory) / "CMakeLists.txt"
            cmake.write_text(
                "    src/framework/modules/transformers/decoder.cpp\n"
                "    src/framework/modules/transformers/causal_decoder.cpp\n"
                "    src/framework/modules/transformers/causal_decoder_runtime.cpp\n")
            MODULE.include_yue2_dependencies(cmake)
            os.utime(cmake, ns=(1_000_000_000, 1_000_000_000))
            before = cmake.stat().st_mtime_ns
            MODULE.include_yue2_dependencies(cmake)
            self.assertEqual(cmake.stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
