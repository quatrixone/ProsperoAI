#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# The app compiles against PacBrew headers and links its static archives directly. `make deps`
# only validates selected PacBrew packages, so ensure the pinned sysroot exists for clean CI
# runners where the dependency cache has not been populated yet.
bash "$root/tools/setup-pacbrew-dependencies.sh" --all >/dev/null
ref="$root/.deps/mihawk-vulkan-review"
sdk="$ref/.deps/native/ps5-payload-sdk"
archive="$ref/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
# All text and media now share upstream GGML v0.26.0 and its Vulkan backend.
# Legacy AGC/private-ggml comparison builds live in earlier commits.
[[ ${SHARED_SD:-1} == 1 && ${MEDIA_VULKAN:-1} == 1 ]] || {
 echo "AGC inference was removed; this build requires shared Vulkan media" >&2; exit 2;
}
work="$root/build/vulkan-runtime"
llama_source="$root/.deps/llama.cpp"
runtime_build="$root/build/vulkan-runtime-upstream-ps5"
llama_build="$runtime_build/llama"
ggml_build="$runtime_build/ggml"
[[ -f $llama_build/src/libllama.a ]] || { echo "Run make vulkan-runtime-libs first" >&2; exit 2; }
title=${PROSPERO_APP_TITLE:?Set PROSPERO_APP_TITLE to the target title ID}
[[ $title =~ ^PPSA[0-9]{5}$ ]] || { echo "Invalid title ID" >&2; exit 2; }
app="$work/$title"
label=${BUILD_LABEL:-vulkan-runtime}
[[ $label =~ ^[A-Za-z0-9\ ,._#-]{1,40}$ ]] || { echo "Invalid build label" >&2; exit 2; }
bash "$root/tools/prepare-opengl.sh" >/dev/null
kit_stage=$(bash "$root/tools/prepare-ui-kit.sh")
kit=$(bash "$root/tools/prepare-ui-kit.sh" --kit)
fonts=$(bash "$root/tools/bake-fonts.sh")
mkdir -p "$work/obj" "$work/stubs" "$app/sce_sys" "$app/sce_module" "$work/generated"
printf '%s\n' "$label" > "$app/build-label.txt"
python3 "$root/tools/prepare-vulkan-ui-shaders.py" --kit "$kit_stage/src" --output "$work/generated"
version=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["contentVersion"])' "$root/sce_sys/param.json")
printf '#define PROSPERO_VERSION "%s"\n#define PROSPERO_BUILD_LABEL "%s"\n' "$version" "$label" > "$work/generated/prospero_build.h"
cc(){ PS5_PAYLOAD_SDK="$sdk" sh "$root/tooling/prospero-clang18" "$@"; }
export CCACHE_DIR=${CCACHE_DIR:-$root/build/ccache}
source "$root/tools/ninja-build.sh"
ninja_begin "$work/app.ninja"
objects=()
sources=(src/kv_budget.cpp src/prefix_reuse.cpp src/main.cpp src/native_app.cpp src/native_ui.cpp src/native_ui_screens.cpp src/native_ui_style.cpp third_party/qrcodegen/qrcodegen.cpp
 src/font_set.cpp src/media_preview.cpp src/dev_script.cpp src/gpt_input.cpp src/gpt_ime.cpp
 search-engine/src/query.cpp search-engine/src/providers.cpp search-engine/src/scan.cpp
 search-engine/src/cache.cpp search-engine/src/search.cpp search-engine/src/transport.cpp
 src/media_output_ps5.cpp src/voice_recorder.cpp src/session_store.cpp src/runtime_support.cpp
 src/agc_lifecycle.cpp inference/router.cpp inference/adapters/registry.cpp
 inference/adapters/media/stable_diffusion_ps5.cpp inference/adapters/media/stable_audio_ps5.cpp
 inference/adapters/media/kokoro_ps5.cpp inference/adapters/media/ace_step_ps5.cpp
 inference/adapters/media/chatterbox_ps5.cpp inference/adapters/media/yue2_ps5.cpp
 inference/adapters/media/sheetsage_ps5.cpp
 inference/adapters/media/stems_ps5.cpp
 tools/yue2-ggml-compat.cpp
 vulkan/storage.cpp vulkan/storage_paths.cpp vulkan/elevation/elevation.cpp vulkan/debug_tee.cpp src/debug_log.cpp
 src/native_ui_update.cpp vulkan/update_ps5.cpp
 inference/adapters/llama/vulkan.cpp vulkan/http_server.cpp vulkan/model_downloader_ps5.cpp
 vulkan/ui/backend.cpp vulkan/ui/program.cpp)
media_definitions=()
# Speech is kokoro.cpp on the shared ggml/Vulkan backend (inference/adapters/media/kokoro_ps5.cpp); the
# prebuilt OpenCL archive it replaces is no longer linked.
kokoro_archives=("$runtime_build/kokoro/libkokoro.a"
 "$runtime_build/kokoro/highway/libhwy.a")
# Not `archive`: that variable already holds RADV's library for radv_link_recipe below.
for kokoro_input in "${kokoro_archives[@]}"; do
 [[ -f $kokoro_input ]] || { echo "Run make vulkan-runtime-libs first: missing $kokoro_input" >&2; exit 2; }
done
while IFS= read -r relative; do
 [[ -n $relative && $relative != \#* ]] || continue
 [[ $relative != gfx/gl_program.cpp && $relative != platform/ps5/display_egl.cpp ]] || continue
 sources+=("${kit_stage#"$root/"}/src/$relative")
done < "$root/ui-kit/sources.txt"
# UPDATE_DEV_OFFER=1: the update offer comes from update-offer.txt in the app's folder instead
# of the catalog (third_party/update-check/README.md). Never for a release.
update_definitions=()
[[ ${UPDATE_DEV_OFFER:-0} != 1 ]] || update_definitions+=(-DPROSPERO_UPDATE_DEV_OFFER=1)
for relative in "${sources[@]}"; do
 media_includes=()
 abi_definitions=()
 abi_definitions+=(-DGGML_MAX_NAME=160)
 case "$relative" in
  inference/adapters/media/stable_diffusion_ps5.cpp)
   media_includes=(-I "$root/.deps/stable-diffusion-vulkan/include");;
  inference/adapters/media/stable_audio_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include");;
  inference/adapters/media/ace_step_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include");;
  inference/adapters/media/yue2_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include");;
  inference/adapters/media/sheetsage_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include");;
  inference/adapters/media/stems_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include");;
  inference/adapters/media/chatterbox_ps5.cpp)
   media_includes=(-I "$root/build/audio-cpp-upstream/include"
    -I "$root/.deps/pacbrew/v0.40.2/sysroot/user/homebrew/include");;
  inference/adapters/media/kokoro_ps5.cpp)
   media_includes=(-I "$root/.deps/kokoro.cpp/include"
    -I "$root/build/ggml-upstream/include");;
  tools/yue2-ggml-compat.cpp)
   media_includes=(-I "$root/build/ggml-upstream/include");;
 esac
 object="$work/obj/${relative//\//_}.o"
 ninja_inputs=("$root/$relative" "$root/tooling/prospero-clang18")
 if [[ $relative == *.c ]]; then
  ninja_edge CC "$object" env PS5_PAYLOAD_SDK="$sdk" USE_CCACHE="${USE_CCACHE:-1}" sh "$root/tooling/prospero-clang18" \
   -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -DPS5_DUAL_BACKEND -DPS5_SANDBOX_APP \
   -DPS5_APP_HAS_DSO_HANDLE -I "$root/include" -I "$root/src" \
   -MD -MF "$object.d" -c "$root/$relative" -o "$object"
  objects+=("$object")
  continue
 fi
 ninja_edge CXX "$object" env PS5_PAYLOAD_SDK="$sdk" USE_CCACHE="${USE_CCACHE:-1}" sh "$root/tooling/prospero-clang18" \
  -std=c++20 -O2 -fexceptions -fcxx-exceptions -frtti -Wall -Wextra -Wno-missing-field-initializers \
  -ffunction-sections -fdata-sections -DPS5_LLAMA_VULKAN -DPROSPERO_UI_VULKAN -DPROSPERO_VULKAN_RUNTIME \
  -DPS5_MEDIA_AUDIO -DPS5_MEDIA_IMAGE -DPS5_SANDBOX_APP -DPS5_APP_HAS_DSO_HANDLE -DPS5_SD_GENERATE -DGL_GLEXT_PROTOTYPES \
  -DSDL_MAIN_HANDLED -DSDL_STATIC_LIB -DUSING_GENERATED_CONFIG_H \
  "${media_definitions[@]}" "${abi_definitions[@]}" "${media_includes[@]}" -I "$root/include" -I "$root/vulkan" -I "$root/vulkan/ui" -I "$kit_stage/src" -I "$work/generated" \
  -I "$root/vendor/ps5/sdl/include" -I "$root/vendor/ps5/sdl/include/SDL2" -I "$root/inference" \
  -I "$llama_source/vendor" -I "$llama_source/include" -I "$llama_source/ggml/include" \
  -I "$root/vendor/include" -I "$root/src" -I "$root/search-engine/include" -I "$root/search-engine/src" \
  -I "$root/third_party/update-check" "${update_definitions[@]}" -I "$root/.deps/Vulkan-Headers/include" -I "$root/.deps/ps5-opengl/current/include" \
  -MD -MF "$object.d" -c "$root/$relative" -o "$object"
 objects+=("$object")
done
ninja_run
# ONNX Runtime is optional for the Vulkan app. Build it only when requested; model support
# can be added in a separate release after its dependency is ready.
if [[ ${PROSPERO_WITH_ONNXRUNTIME:-0} == 1 ]]; then
 bash "$root/tools/build-onnxruntime-ps5.sh"
 ort_object="$root/build/onnxruntime-ps5/Release/libonnxruntime-ps5.o"
 [[ -f $ort_object ]] || { echo "Missing embedded ONNX Runtime object: $ort_object" >&2; exit 2; }
 objects+=("$ort_object")
fi
# The model downloader's HTTPS (vulkan/net): libcurl with OpenSSL, libpsl and zstd from the
# pinned PacBrew prefix, linked into one object with console_curl.c. What that file puts in
# place of libc for them (name lookup, fcntl on sockets, a few more functions) gets a name of
# its own there, so the rest of the app keeps the functions it had. zlib is not taken from
# the prefix: the Vulkan driver's archive already has it.
pacbrew=$(bash "$root/tools/setup-pacbrew-dependencies.sh" --all | tail -n 1)/user/homebrew
[[ -f $pacbrew/lib/libcurl.a && -f $pacbrew/lib/libcrypto.a ]] || {
 echo "libcurl is missing from the PacBrew prefix ($pacbrew): run make pacbrew" >&2; exit 2; }
net_objects=()
for name in console_curl https_get; do
 cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -DCURL_STATICLIB=1 \
  -I "$pacbrew/include" -I "$root/vulkan/net" -c "$root/vulkan/net/$name.c" -o "$work/obj/net_$name.o"
 net_objects+=("$work/obj/net_$name.o")
done
# The update kit (third_party/update-check) uses the same libcurl and OpenSSL, for the
# catalog's signature and the release's download. Its paths are the app's own.
for name in update_check self_update self_update_ps5 self_update_sha256; do
 cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -DCURL_STATICLIB=1 \
  -include "$root/third_party/update-check/self_update_paths.h" \
  -I "$pacbrew/include" -I "$root/vulkan/net" -I "$root/third_party/update-check" \
  -c "$root/third_party/update-check/$name.c" -o "$work/obj/net_$name.o"
 net_objects+=("$work/obj/net_$name.o")
done
net_rename=(--redefine-sym fcntl=__wrap_fcntl --redefine-sym __real_fcntl=fcntl)
for name in getaddrinfo freeaddrinfo gai_strerror gethostbyname getnameinfo fnmatch getpwuid_r \
  _setjmp _longjmp openlog closelog dladdr if_nametoindex pipe2 recvmmsg sendmmsg popen pclose \
  isatty mkstemp gmtime_r ZSTD_trace_compress_begin ZSTD_trace_compress_end \
  ZSTD_trace_decompress_begin ZSTD_trace_decompress_end; do
 net_rename+=(--redefine-sym "$name=console_curl_$name")
done
ld.lld-18 -r -o "$work/obj/net.all.o" "${net_objects[@]}" --start-group "$pacbrew/lib/libcurl.a" \
 "$pacbrew/lib/libpsl.a" "$pacbrew/lib/libssl.a" "$pacbrew/lib/libcrypto.a" "$pacbrew/lib/libzstd.a" --end-group
llvm-objcopy-18 "${net_rename[@]}" "$work/obj/net.all.o" "$work/obj/net.o"
objects+=("$work/obj/net.o")
for name in app_crt app_cpp_runtime; do
 cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$ref/tooling/native/$name.cpp" -o "$work/obj/$name.o"
done
cc -std=c11 -O2 -c "$root/tooling/native/llama_ps5_compat.c" -o "$work/obj/llama_compat.o"
for lib in Agc AgcDriver; do
 source=agc_canary_link_stub.c
 [[ $lib != AgcDriver ]] || source=agc_driver_canary_link_stub.c
 cc -std=c11 -O2 -fPIC -c "$ref/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$lib.o"
 stub_objects=("$work/obj/$lib.o")
 "$sdk/bin/prospero-lld" --shared -soname "libSce$lib.prx" -o "$work/stubs/libSce$lib.so" "${stub_objects[@]}"
done
cc -O2 -fPIC -c "$root/tooling/native/prosperoai_import_stub_common_dialog.cpp" -o "$work/obj/CommonDialog.o"
"$sdk/bin/prospero-lld" --shared -soname libSceCommonDialog.prx -o "$work/stubs/libSceCommonDialog.so" "$work/obj/CommonDialog.o"
cc -O2 -fPIC -c "$root/tooling/native/prosperoai_import_stub_audio_in.cpp" -o "$work/obj/AudioIn.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAudioIn.prx -o "$work/stubs/libSceAudioIn.so" "$work/obj/AudioIn.o"
source "$ref/tools/radv-link.sh"
export PS5_CLANG=${PS5_CLANG:-clang-18}
radv_link_recipe "$ref" "$sdk" "$archive"
for symbol in pthread_once strtof strtod fseek ftell strcasestr; do radv_link_flags+=("--defsym=$symbol=prospero_$symbol"); done
# Every function that takes a path goes through vulkan/storage_paths.cpp, which turns a
# sandbox name (/app0, /download0) into the real one once the app has filesystem access.
for symbol in fopen freopen open stat lstat mkdir rmdir unlink remove rename access opendir \
  sceKernelOpen sceKernelMkdir sceKernelRmdir sceKernelUnlink; do radv_link_flags+=("--wrap=$symbol"); done
# What the app prints for the console's log is also kept by the debug log (vulkan/debug_tee.cpp).
radv_link_flags+=(--wrap=sceKernelDebugOutText)
if [[ ${PROSPERO_WITH_ONNXRUNTIME:-0} == 1 ]]; then
 radv_link_flags+=(--undefined=OrtGetApiBase)
fi
cat > "$work/app-compat.map" <<'MAP'
{ local: pthread_once; strtof; strtod; fseek; ftell; strcasestr; gl*; hui_release_splash; __wrap_*; };
MAP
sd_archives=("$runtime_build/sd/libstable-diffusion.a")
audio_build="$runtime_build/audio-cpp"
audio_archives=("$runtime_build/bin/libaudiocpp.a" "$audio_build/libengine_runtime.a"
 "$audio_build/external/sentencepiece/src/libsentencepiece.a" "$audio_build/libcjson_vendor.a"
 "$audio_build/libyaml_vendor.a")
audio_archives+=("$root/.deps/pacbrew/v0.40.2/sysroot/user/homebrew/lib/libFLAC.a")
for audio_input in "${audio_archives[@]}"; do
 [[ -f $audio_input ]] || { echo "Run make vulkan-runtime-libs first: missing $audio_input" >&2; exit 2; }
done
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr --no-dynamic-linker "${radv_link_flags[@]}" --wrap=sceAgcInit \
 --version-script "$work/app-compat.map" --version-script "$ref/tooling/native/app-symbols.map" --exclude-libs=ALL -e _start \
 -o "$work/llvm-pie.elf" "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "${objects[@]}" "$work/obj/llama_compat.o" \
 "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "$work/stubs/libSceCommonDialog.so" "$work/stubs/libSceAudioIn.so" \
 --start-group "$root/vendor/ps5/sdl/lib/libSDL2.a" "$llama_build/src/libllama.a" \
 "$ggml_build/src/libggml.a" "$ggml_build/src/libggml-base.a" \
 "$ggml_build/src/libggml-cpu.a" "$ggml_build/src/ggml-vulkan/libggml-vulkan.a" \
 "${sd_archives[@]}" "${audio_archives[@]}" \
 "${kokoro_archives[@]}" "$root/vendor/lib/libespeak-ng.a" \
 "$root/vendor/lib/libcompat.a" --end-group \
 --defsym=vkGetInstanceProcAddr=radv_GetInstanceProcAddr --defsym=vkGetDeviceProcAddr=vk_common_GetDeviceProcAddr \
 --defsym=vkCmdCopyBuffer=vk_common_CmdCopyBuffer --defsym=vkGetPhysicalDeviceFeatures2=vk_common_GetPhysicalDeviceFeatures2 \
 --defsym=execlp=prospero_execlp --defsym=posix_madvise=prospero_posix_madvise \
 "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
if [[ ${PROSPERO_WITH_ONNXRUNTIME:-0} == 1 ]]; then
 if readelf -d "$work/llvm-pie.elf" | grep 'libonnxruntime' >/dev/null; then
  echo "ONNX Runtime must be statically linked into eboot, not listed as a dependency" >&2
  exit 1
 fi
 nm "$work/llvm-pie.elf" | grep ' OrtGetApiBase$' >/dev/null || {
  echo "Embedded ONNX Runtime entry point is missing from the linked eboot" >&2; exit 1;
 }
fi
"$root/build/host/ps5-native-tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
 --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" --stub "$work/stubs/libSceCommonDialog.so" --stub "$work/stubs/libSceAudioIn.so" \
 --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$root/build/host/ps5-native-tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$root/runtime/libc.prx" "$app/sce_module/"
# What the console reads; the pictures' sources stay in the repository.
rm -f "$app/sce_sys/"*-source.png
for asset in param.json icon0.png pic0.dds pic1.dds snd0.at9; do
 cp "$root/sce_sys/$asset" "$app/sce_sys/"
done
python3 - "$app/sce_sys/param.json" "$title" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]));title=sys.argv[2]
if title != p['titleId']:
    p.update(titleId=title,conceptId=title[4:],contentId=f'UP9000-{title}_00-PROSPEROVKSMOKE1X')
with open(sys.argv[1],'w') as f:json.dump(p,f,indent=2)
PY
mkdir -p "$app/assets/fonts" "$app/assets/audio/sfx"
cp "$fonts/"*.huifont "$fonts/"*.txt "$app/assets/fonts/"
cp -a "$kit/assets/audio/sfx/." "$app/assets/audio/sfx/"
mkdir -p "$app/assets/web/fonts"
cp -a "$root/assets/web/." "$app/assets/web/"
rm -rf "$app/assets/audio/chatterbox-voices"
for font in Inter-Regular.ttf Inter-SemiBold.ttf Montserrat-Medium.ttf Inter-LICENSE.txt Montserrat-LICENSE.txt; do
    cp "$root/.deps/ui-kit/checkout/third_party/fonts/$font" "$app/assets/web/fonts/"
done
for sounds in "$app/assets/audio/sfx/"*/; do
 python3 - "$sounds" <<'PYINDEX'
from pathlib import Path
import sys
p=Path(sys.argv[1]);(p/'index.txt').write_text(''.join(f.name+'\n' for f in sorted(p.glob('*.wav'))))
PYINDEX
done
# Lapy's one-request helper for this exact title, built from its pinned source and checked
# against the manifest it comes with (tools/build-lapy-helper.py).
python3 "$root/tools/build-lapy-helper.py" "$title" "$work/lapy/$title"
rm -rf "$app/assets/platform"
mkdir -p "$app/licenses"
cp "$work/lapy/$title/lapy.elf" "$work/lapy/$title/lapy-manifest.json" "$app/"
cp "$work/lapy/$title/Lapy-MIT.txt" "$app/licenses/Lapy-MIT.txt"
# The self-update helper (third_party/self-update-helper), a payload for the console's loader
# like Lapy's and built with the same PS5 Payload SDK. The app sends it when the user accepts
# an update; it replaces the app's files once the app has closed.
make --no-print-directory -s -C "$root/third_party/self-update-helper" \
 PS5_PAYLOAD_SDK="$root/.deps/lapy/ps5-payload-sdk-v0.42" OUTPUT="$work/self-update/self-updater.elf"
python3 "$root/tools/validate-loader-elf.py" "$work/self-update/self-updater.elf"
cp "$work/self-update/self-updater.elf" "$app/self-updater.elf"
cp "$root/third_party/miniz/LICENSE" "$app/licenses/miniz-MIT.txt"
cp "$llama_source/LICENSE" "$app/licenses/llama.cpp-MIT.txt"
cp "$root/THIRD_PARTY_NOTICES.md" "$app/licenses/THIRD_PARTY_NOTICES.md"
printf '%s\n' 'llama.cpp cb7934c52ca8710994b2ecc19775ebefcfdb8d01; GGML v0.26.0 d7cb574130e6f01ad25b3289685489200febcd74; GGML_MAX_NAME=160' > "$app/licenses/vulkan-runtime-source.txt"
echo "Native Vulkan app folder: $app"
