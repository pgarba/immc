#!/bin/sh
# immc draws its entire interface with Dear ImGui, and ImGui is not vendored in
# this repository. This puts a copy where CMakeLists.txt looks for it:
# extern_dont_commit/imgui, and the one header the picture preview decodes with
# next to it in extern_dont_commit/stb.
#
# The commit is pinned rather than "latest". The app is built and tested against
# this one, and ImGui's master carries a "WIP" version string and moves under
# it, so an unpinned clone can break a build whose own source never changed.
#
# Set IMGUI_DEST to put the checkout somewhere else.
set -eu

IMGUI_REPO="${IMGUI_REPO:-https://github.com/ocornut/imgui}"
IMGUI_COMMIT="${IMGUI_COMMIT:-f502f9c980ce8c71ac0a8e326637a7bc71b06c03}"

here=$(cd "$(dirname "$0")" && pwd)
DEST="${IMGUI_DEST:-$here/../extern_dont_commit/imgui}"

if [ -d "$DEST/.git" ]; then
    printf 'imgui: %s is already a checkout, updating it\n' "$DEST"
    git -C "$DEST" fetch --depth 1 origin "$IMGUI_COMMIT" 2>/dev/null ||
        git -C "$DEST" fetch origin
else
    mkdir -p "$DEST"
    git -C "$DEST" init -q
    git -C "$DEST" remote add origin "$IMGUI_REPO"
    # One commit fetched rather than the whole history: the build needs one
    # snapshot of the tree, and the history is most of the download. If the
    # server refuses a fetch by SHA, the fallback brings every branch, which
    # makes the pinned commit reachable the slower way.
    git -C "$DEST" fetch --depth 1 origin "$IMGUI_COMMIT" 2>/dev/null ||
        git -C "$DEST" fetch origin
fi

git -C "$DEST" checkout -q "$IMGUI_COMMIT"
printf 'imgui %s (%s) at %s\n' \
    "$(git -C "$DEST" describe --tags 2>/dev/null || git -C "$DEST" rev-parse --short HEAD)" \
    "$(git -C "$DEST" rev-parse --short HEAD)" "$DEST"

# stb_image is what quick view decodes a picture with: one public domain header,
# no system library, so the build without libcurl is no different in this respect
# from the build with it.
#
# Pinned by commit and checked by hash for the same reason ImGui is, with one more
# reason of its own: stb carries no tags, so there is no v2.30 to point at, and the
# commit chosen is the one that last touched this file. The download lands as .tmp,
# the hash decides whether it is the file we expect, and only then does it become
# stb_image.h - a transfer the server refused partway through must not leave a
# truncated header for the next build to half-compile with.
STB_REPO="${STB_REPO:-https://raw.githubusercontent.com/nothings/stb}"
STB_COMMIT="${STB_COMMIT:-013ac3beddff3dbffafd5177e7972067cd2b5083}"
STB_SHA256="${STB_SHA256:-594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3}"

STB_DIR="${STB_DEST:-$here/../extern_dont_commit/stb}"
STB_FILE="$STB_DIR/stb_image.h"

stb_matches() {
    [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$STB_SHA256" ]
}

if stb_matches "$STB_FILE"; then
    printf 'stb_image.h: already there, and it matches\n'
else
    mkdir -p "$STB_DIR"
    curl -fsSL "$STB_REPO/$STB_COMMIT/stb_image.h" -o "$STB_FILE.tmp"
    if ! stb_matches "$STB_FILE.tmp"; then
        rm -f "$STB_FILE.tmp"
        printf 'stb_image.h: the download is not the pinned file\n' >&2
        exit 1
    fi
    mv "$STB_FILE.tmp" "$STB_FILE"
fi
printf 'stb_image %s at %s\n' "$STB_COMMIT" "$STB_FILE"
