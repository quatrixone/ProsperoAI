#!/usr/bin/env bash
# ProsperoAI - Fetch the interface kit at its pinned commit and stage its sources.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/prepare-ui-kit.sh           (prints the staged kit: <stage>/src)
#        tools/prepare-ui-kit.sh --kit     (prints the kit checkout: fonts, sounds, font baker)
#
# The kit (https://github.com/blackbearreloaded/ps5-homebrew-ui) is a build
# dependency, not part of this repository: nothing of it is copied or patched
# here. Its pinned commit is fetched once into .deps/ui-kit/checkout and its
# src/ is staged in .deps/ui-kit/stage, where every build compiles it from
# (see ui-kit/README.md). UI_KIT_DIR names a local checkout to use instead, for
# work on both at once.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
commit=f775c31d9b9809be7f26a4b6da1b1ddb28428652
# The upstream account was renamed (2026-10-19) and blackbearreloaded/ps5-homebrew-ui
# vanished; fetch from our full-history mirror first, then the original remote.
urls=(
    https://github.com/Skystar9999/ps5-homebrew-ui-mirror.git
    https://github.com/blackbearreloaded/ps5-homebrew-ui.git
)

cache="$root/.deps/ui-kit"
stage="$cache/stage"
mkdir -p "$cache"

if [[ -n ${UI_KIT_DIR:-} ]]; then
    kit=$(realpath -e -- "$UI_KIT_DIR")
    identity="local $(cd "$kit/src" && find . -type f -print0 | sort -z | xargs -0 sha256sum |
        sha256sum | cut -d' ' -f1)"
else
    kit="$cache/checkout"
    if [[ $(git -C "$kit" rev-parse HEAD 2>/dev/null || true) != "$commit" ]]; then
        printf '==> [ui-kit] Fetching ps5-homebrew-ui %s\n' "${commit:0:7}" >&2
        rm -rf -- "$kit"
        git init --quiet "$kit"
        for candidate in "${urls[@]}"; do
            git -C "$kit" remote add origin "$candidate"
            if git -C "$kit" fetch --quiet --depth 1 origin "$commit"; then break; fi
            printf '==> [ui-kit] remote unavailable: %s\n' "$candidate" >&2
            git -C "$kit" remote remove origin
        done
        git -C "$kit" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    fi
    [[ $(git -C "$kit" rev-parse HEAD) == "$commit" ]] || {
        echo "ps5-homebrew-ui checkout is not the pinned commit" >&2
        exit 2
    }
    identity="pinned $commit"
fi

if [[ ${1:-} == --kit ]]; then
    printf '%s\n' "$kit"
    exit 0
fi

# Staged again only when the kit or the list of what is compiled changed.
stamp=$( { printf '%s\n' "$identity"; sha256sum < "$root/ui-kit/sources.txt"; } |
    sha256sum | cut -d' ' -f1)
if [[ ! -f $stage/.stamp || $(< "$stage/.stamp") != "$stamp" ]]; then
    rm -rf -- "$stage.tmp"
    mkdir -p "$stage.tmp"
    cp -a -- "$kit/src" "$stage.tmp/src"
    while IFS= read -r relative; do
        [[ -z $relative || $relative == \#* ]] && continue
        [[ $relative =~ ^[A-Za-z0-9_./-]+\.cpp$ && $relative != *..* && -f $stage.tmp/src/$relative ]] || {
            echo "ui-kit/sources.txt names a file the kit does not have: $relative" >&2
            exit 2
        }
    done < "$root/ui-kit/sources.txt"
    printf '%s\n' "$stamp" > "$stage.tmp/.stamp"
    rm -rf -- "$stage"
    mv -- "$stage.tmp" "$stage"
fi
printf '%s\n' "$stage"
