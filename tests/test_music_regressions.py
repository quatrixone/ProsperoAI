# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MusicRegressionTests(unittest.TestCase):
    def test_stems_resampling_checks_duration_before_allocating_output(self):
        source = (ROOT / "inference/adapters/media/stems_ps5.cpp").read_text()
        self.assertIn("output_frame_count", source)
        guard = source.index("output_frame_count")
        allocation = source.index("std::vector<float> resampled")
        self.assertLess(guard, allocation)

    def test_cover_uses_selected_web_audio_constructors(self):
        source = (ROOT / "assets/web/music.js").read_text()
        cover = source[source.index("// ---- cover (audio -> ABC"):]
        self.assertIn("window.webkitAudioContext", cover)
        self.assertIn("window.webkitOfflineAudioContext", cover)
        self.assertNotIn("AudioContext.prototype", cover)
        self.assertNotIn("new OfflineAudioContext", cover)

    def test_music_volume_listener_is_shared_not_created_per_player(self):
        source = (ROOT / "assets/web/music.js").read_text()
        listener = source.index("addEventListener('music-vol'")
        player_start = source.index("function makePlayer(")
        player_end = source.index("function makeStemGroup(")
        self.assertFalse(player_start < listener < player_end)
        library_start = source.index("function loadLibrary()")
        library_end = source.index("// ----", library_start + 1)
        self.assertIn("playerAudios = []", source[library_start:library_end])

    def test_stems_resampling_limits(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a C++ compiler is required")
        build = ROOT / "build/tests"
        build.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="prospero-stems-limits-", dir=build) as directory:
            binary = Path(directory) / "stems-audio-limits"
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "include"),
                            str(ROOT / "tests/stems_audio_limits_test.cpp"), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
