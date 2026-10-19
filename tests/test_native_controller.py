"""Compile the real UI controller against deterministic host runtime/storage fakes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct
import json

ROOT = Path(__file__).resolve().parents[1]


def tool(name, *args):
    """Runs a build tool that prints a folder (the kit staged at its pin, the baked fonts)."""
    return Path(subprocess.run(["bash", str(ROOT / "tools" / name), *args], check=True,
                               stdout=subprocess.PIPE, text=True).stdout.strip())


class NativeController(unittest.TestCase):
    def test_real_catalog_paging(self):
        with tempfile.TemporaryDirectory(prefix="prospero-catalog-") as directory:
            directory = Path(directory)
            models = directory / "models"
            models.mkdir()
            for index in range(24):
                (models / f"variant-{index:02d}.gguf").write_bytes(b"fixture")
            (models / ".hidden.gguf").write_bytes(b"fixture")
            (models / "interrupted.gguf.part").write_bytes(b"fixture")
            (models / "model.ps5lm").write_bytes(b"legacy bundle")
            binary = directory / "catalog-test"
            subprocess.run([os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-O1",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                            "-Wl,--gc-sections", "-DPS5_LLAMA_VULKAN", "-DPROSPERO_PRISM_RUNTIME",
                            "-I" + str(ROOT / "build/prism-llama-source/include"),
                            "-I" + str(ROOT / "build/prism-llama-source/ggml/include"),
                            "-I" + str(ROOT / "include"),
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/native_catalog_test.cpp"),
                            str(ROOT / "inference/adapters/llama/vulkan.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(models)], check=True, timeout=15)

    def test_allocator_realloc(self):
        with tempfile.TemporaryDirectory(prefix="prospero-allocator-") as directory:
            binary = Path(directory) / "allocator-test"
            subprocess.run([os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-O1", "-pthread",
                            "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/native_allocator_test.cpp"),
                            str(ROOT / "tooling/native/app_cpp_runtime.cpp"),
                            str(ROOT / "src/agc_lifecycle.cpp"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_worker_state_and_unicode(self):
        with tempfile.TemporaryDirectory(prefix="prospero-controller-") as directory:
            directory = Path(directory)
            kit = tool("prepare-ui-kit.sh") / "src"
            fonts = tool("bake-fonts.sh")
            binary = directory / "controller-test"
            logs = directory / "logs"
            logs.mkdir()
            subprocess.run([os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-pthread",
                            "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-Wall", "-Wextra", "-Werror", "-DPROSPERO_HOST",
                            f'-DPROSPERO_SETTINGS_PATH="{directory / "settings.cfg"}"',
                            "-I" + str(ROOT / "include"),
                            "-I" + str(ROOT / ".deps/llama.cpp/vendor"),
                            "-I" + str(ROOT / "search-engine/include"),
                            "-I" + str(ROOT / "search-engine/src"),
                            "-I" + str(kit),
                            str(ROOT / "tests/native_controller_test.cpp"),
                            str(ROOT / "src/native_app.cpp"),
                            str(ROOT / "src/debug_log.cpp"),
                            str(ROOT / "src/media_preview.cpp"),
                            # The conversation understands /search and /fetch, so it
                            # links the search module; on the host it has no network.
                            str(ROOT / "search-engine/src/query.cpp"),
                            str(ROOT / "search-engine/src/providers.cpp"),
                            str(ROOT / "search-engine/src/scan.cpp"),
                            str(ROOT / "search-engine/src/cache.cpp"),
                            str(ROOT / "search-engine/src/search.cpp"),
                            str(ROOT / "search-engine/src/transport.cpp"),
                            str(kit / "gfx/font.cpp"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(fonts / "inter-regular.huifont"),
                            str(fonts / "noto-sans-east-asian.huifont"), str(logs)],
                           check=True, timeout=30)
