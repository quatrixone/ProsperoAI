#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build pinned RADV inputs on Ubuntu 24.04 using packaged LLVM 19 host libraries.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$root/tools/deps.py" fetch
ref="$root/.deps/mihawk-vulkan-review"
# PS5LM: arena sub-allocation in the PS5 winsys, so small buffers stop leaving
# physical holes in the kernel's direct-memory pool when they die. The recipe
# exports Mesa with git archive at its pinned revision, so the patch has to be
# committed inside the checkout and the recipe's pin has to name that commit.
# A restored cache keeps both, hence the guard.
mesa="$root/.deps/mihawk-mesa-review"
stamp="$root/.deps/.radv-arena-stamp"
arena_hash=$(sha256sum "$root/tools/radv-ps5-arena.patch" | cut -d' ' -f1)
if grep -q RADV_PS5_ARENA_CUTOFF "$mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c" &&
   [ "$(cat "$stamp" 2>/dev/null)" = "$arena_hash" ]; then
 echo "arena patch already present at $(git -C "$mesa" rev-parse --short HEAD)"
else
 if grep -q RADV_PS5_ARENA_CUTOFF "$mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c"; then
  # a stale arena commit from a previous patch revision: drop it, then re-apply
  git -C "$mesa" reset -q --hard HEAD~1
 fi
 git -C "$mesa" -c user.email=ci@quatrix.one -c user.name="ProsperoAI CI" \
   am -q "$root/tools/radv-ps5-arena.patch"
 grep -q RADV_PS5_ARENA_CUTOFF "$mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c" ||
  { echo "the arena patch did not land in $mesa" >&2; exit 1; }
 echo "$arena_hash" > "$stamp"
fi
sed -i "s/^mesa_revision=.*/mesa_revision=$(git -C "$mesa" rev-parse HEAD)/" "$ref/tools/build-radv.sh"
# debug #36: the radv step died inside a meson setup whose log tail showed no
# error line — show the whole setup log and name the step that failed.
sed -i 's/tail -20 "\$clc_build.setup.log"/echo "[ci] clc meson setup failed:"; cat "$clc_build.setup.log"/;
        s/tail -20 "\$build.setup.log"/echo "[ci] radv meson setup failed:"; cat "$build.setup.log"/g;
        s|export PATH="\$clc_bin:\$PATH"|export PATH="$clc_bin:$PATH"\necho "[ci] clc tools built"|;
        s|"\$meson" configure "\$build"|echo "[ci] meson configure for build-id"; "$meson" configure "$build"|' "$ref/tools/build-radv.sh"
export PS5_PAYLOAD_SDK_FORK="$root/.deps/mihawk-sdk-review"
export PS5_MESA_FORK="$root/.deps/mihawk-mesa-review"
bash "$ref/tools/setup-native-dependencies.sh"
host="$root/.deps/vulkan-host"
if [[ ! -x $host/venv/bin/meson ]]; then
 python3 -m venv "$host/venv"
 "$host/venv/bin/pip" install meson==1.7.2 mako==1.3.10 ply==3.11 packaging==24.2 PyYAML==6.0.2
fi
mkdir -p "$host/bin"
cat > "$host/bin/glslangValidator" <<'WRAPPER'
#!/usr/bin/env bash
if [[ ${1:-} == --version ]]; then
 /usr/bin/glslangValidator --version | sed -n '1p'
else
 exec /usr/bin/glslangValidator "$@"
fi
WRAPPER
chmod +x "$host/bin/glslangValidator"
export MESON="$host/venv/bin/meson" NINJA=/usr/bin/ninja
export PATH="$host/bin:$host/venv/bin:/usr/lib/llvm-19/bin:$PATH"
export CC=clang-19 CXX=clang++-19
export CMAKE_PREFIX_PATH="/usr/lib/llvm-19${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
bash "$ref/tools/build-radv.sh" release
