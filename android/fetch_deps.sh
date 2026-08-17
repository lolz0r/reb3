#!/usr/bin/env bash
# ===========================================================================
# fetch_deps.sh -- clone the three third-party libraries the Android build
# needs into android/third_party/ (gitignored; nothing vendored into the
# repo).  Pinned tags, so a rebuild months from now produces the same APK.
#
#   ./android/fetch_deps.sh
#
# The desktop build needs none of this -- it links the system SDL2 /
# SDL2_image / libGL through pkg-config, exactly as the Makefile says.
# ===========================================================================
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tp="$here/third_party"
mkdir -p "$tp"

clone() {  # clone <dir> <url> <tag>
    local dir="$tp/$1" url="$2" tag="$3"
    if [ -d "$dir/.git" ]; then
        echo "== $1 already present ($(git -C "$dir" describe --tags 2>/dev/null || echo '?'))"
        return
    fi
    echo "== cloning $1 @ $tag"
    git clone --depth 1 --branch "$tag" "$url" "$dir"
}

clone SDL2       https://github.com/libsdl-org/SDL.git       release-2.32.10
clone SDL2_image https://github.com/libsdl-org/SDL_image.git release-2.8.8
# gl4es has no release tags; pin the commit the port was validated against.
if [ ! -d "$tp/gl4es/.git" ]; then
    echo "== cloning gl4es"
    git clone https://github.com/ptitSeb/gl4es.git "$tp/gl4es"
    git -C "$tp/gl4es" checkout 81547d9
else
    echo "== gl4es already present ($(git -C "$tp/gl4es" rev-parse --short HEAD))"
fi

echo
echo "third_party ready under $tp"
