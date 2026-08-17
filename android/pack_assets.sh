#!/usr/bin/env bash
# ===========================================================================
# pack_assets.sh -- build the Android asset payload from the SAME build/ tree
# the desktop harness reads.
#
#   ./android/pack_assets.sh [TRACK_ID]        (default: $B3_TRACK or US_C3_V1)
#
# Output: android/app/src/main/assets/burnout3_assets.zip, whose entries are
# rooted at "build/..." so that MainActivity's extractor + the native chdir()
# reproduce the desktop working directory exactly.  Re-run this after ANY
# extractor rerun (tools/extract_*.py) and rebuild the APK -- that is the
# whole refresh story; nothing is hand-copied.
#
# The full build/ tree is ~6.7 GiB (4.3 GiB of it audio, 722 MiB of music,
# plus gigabytes of debug_dump_*.bmp captures).  What goes in the APK is the
# subset the harness actually opens for ONE track; the knobs below widen it.
#
# Environment knobs:
#   B3_MUSIC_TRACKS=N   EA TRAX streams to bundle       (default 2, 0 = none)
#   B3_PACK_CRASH_AUDIO=1  add build/audio/rws_crash*   (default off, ~147 MiB)
#   B3_PACK_ALL_CARS=0  restrict build/cars to .obj/.hull/... only (see below)
#   B3_ZIP_LEVEL=N      deflate level                   (default 6)
# ===========================================================================
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
src="$root/build"
out="$here/app/src/main/assets/burnout3_assets.zip"

track="${1:-${B3_TRACK:-US_C3_V1}}"
music_n="${B3_MUSIC_TRACKS:-2}"
zip_level="${B3_ZIP_LEVEL:-6}"

[ -d "$src" ] || { echo "no build/ tree at $src -- run the extractors first" >&2; exit 1; }
tdir="$src/tracks/$track"
[ -d "$tdir" ] || { echo "no build/tracks/$track" >&2; exit 1; }

stage=$(mktemp -d -t b3assets.XXXXXX)
trap 'rm -rf "$stage"' EXIT
b="$stage/build"
mkdir -p "$b"

# Hard-link where we can (same filesystem): staging must not copy ~200 MiB.
link() {  # link <src-file-or-dir> <dest-path-under-build/>
    local s="$1" d="$b/$2"
    [ -e "$s" ] || { echo "  (skip, absent) $s"; return 0; }
    mkdir -p "$(dirname "$d")"
    # The staging dir usually sits on tmpfs while build/ is on ext4, so the
    # hard-link attempt fails cross-filesystem AFTER creating the directory
    # skeleton; a bare `cp -a dir existing-dir` would then nest (dir/dir/...).
    # Scrub the destination before each attempt so both paths copy TO $d.
    rm -rf "$d"
    cp -al "$s" "$d" 2>/dev/null || { rm -rf "$d"; cp -a "$s" "$d"; }
}

echo "== packing track $track =="

# -- 1. the active track's geometry, promoted to the top-level names the
#       harness hard-codes (build/track.obj, build/collision.bin,
#       build/textures/).  Taking them from tracks/<ID>/ rather than from the
#       loose top-level copies keeps the payload self-consistent even when
#       the checkout currently has a DIFFERENT track installed.
link "$tdir/track.obj"      track.obj
link "$tdir/track.mtl"      track.mtl
link "$tdir/collision.bin"  collision.bin
link "$tdir/textures"       textures
[ -e "$b/track.obj" ]     || link "$src/track.obj"     track.obj
[ -e "$b/track.mtl" ]     || link "$src/track.mtl"     track.mtl
[ -e "$b/collision.bin" ] || link "$src/collision.bin" collision.bin
[ -e "$b/textures" ]      || link "$src/textures"      textures

# -- 2. per-track data the code reads out of build/tracks/<ID>/ by name.
#       (route.bin / grid.bin / traffic.bin are extractor outputs baked into
#       src/burnout3_*_data.h at build time -- not opened at runtime.)
for f in envmap.png light_probes.bin props.bin; do
    link "$tdir/$f" "tracks/$track/$f"
done

# -- 3. art banks
link "$src/cars"        cars
link "$src/frontend"    frontend
link "$src/carfx"       carfx
link "$src/particlefx"  particlefx
link "$src/boostfx"     boostfx
for f in "$src"/postfx/"$track"_*; do link "$f" "postfx/$(basename "$f")"; done

# -- 4. loose files
link "$src/Globalus.bin" Globalus.bin
link "$src/mixer.cfg"    mixer.cfg

# -- 5. audio.  b3_sfx_init() reads build/audio/<bank>/<file>.wav for exactly
#       four banks; load_real_audio() reads the player car's four rpm-labelled
#       engine loops out of awd_pveh_<CLASS>_<CarN>_high/.  Everything else
#       under build/audio (4.3 GiB: per-track ambience, EATrax0/1, Movie,
#       DJ*, the *_low variants, the .wma twins) is left out.
for bank in awd_crashmod awd_single awd_generic "awd_$track"; do
    link "$src/audio/$bank" "audio/$bank"
done
for d in "$src"/audio/awd_pveh_*_high; do
    [ -d "$d" ] || continue
    n=$(basename "$d")
    for w in "$d"/eng_*.wav; do
        [ -e "$w" ] && link "$w" "audio/$n/$(basename "$w")"
    done
done
if [ "${B3_PACK_CRASH_AUDIO:-0}" = "1" ]; then
    for d in "$src"/audio/rws_crash*; do
        [ -d "$d" ] && link "$d" "audio/$(basename "$d")"
    done
fi

# -- 6. music: b3_music_init() scans build/music/track_%02d.wav upward and
#       stops at the first gap, so a prefix of the 44 EA TRAX streams works.
if [ "$music_n" -gt 0 ]; then
    i=0
    while [ "$i" -lt "$music_n" ]; do
        f=$(printf "%s/music/track_%02d.wav" "$src" "$i")
        [ -e "$f" ] || break
        link "$f" "music/$(basename "$f")"
        i=$((i + 1))
    done
    link "$src/music/eatrax.txt" music/eatrax.txt
fi

# -- 7. stamp.  MainActivity compares the stamp inside the APK with the one
#       on disk and re-extracts when they differ -- this is what makes a
#       repack after an extractor rerun actually land on the device.
{
    echo "track=$track"
    echo "packed=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host=$(hostname)"
    ( cd "$stage" && find build -type f -printf '%s %p\n' | sort | sha256sum )
} > "$b/ASSET_STAMP"

mkdir -p "$(dirname "$out")"
rm -f "$out"
( cd "$stage" && zip -qr -"$zip_level" -X "$out" build )

echo
echo "staged (uncompressed): $(du -sh "$stage" | cut -f1)"
echo "wrote $out ($(du -h "$out" | cut -f1))"
( cd "$stage" && du -sh build/* | sort -h | tail -12 )
