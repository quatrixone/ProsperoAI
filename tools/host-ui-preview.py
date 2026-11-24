#!/usr/bin/env python3
"""Build and render the actual native frontend using host-only runtime fixtures.

usage: tools/host-ui-preview.py [output folder] [--reel]

PROSPERO_STRESS=1 fills the library with 500 models; PROSPERO_EMPTY=1 leaves it empty.
"""
import os
from pathlib import Path
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]
VULKAN = "--vulkan" in sys.argv
BUILD = ROOT / ("build/ui-preview-vulkan" if VULKAN else "build/ui-preview")
BUILD.mkdir(parents=True, exist_ok=True)
arguments = [value for value in sys.argv[1:] if not value.startswith("--")]
OUTPUT = Path(arguments[0]) if arguments else BUILD / "screenshots"
OUTPUT.mkdir(parents=True, exist_ok=True)


def tool(name, *args):
    return Path(subprocess.run(["bash", str(ROOT / "tools" / name), *args], check=True,
                               stdout=subprocess.PIPE, text=True).stdout.strip())


# The kit at its pinned commit, and the same fonts the console build packages.
KIT = tool("prepare-ui-kit.sh") / "src"
FONTS = tool("bake-fonts.sh")
kit_sources = [KIT / line for line in (ROOT / "ui-kit/sources.txt").read_text().split()
               if line.endswith(".cpp") and not line.startswith("platform/")]
sources = [ROOT / "src/i18n.cpp", ROOT / "src/i18n_it.cpp", ROOT / "src/native_app.cpp", ROOT / "search-engine/src/query.cpp",
           ROOT / "search-engine/src/providers.cpp", ROOT / "search-engine/src/scan.cpp",
           ROOT / "search-engine/src/cache.cpp", ROOT / "search-engine/src/search.cpp",
           ROOT / "search-engine/src/transport.cpp", ROOT / "src/native_ui.cpp",
           ROOT / "src/voice_recorder.cpp", ROOT / "third_party/qrcodegen/qrcodegen.cpp",
           ROOT / "src/native_ui_screens.cpp", ROOT / "src/native_ui_style.cpp",
           ROOT / "src/native_ui_update.cpp",
           ROOT / "src/font_set.cpp", ROOT / "src/media_preview.cpp",
           ROOT / "src/dev_script.cpp", ROOT / "src/debug_log.cpp",
           ROOT / "host/ui_preview.cpp", ROOT / "host/platform_host.cpp", *kit_sources]
if VULKAN:
    subprocess.run([sys.executable, str(ROOT / "tools/prepare-vulkan-ui-shaders.py"),
                    '--kit', str(KIT), '--output', str(BUILD / 'generated')], check=True)
    sources = [p for p in sources if p.name != 'gl_program.cpp']
    sources += [ROOT / 'vulkan/ui/backend.cpp', ROOT / 'vulkan/ui/program.cpp']
cxx = os.environ.get("HOST_CXX", "clang++")
flags = ["-std=c++20", "-O2", "-g", "-fexceptions", "-fno-rtti", "-DGL_GLEXT_PROTOTYPES",
         "-DPROSPERO_HOST", f'-DPROSPERO_SETTINGS_PATH="{BUILD / "settings.cfg"}"',
         "-I" + str(ROOT / "include"), "-I" + str(ROOT / "search-engine/include"), "-I" + str(ROOT / "search-engine/src"), "-I" + str(ROOT / "src"), "-I" + str(ROOT / "vulkan"), "-I" + str(KIT)]
if VULKAN:
    flags += ['-DPROSPERO_UI_VULKAN', '-I' + str(ROOT / 'vulkan'), '-I' + str(ROOT / 'vulkan/ui'), '-I' + str(BUILD / 'generated')]
flags += os.environ.get("HOST_PREVIEW_CXXFLAGS", "").split()
headers_time = max(p.stat().st_mtime for base in (KIT, ROOT / "include", ROOT / "src")
                   for p in base.rglob("*.hpp"))


def compile_source(source):
    obj = BUILD / (str(source.relative_to(ROOT)).replace("/", "_") + ".o")
    if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, headers_time):
        subprocess.run([cxx, *flags, "-c", str(source), "-o", str(obj)], check=True)
    return str(obj)


with ThreadPoolExecutor(max_workers=int(os.environ.get("BUILD_JOBS", "6"))) as pool:
    objects = list(pool.map(compile_source, sources))
binary = BUILD / "prospero-ui-preview"
subprocess.run([cxx, *os.environ.get("HOST_PREVIEW_CXXFLAGS", "").split(), *objects, "-pthread",
                *(["-ldl"] if VULKAN else ["-lEGL", "-lGL"]), "-Wl,--wrap=fopen", "-o", str(binary)], check=True)
command = [str(binary), str(FONTS), str(OUTPUT)]
if "--reel" in sys.argv:
    encoder = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo",
        "-pixel_format", "rgb24", "-video_size", "1280x720", "-framerate", "20", "-i", "-",
        "-vf", "vflip", "-c:v", "libwebp_anim", "-lossless", "1", "-q:v", "60",
        "-compression_level", "3", "-loop", "0", str(OUTPUT / "prospero-native-motion.webp")], stdin=subprocess.PIPE)
    try:
        subprocess.run(command, stdout=encoder.stdin, env={**os.environ, "PROSPERO_REEL": "1"}, check=True)
    finally:
        encoder.stdin.close()
    if encoder.wait() != 0:
        raise SystemExit("animation encoding failed")
else:
    subprocess.run(command, check=True)
from PIL import Image
for ppm in OUTPUT.glob("*.ppm"):
    with Image.open(ppm) as image:
        image.save(ppm.with_suffix(".png"))
    ppm.unlink()
