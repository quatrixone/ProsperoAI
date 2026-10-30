"""Saved conversations: what the interface writes, the store reads back."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STORE = Path("/tmp/prosperoai-session-test")


class SessionStoreTests(unittest.TestCase):
    def test_round_trip_of_a_saved_conversation(self):
        shutil.rmtree(STORE, ignore_errors=True)
        self.addCleanup(shutil.rmtree, STORE, ignore_errors=True)
        with tempfile.TemporaryDirectory(prefix="prospero-session-") as directory:
            binary = Path(directory) / "store"
            subprocess.run([
                os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-O1", "-g",
                "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers",
                "-DPROSPERO_SESSION_TEST", "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / ".deps/stable-diffusion-vulkan/thirdparty"),
                str(ROOT / "tests/session_store_test.cpp"), str(ROOT / "src/session_store.cpp"),
                str(ROOT / "src/image_codec.cpp"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
