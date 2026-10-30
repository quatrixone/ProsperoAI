# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioWorkflowsTests(unittest.TestCase):
    def test_audio_library_and_runtime_operations(self):
        with tempfile.TemporaryDirectory(prefix="prospero-audio-workflows-") as directory:
            temp = Path(directory)
            audio = temp / "audio"
            audio.mkdir()
            binary = temp / "audio-workflows-test"
            cxx = shlex.split(os.environ.get("HOST_CXX", "clang++"))
            cflags = shlex.split(
                os.environ.get(
                    "HOST_TEST_CXXFLAGS",
                    "-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror "
                    "-ffunction-sections -fdata-sections",
                )
            )
            ldflags = shlex.split(os.environ.get("HOST_TEST_LDFLAGS", "-Wl,--gc-sections"))
            definitions = [
                f'-DPROSPERO_AUDIO_DIR="{audio}/"',
                f'-DPROSPERO_SETTINGS_PATH="{temp / "settings.cfg"}"',
            ]
            sources = [ROOT / "tests/audio_workflows_test.cpp"]
            implementation = ROOT / "src/audio_workflows.cpp"
            if implementation.exists():
                sources.append(implementation)
            subprocess.run(
                cxx + cflags + ["-pthread", "-I" + str(ROOT / "include")] + definitions + [
                    *map(str, sources),
                    *ldflags,
                    "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary), str(temp / "library")], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
