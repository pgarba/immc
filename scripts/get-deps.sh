#!/bin/sh
# immc draws its entire interface with Dear ImGui, and ImGui is not vendored in
# this repository. This puts a copy where CMakeLists.txt looks for it:
# extern_dont_commit/imgui.
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
