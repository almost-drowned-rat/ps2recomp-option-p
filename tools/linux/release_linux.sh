#!/usr/bin/env bash
# release_linux.sh - a PS2 port's Linux release: ONE AppImage, the same release as the Windows exe (agent L,
# 2026-11-21). Also <APPIMAGE base>-linux-x86_64.tar.gz: the same without the AppImage (no FUSE needed). The AppImage holds the launcher (engine/launcher, its Windows 95 setup window drawn with SDL3); the
# payload - the engine's libraries and the port's own objects prebuilt, the translator, the game's player package,
# Python and the compiler bundle - is appended to it as player_release.py appends it to the Windows exe, and the
# launcher reads it from $APPIMAGE. The player's first start translates his disc and builds the game into data/
# beside the AppImage (only the AppImage on top - the maintainer's rule 2026-11-21), nothing downloaded. Never in it: game data, the game's code, the translated code.
#
#   bash engine/tools/linux/release_linux.sh --game games/zoe_us --release v0.8.3 --out <folder>
#        --llvm <the official LLVM release, unpacked> --sysroot <make_dev_sysroot.sh's folder>
#        --bundle <make_toolchain_bundle.sh's out folder> --generated <a translation's build/port/generated>
#        --package-from <the game's player package: game/> --appimagetool <appimagetool-x86_64.AppImage>
#        [--icon <png>] [--steps ...] [--script ...]   (the port's own release settings: release_settings.sh below)
#
# Builds (into <out>/build-port and <out>/build-launcher, with tools/linux/release_toolchain.cmake):
#   the port, as the player's build will be (the flags and the objects player_release.py reads from it);
#   the launcher with the port's names (the Windows release's PS2_LAUNCHER_* - see the game's port/release);
# then <out>/AppDir -> appimagetool -> the AppImage, and the payload appended to it.
set -euo pipefail
GAME= RELEASE= OUT= LLVM= SYSROOT= BUNDLE= GENERATED= PACKAGE= TOOL= ICON=
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME="$2"; shift 2 ;;
        --release) RELEASE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --llvm) LLVM="$2"; shift 2 ;;
        --sysroot) SYSROOT="$2"; shift 2 ;;
        --bundle) BUNDLE="$2"; shift 2 ;;
        --generated) GENERATED="$2"; shift 2 ;;
        --package-from) PACKAGE="$2"; shift 2 ;;
        --appimagetool) TOOL="$2"; shift 2 ;;
        --icon) ICON="$2"; shift 2 ;;
        --steps|--script) EXTRA+=("$1" "$2"); shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
for v in GAME RELEASE OUT LLVM SYSROOT BUNDLE GENERATED TOOL; do
    [ -n "${!v}" ] || { echo "missing --${v,,} (see the header)" >&2; exit 2; }
done
HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE="$(cd "$HERE/../.." && pwd)"
PROJECT="$(cd "$ENGINE/.." && pwd)"
GAME_DIR="$(cd "$PROJECT/$GAME" && pwd)"
TARGET="$(basename "$GAME_DIR")"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
# the port's release settings (its launcher names, the AppImage's name): <game>/port/release/linux.sh
SETTINGS="$GAME_DIR/port/release/linux.sh"
[ -f "$SETTINGS" ] || { echo "no $SETTINGS (the launcher's names, APPIMAGE_NAME, DESKTOP_NAME)" >&2; exit 2; }
# shellcheck source=/dev/null
. "$SETTINGS"
TC=(-DCMAKE_TOOLCHAIN_FILE="$HERE/release_toolchain.cmake" -DPS2_LINUX_LLVM="$LLVM" -DPS2_LINUX_SYSROOT="$SYSROOT"
    -DSDL_SNDIO=OFF -DSDL_IBUS=OFF)

echo "== the port (release build)"
cmake -S "$GAME_DIR/port" -B "$OUT/build-port" -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" \
    -DPS2_GENERATED_DIR="$GENERATED" -DPS2HOST_AVX2=ON -DPS2HOST_DEBUG_PANEL=OFF > "$OUT/build-port.configure.log"
ninja -C "$OUT/build-port" > "$OUT/build-port.log"
"$OUT/build-port/ps2host/ps2host_tests" > "$OUT/host_tests.log" 2>&1 || { echo "the host's tests failed" >&2; exit 1; }
tail -1 "$OUT/host_tests.log"

echo "== the launcher"
# its window icon: the --icon PNG's BMP twin beside it (<name>.bmp, 64x64, 32-bit with alpha), if there is one
ICON_DEF=()
[ -n "$ICON" ] && [ -f "${ICON%.png}.bmp" ] && ICON_DEF=(-DPS2_LAUNCHER_ICON="$(realpath "${ICON%.png}.bmp")")
cmake -S "$ENGINE/launcher" -B "$OUT/build-launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" \
    -DPS2_LAUNCHER_GAME="$LAUNCHER_GAME" -DPS2_LAUNCHER_DISC="$LAUNCHER_DISC" -DPS2_LAUNCHER_TITLE="$LAUNCHER_TITLE" \
    -DPS2_LAUNCHER_GAME_EXE="${LAUNCHER_GAME_EXE:-$TARGET}" -DPS2_LAUNCHER_OUTPUT=launcher \
    -DPS2_LAUNCHER_OLD_EXES="${LAUNCHER_OLD_EXES:-}" \
    -DPS2_LAUNCHER_EXTRA_DISC="${LAUNCHER_EXTRA_DISC:-}" -DPS2_LAUNCHER_EXTRA_KEY="${LAUNCHER_EXTRA_KEY:-}" \
    -DPS2_LAUNCHER_FILE_LABEL="${LAUNCHER_FILE_LABEL:-}" -DPS2_LAUNCHER_FILE_FILTER_NAME="${LAUNCHER_FILE_FILTER_NAME:-}" \
    -DPS2_LAUNCHER_FILE_FILTER="${LAUNCHER_FILE_FILTER:-}" -DPS2_LAUNCHER_FILE_NAME="${LAUNCHER_FILE_NAME:-}" \
    "${ICON_DEF[@]}" \
    > "$OUT/build-launcher.configure.log"
ninja -C "$OUT/build-launcher" > "$OUT/build-launcher.log"
max="$(objdump -T "$OUT/build-launcher/launcher" "$OUT/build-port/$TARGET" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1)"
case "$max" in GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|GLIBC_2.3[0-5]) echo "  glibc: $max at most" ;;
    *) echo "the build needs $max (more than glibc 2.35)" >&2; exit 1 ;; esac

# The payload goes on the launcher itself (since 2026-11-24), once, for both: the AppImage's usr/bin/<game>-launcher
# and the tar.gz's launcher. So it is there however the launcher runs - the AppImage started, the AppImage unpacked
# and its AppRun started without APPIMAGE (RecompLauncher does so and deletes the AppImage file: SDK guide part 1,
# "AppImages"), or the tar.gz. (Before, it was appended to the AppImage file, gone once unpacked.)
echo "== the payload"
PKG=()
[ -n "$PACKAGE" ] && PKG=(--package-from "$PACKAGE")
rm -rf "$OUT/release" "$OUT/release-payload"
mkdir -p "$OUT/release"
cp "$OUT/build-launcher/launcher" "$OUT/launcher-plain"
"$LLVM/bin/llvm-objcopy" --strip-unneeded "$OUT/launcher-plain"
python3 -I "$ENGINE/tools/player_release.py" --linux --game "$GAME_DIR" --release "$RELEASE" \
    --launcher "$OUT/launcher-plain" --out "$OUT/release-payload" --tree "$OUT/build-port" --llvm-bundle "$BUNDLE" \
    --llvm-root "$LLVM" --sysroot "$SYSROOT" --game-exe launcher "${PKG[@]}" "${EXTRA[@]}"
LAUNCHER_PAYLOAD="$OUT/release-payload/launcher"
# RecompLauncher's manifest (the maintainer's rule 2026-11-24: a fixed part of every release): the port's
# port/release/recomplauncher.json, at /recomplauncher.json in the AppImage and on top of the tar.gz's folder
MANIFEST="$GAME_DIR/port/release/recomplauncher.json"
[ -f "$MANIFEST" ] || echo "  WARNING: no $MANIFEST (RecompLauncher's manifest)" >&2

echo "== the AppImage"
A="$OUT/AppDir"
rm -rf "$A"
mkdir -p "$A/usr/bin"
cp "$LAUNCHER_PAYLOAD" "$A/usr/bin/$TARGET-launcher"
chmod 755 "$A/usr/bin/$TARGET-launcher"
[ -f "$MANIFEST" ] && cp "$MANIFEST" "$A/recomplauncher.json"
cat > "$A/AppRun" <<EOF
#!/bin/sh
# the launcher: it builds the game into data/ beside this AppImage at the first start, then starts it. Unpacked and
# started without APPIMAGE (RecompLauncher), the game's folder is APPDIR - this AppRun's folder unless already set.
HERE="\$(dirname "\$(readlink -f "\$0")")"
[ -n "\$APPIMAGE" ] || export APPDIR="\${APPDIR:-\$HERE}"
exec "\$HERE/usr/bin/$TARGET-launcher" "\$@"
EOF
chmod 755 "$A/AppRun"
cat > "$A/$TARGET.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$DESKTOP_NAME
Comment=$DESKTOP_COMMENT
Exec=$TARGET-launcher
Icon=$TARGET
Categories=Game;
Terminal=false
EOF
if [ -n "$ICON" ]; then cp "$ICON" "$A/$TARGET.png"
else echo "  WARNING: no --icon: a plain placeholder" >&2
     python3 -I -c "
import struct, zlib, sys
w = h = 256
row = b'\x00' + bytes([192, 192, 192, 255]) * w
raw = row * h
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)" "$A/$TARGET.png"; fi
ln -sf "$TARGET.png" "$A/.DirIcon"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$A" "$OUT/release/$APPIMAGE_NAME" \
    > "$OUT/appimagetool.log" 2>&1 || { tail -20 "$OUT/appimagetool.log" >&2; exit 1; }
chmod 755 "$OUT/release/$APPIMAGE_NAME"

# The same release as a tar.gz, for systems without FUSE: a folder with the launcher itself (no AppImage around it,
# the same payload on it) - it reads its own file and builds the game into data/ beside it. Its name: the
# launcher's (<game>_launcher, LAUNCHER_TAR_EXE), the only program on top (the maintainer's rule 2026-11-21).
echo "== the tar.gz"
TAR_BASE="${APPIMAGE_NAME%-x86_64.AppImage}"
TAR_NAME="$TAR_BASE-linux-x86_64.tar.gz"
TAR_EXE="${LAUNCHER_TAR_EXE:-$TAR_BASE}"
rm -rf "$OUT/tar-stage"
mkdir -p "$OUT/tar-stage/$TAR_BASE"
cp "$LAUNCHER_PAYLOAD" "$OUT/tar-stage/$TAR_BASE/$TAR_EXE"
chmod 755 "$OUT/tar-stage/$TAR_BASE/$TAR_EXE"
[ -f "$MANIFEST" ] && cp "$MANIFEST" "$OUT/tar-stage/$TAR_BASE/recomplauncher.json"
[ -n "$ICON" ] && cp "$ICON" "$OUT/tar-stage/$TAR_BASE/icon.png"
printf '%s\n' "$DESKTOP_NAME - start ./$TAR_EXE (the first start builds the game from your disc, in this folder)." \
    > "$OUT/tar-stage/$TAR_BASE/README.txt"
tar -C "$OUT/tar-stage" -czf "$OUT/release/$TAR_NAME" "$TAR_BASE"
rm -rf "$OUT/release-payload" "$OUT/tar-stage" "$OUT/launcher-plain"
(cd "$OUT/release" && sha256sum "$APPIMAGE_NAME" "$TAR_NAME" > SHA256SUMS.txt && cat SHA256SUMS.txt)
echo "done: $OUT/release/$APPIMAGE_NAME, $OUT/release/$TAR_NAME"
