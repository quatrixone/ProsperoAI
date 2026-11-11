#!/usr/bin/env bash
# ProsperoAI - Pinned ps5-opengl SDK download (after ps5-homebrew-ui tools/fetch-opengl-sdk.sh).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads and verifies the pinned ps5-opengl release into .deps/ps5-opengl
# and prints the SDK prefix (the directory holding manifest.sha256).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=1.0.2
archive_sha256=30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4
manifest_sha256=0d432edc9d648d6e0cccb4d5eb3db8484f10ba4b075b5ccf23edda3251b66360
# The upstream account was renamed and its ps5-opengl release vanished (2026-10-19);
# try our byte-identical mirror first, then the community forks, then the original URL.
urls=(
    "https://github.com/Skystar9999/ProsperoAI/releases/download/mirror-opengl-sdk-1.0.2/ps5-opengl-sdk-1.0.2.tar.gz"
    "https://github.com/LedyBacer/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz"
    "https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz"
)

cache="$root/.deps/ps5-opengl"
archive="$cache/ps5-opengl-sdk-$version.tar.gz"
extracted="$cache/ps5-opengl-sdk-$version"
prefix="$extracted/sdk"
mkdir -p "$cache"

if [[ ! -f $prefix/manifest.sha256 ]]; then
    if [[ ! -f $archive ]] || ! sha256sum --check --status <<<"$archive_sha256  $archive"; then
        printf '==> [opengl] Downloading ps5-opengl SDK %s\n' "$version" >&2
        for candidate in "${urls[@]}"; do
            if curl -fL --retry 3 -o "$archive.part" "$candidate"; then break; fi
            printf '==> [opengl] mirror unavailable: %s\n' "$candidate" >&2
            rm -f -- "$archive.part"
        done
        [[ -f $archive.part ]] || {
            echo "ps5-opengl SDK download failed from every known mirror" >&2
            exit 2
        }
        mv -- "$archive.part" "$archive"
    fi
    sha256sum --check --status <<<"$archive_sha256  $archive" || {
        echo "ps5-opengl SDK archive checksum mismatch" >&2
        exit 2
    }
    rm -rf -- "$extracted"
    # Only the compiled SDK and the license texts are needed to build.
    tar -xzf "$archive" -C "$cache" "ps5-opengl-sdk-$version/sdk" \
        "ps5-opengl-sdk-$version/LICENSE" "ps5-opengl-sdk-$version/LICENSES" \
        "ps5-opengl-sdk-$version/THIRD_PARTY_NOTICES.md"
    rm -f -- "$archive"
fi

(cd "$prefix" && sha256sum --check --strict --quiet manifest.sha256) || {
    echo "ps5-opengl SDK manifest verification failed: $prefix" >&2
    exit 2
}
actual=$(sha256sum "$prefix/manifest.sha256" | cut -d' ' -f1)
[[ $actual == "$manifest_sha256" ]] || {
    echo "unexpected ps5-opengl SDK manifest: $actual" >&2
    exit 2
}
printf '%s\n' "$prefix"
