#!/usr/bin/env bash
# make_dev_sysroot.sh - the sysroot the Linux RELEASE build compiles against (tools/linux/release_toolchain.cmake):
# the player's bundle sysroot (make_toolchain_bundle.sh: glibc 2.35) plus the Ubuntu 22.04 -dev packages SDL3 is
# built with (X11, Wayland, xkbcommon, libdecor, PulseAudio, PipeWire, ALSA, JACK, D-Bus, udev, EGL/GL, libusb, IBus)
# and their libraries (SDL reads their SONAMEs to load them at run time). Only the build machine uses it: SDL3 links
# none of them, the player's compile needs none of them. Agent L, 2026-11-21.
#
#   bash engine/tools/linux/make_dev_sysroot.sh --bundle-sysroot <bundle>/llvm/sysroot --debs <folder of .debs> --out <dir>
set -euo pipefail
SRC= DEBS= OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --bundle-sysroot) SRC="$2"; shift 2 ;;
        --debs) DEBS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$SRC" ] && [ -n "$DEBS" ] && [ -n "$OUT" ] || { echo "see the header" >&2; exit 2; }
[ -e "$OUT" ] && { echo "$OUT exists - give a new folder" >&2; exit 2; }
cp -a "$SRC" "$OUT"
OUT="$(cd "$OUT" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
n=0
for deb in "$DEBS"/*.deb; do
    rm -rf "$TMP/x" "$TMP/d"
    mkdir -p "$TMP/x" "$TMP/d"
    (cd "$TMP/x" && ar x "$(realpath "$deb")")
    tar -xf "$TMP"/x/data.tar.* -C "$TMP/d"
    for d in usr/include usr/lib/x86_64-linux-gnu lib/x86_64-linux-gnu usr/share/pkgconfig usr/lib/pkgconfig \
             usr/share/wayland usr/share/wayland-protocols usr/share/X11 usr/bin; do
        [ -e "$TMP/d/$d" ] && { mkdir -p "$OUT/$d"; cp -a "$TMP/d/$d/." "$OUT/$d/"; }
    done
    n=$((n + 1))
done
# absolute links (into the host) made relative inside the sysroot
while IFS= read -r -d '' ln; do
    t="$(readlink "$ln")"
    case "$t" in
        /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$ln")" "$OUT$t")" "$ln" ;;
    esac
done < <(find "$OUT" -type l -print0)
echo "dev sysroot: $n packages over the bundle's sysroot in $OUT ($(du -sm "$OUT" | cut -f1) MB)"
