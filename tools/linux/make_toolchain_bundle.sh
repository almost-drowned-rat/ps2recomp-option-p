#!/usr/bin/env bash
# make_toolchain_bundle.sh - the compiler + Python a PS2 port's Linux release ships (the Linux twin of the Windows
# release's slim llvm-mingw zip + embedded Python): the player's first start translates his disc with this Python
# and compiles the port with this clang, nothing downloaded. Agent L, 2026-11-21, after the Xbox toolkit's script of
# the same name (xboxrecomp option-c tools/linux/make_toolchain_bundle.sh, the same maintainer's project).
#
#   bash engine/tools/linux/make_toolchain_bundle.sh \
#       --llvm   <LLVM-23.1.x-Linux-X64.tar.zst (the official release)> \
#       --python <cpython-3.12.x+...-x86_64-unknown-linux-gnu-install_only.tar.gz (python-build-standalone)> \
#       --debs   <folder with the Ubuntu 22.04 .debs: libc6 libc6-dev linux-libc-dev libgcc-11-dev libgcc-s1 libstdc++6> \
#       --llvm-license <the LLVM project's LICENSE.TXT (the official Linux tarball has none)> \
#       --out    <empty or missing folder>
#
# Writes <out>/llvm: ONE clang binary named bin/clang++ (the driver in C++ mode - the translated code and the port are
# C++), bin/ld.lld, clang's resource headers + compiler-rt builtins, libc++ / libc++abi / libunwind headers and
# STATIC libraries (the game asks for no C++ runtime of the player's system), and sysroot/ = glibc 2.35 from Ubuntu
# 22.04: what is linked against it runs on every distribution with glibc >= 2.35 (the smoke test checks it).
# sysroot/usr/lib/x86_64-linux-gnu/libvulkan.so is a LINK STUB (SONAME libvulkan.so.1): the game uses the player's
# Vulkan loader. <out>/python: python-build-standalone, the bulk the translator never imports removed, ONE binary
# bin/python3.12. No symbolic links anywhere in <out> (the launcher's zip reader makes files only).
# Then a smoke test (a C++ program with threads, exceptions, math and dlopen, linked against a Vulkan call, built
# with the bundle only, run) and a size report.
#
# The engine's libraries and the port's own objects in the payload must be compiled with THIS bundle (the same
# libc++ headers, the same sysroot): tools/linux/release_linux.sh does that.
set -euo pipefail

LLVM_TAR= PY_TAR= DEBS= OUT= LLVM_LICENSE=
while [ $# -gt 0 ]; do
    case "$1" in
        --llvm) LLVM_TAR="$2"; shift 2 ;;
        --python) PY_TAR="$2"; shift 2 ;;
        --debs) DEBS="$2"; shift 2 ;;
        --llvm-license) LLVM_LICENSE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
for v in LLVM_TAR PY_TAR DEBS OUT; do
    [ -n "${!v}" ] || { echo "missing --${v,,} (see the header)" >&2; exit 2; }
done
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT")" ]; then
    echo "$OUT is not empty - give a new folder" >&2
    exit 2
fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
TMP="$OUT/.scratch"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# ── LLVM ────────────────────────────────────────────────────────────────────
echo "== llvm"
# (the official .tar.zst needs zstd's long window: 1 GB)
ZTAR=(tar -I "zstd -d --long=31")
case "$LLVM_TAR" in *.zst) ;; *) ZTAR=(tar) ;; esac
(cd "$TMP" && "${ZTAR[@]}" -tf "$LLVM_TAR") > "$TMP/llvm_list.txt"
TOP="$(sed -n '1s|/.*||p' "$TMP/llvm_list.txt")"
VER_MAJOR="$(grep -oE '/lib/clang/[0-9]+/' "$TMP/llvm_list.txt" | sed -n '1s|[^0-9]||gp')"
RT="lib/clang/$VER_MAJOR"
(cd "$TMP" && "${ZTAR[@]}" -xf "$LLVM_TAR" \
    "$TOP/bin/clang-$VER_MAJOR" "$TOP/bin/lld" "$TOP/bin/llvm-objcopy" \
    "$TOP/$RT/include" "$TOP/$RT/lib/x86_64-unknown-linux-gnu" "$TOP/lib/x86_64-unknown-linux-gnu" \
    "$TOP/include/c++" "$TOP/include/x86_64-unknown-linux-gnu")
L="$OUT/llvm"
mkdir -p "$L/bin" "$L/$RT/lib/x86_64-unknown-linux-gnu" "$L/lib/x86_64-unknown-linux-gnu" "$L/include"
OBJCOPY="$TMP/$TOP/bin/llvm-objcopy"       # only for this script
cp "$TMP/$TOP/bin/clang-$VER_MAJOR" "$L/bin/clang++"
cp "$TMP/$TOP/bin/lld" "$L/bin/ld.lld"
"$OBJCOPY" --strip-unneeded "$L/bin/clang++"
"$OBJCOPY" --strip-unneeded "$L/bin/ld.lld"
for a in libc++.a libc++abi.a libunwind.a libc++experimental.a; do
    [ -f "$TMP/$TOP/lib/x86_64-unknown-linux-gnu/$a" ] && cp "$TMP/$TOP/lib/x86_64-unknown-linux-gnu/$a" \
        "$L/lib/x86_64-unknown-linux-gnu/"
done
cp -rL "$TMP/$TOP/include/c++" "$L/include/"
cp -rL "$TMP/$TOP/include/x86_64-unknown-linux-gnu" "$L/include/"
cp -rL "$TMP/$TOP/$RT/include" "$L/$RT/"
for f in libclang_rt.builtins.a clang_rt.crtbegin.o clang_rt.crtend.o; do
    [ -f "$TMP/$TOP/$RT/lib/x86_64-unknown-linux-gnu/$f" ] && cp "$TMP/$TOP/$RT/lib/x86_64-unknown-linux-gnu/$f" \
        "$L/$RT/lib/x86_64-unknown-linux-gnu/"
done
find "$L/$RT/include" -maxdepth 1 \( -name 'arm*' -o -name 'riscv*' -o -name 'hexagon*' -o -name 'hvx*' \
    -o -name 'altivec.h' -o -name 'htm*' -o -name 'vecintrin.h' -o -name 'velintrin*' -o -name 'msa.h' \
    -o -name 'lasx*' -o -name 'lsx*' -o -name 'larchintrin.h' -o -name 'wasm*' -o -name 'sifive*' \
    -o -name 'ppc_wrappers' -o -name 'cuda_wrappers' -o -name 'openmp_wrappers' -o -name 'llvm_libc_wrappers' \
    -o -name 'zos_wrappers' -o -name '__clang_cuda*' -o -name '__clang_hip*' -o -name 'opencl*' \
    -o -name 'hlsl*' -o -name 'spirv*' -o -name 'gpuintrin*' -o -name 'nvptx*' -o -name 'amdgpu*' \) \
    -exec rm -rf {} +
echo "clang $("$L/bin/clang++" --version | sed -n 1p)"

# a .deb's files into a folder: dpkg-deb where there is one, else ar + tar (an Arch-based build machine)
deb_x() {
    if command -v dpkg-deb >/dev/null; then dpkg-deb -x "$1" "$2"; return; fi
    local t; t="$(mktemp -d "$TMP/deb.XXXXXX")"
    (cd "$t" && ar x "$1")
    tar -xf "$t"/data.tar.* -C "$2"
    rm -rf "$t"
}

# ── sysroot: glibc 2.35 from the Ubuntu 22.04 packages ──────────────────────
echo "== sysroot"
S="$L/sysroot"
mkdir -p "$TMP/debs"
for p in libc6 libc6-dev linux-libc-dev libgcc-11-dev libgcc-s1 libstdc++6; do   # libstdc++6: clang/lld's own
    deb="$(ls "$DEBS"/${p}_*_amd64.deb 2>/dev/null | head -1)"
    [ -n "$deb" ] || { echo "no $p deb in $DEBS" >&2; exit 2; }
    deb_x "$(realpath "$deb")" "$TMP/debs"
    echo "  $(basename "$deb")"
done
mkdir -p "$S"
for d in usr/include lib/x86_64-linux-gnu lib64 usr/lib/x86_64-linux-gnu usr/lib/gcc/x86_64-linux-gnu; do
    [ -e "$TMP/debs/$d" ] && { mkdir -p "$S/$(dirname "$d")"; cp -a "$TMP/debs/$d" "$S/$d"; }
done
rm -rf "$S/usr/lib/x86_64-linux-gnu/gconv" "$S/usr/lib/x86_64-linux-gnu/audit" "$S/usr/lib/x86_64-linux-gnu/perl"*
rm -rf "$S/usr/lib/gcc/x86_64-linux-gnu/11/plugin" "$S/usr/lib/gcc/x86_64-linux-gnu/11/include-fixed"
find "$S/usr/lib/x86_64-linux-gnu" -maxdepth 1 -name '*.a' ! -name '*_nonshared.a' \
    ! -name libpthread.a ! -name libdl.a ! -name librt.a ! -name libutil.a ! -name libanl.a -delete
# libstdc++ / libgcc_s of the GCC runtime: clang and lld load them (below) - not in the sysroot
find "$S" -name 'libstdc++*' -delete
# links resolved: absolute ones (into the host) made relative first, then every link a file of its own
while IFS= read -r -d '' ln; do
    t="$(readlink "$ln")"
    case "$t" in
        /*) rel="$(realpath -m --relative-to="$(dirname "$ln")" "$S$t")"; ln -sfn "$rel" "$ln" ;;
    esac
done < <(find "$S" -type l -print0)
while IFS= read -r -d '' ln; do
    if [ -e "$ln" ]; then cp -L --remove-destination "$(readlink -f "$ln")" "$ln.tmp" && mv -f "$ln.tmp" "$ln"
    else rm -f "$ln"; fi
done < <(find "$S" -type l -print0)

# ── what the official clang / lld load at run time ($ORIGIN/../lib) ─────────
echo "== llvm run-time libraries"
mkdir -p "$L/lib"
for so in libstdc++.so.6 libgcc_s.so.1; do
    src="$(find "$TMP/debs" -name "$so" | sort | sed -n 1p)"
    [ -n "$src" ] || { echo "no $so in the debs" >&2; exit 2; }
    cp -L "$src" "$L/lib/$so"
done
# lld's static libxml2 binds 7 ICU 70 functions (BIND_NOW); no current distribution has ICU 70 and an ELF link never
# reaches them (see the Xbox toolkit's script): small libraries that answer with an error
if readelf -d "$L/bin/ld.lld" | grep -q 'libicuuc.so'; then
    cat > "$TMP/icu_shim.c" <<'EOF'
typedef int UErrorCode;
#define U_UNSUPPORTED_ERROR 16
void *ucnv_open_70(const char *name, UErrorCode *err) { (void)name; if (err) *err = U_UNSUPPORTED_ERROR; return 0; }
void ucnv_close_70(void *cnv) { (void)cnv; }
void ucnv_setToUCallBack_70(void *c, void *f, const void *x, void *of, const void **ox, UErrorCode *err)
{ (void)c; (void)f; (void)x; (void)of; (void)ox; if (err) *err = U_UNSUPPORTED_ERROR; }
void ucnv_setFromUCallBack_70(void *c, void *f, const void *x, void *of, const void **ox, UErrorCode *err)
{ (void)c; (void)f; (void)x; (void)of; (void)ox; if (err) *err = U_UNSUPPORTED_ERROR; }
void ucnv_convertEx_70(void *t, void *s, char **tg, const char *tl, const char **sr, const char *sl, void *ps,
                       void **psrc, void **ptgt, const void *pl, char reset, char flush, UErrorCode *err)
{ (void)t; (void)s; (void)tg; (void)tl; (void)sr; (void)sl; (void)ps; (void)psrc; (void)ptgt; (void)pl;
  (void)reset; (void)flush; if (err) *err = U_UNSUPPORTED_ERROR; }
void UCNV_TO_U_CALLBACK_STOP_70(void) {}
void UCNV_FROM_U_CALLBACK_STOP_70(void) {}
EOF
    "$L/bin/clang++" -x c -shared -fPIC -O2 -nostdlib -Wl,-soname,libicuuc.so.70 -o "$L/lib/libicuuc.so.70" "$TMP/icu_shim.c"
    for n in libicui18n libicudata; do
        echo "int ${n}_shim_70;" > "$TMP/$n.c"
        "$L/bin/clang++" -x c -shared -fPIC -nostdlib -Wl,-soname,$n.so.70 -o "$L/lib/$n.so.70" "$TMP/$n.c"
    done
    echo "  ICU shims made"
fi
echo "  $("$L/bin/ld.lld" --version | sed -n 1p)"

# the Vulkan link stub (from clang's own include folder? no - from the engine's Vulkan headers: --vulkan-headers is
# not needed, the stub only has to name the functions the engine calls directly; volk loads the rest)
cat > "$TMP/vk_stub.c" <<'EOF'
/* generated by make_toolchain_bundle.sh: a link stub, never loaded (SONAME libvulkan.so.1) */
void vkGetInstanceProcAddr(void) {}
void vkGetDeviceProcAddr(void) {}
void vkCreateInstance(void) {}
void vkEnumerateInstanceExtensionProperties(void) {}
void vkEnumerateInstanceLayerProperties(void) {}
void vkEnumerateInstanceVersion(void) {}
EOF
"$L/bin/clang++" -x c --sysroot="$S" -fuse-ld=lld -shared -fPIC -O0 -nostdlib -Wl,-soname,libvulkan.so.1 \
    -o "$S/usr/lib/x86_64-linux-gnu/libvulkan.so" "$TMP/vk_stub.c"

# ── Python ──────────────────────────────────────────────────────────────────
echo "== python"
P="$OUT/python"
(cd "$TMP" && tar xzf "$PY_TAR")
mv "$TMP/python" "$P"
PYV="$(ls -d "$P"/lib/python3.* | head -1)"
rm -f "$PYV"/lib-dynload/_dbm.* "$PYV"/lib-dynload/_crypt.*
rm -rf "$PYV/test" "$PYV/idlelib" "$PYV/tkinter" "$PYV/turtledemo" "$PYV/ensurepip" "$PYV/lib2to3" \
       "$PYV/pydoc_data" "$PYV/turtle.py" "$PYV/config-"* "$P/include" "$P/share" \
       "$PYV"/lib-dynload/_tkinter* "$P"/lib/libtcl* "$P"/lib/libtk* "$P"/lib/tcl* "$P"/lib/tk* "$P"/lib/itcl* \
       "$P"/lib/thread* "$P"/lib/pkgconfig "$P"/lib/libpython*.a
find "$P" -name __pycache__ -type d -prune -exec rm -rf {} +
find "$P" -path '*/site-packages/pip*' -prune -exec rm -rf {} + 2>/dev/null || true
PYBIN="$(ls "$P"/bin/python3.1[0-9] | head -1)"
if ! readelf -d "$PYBIN" | grep -q 'NEEDED.*libpython'; then
    rm -f "$P"/lib/libpython3*.so*
fi
find "$P/bin" -mindepth 1 ! -name "$(basename "$PYBIN")" -delete
"$OBJCOPY" --strip-unneeded "$PYBIN"
find "$P" -name '*.so' -type f -exec "$OBJCOPY" --strip-unneeded {} \;
while IFS= read -r -d '' ln; do                 # (no links: the launcher's zip reader makes files only)
    if [ -e "$ln" ]; then cp -L --remove-destination "$(readlink -f "$ln")" "$ln.tmp" && mv -f "$ln.tmp" "$ln"
    else rm -f "$ln"; fi
done < <(find "$P" -type l -print0)
"$PYBIN" -I -c "import sys, zlib, hashlib, json, struct, concurrent.futures; print('python', sys.version.split()[0])"
"$PYBIN" -I -m compileall -q -j 0 "$PYV" >/dev/null
mkdir -p "$P/LICENSES"
cp "$PYV/LICENSE.txt" "$P/LICENSES/Python.txt"

# ── licences ────────────────────────────────────────────────────────────────
echo "== licences"
LIC="$L/LICENSES"
mkdir -p "$LIC"
if [ -n "$LLVM_LICENSE" ] && grep -q 'LLVM Exceptions' "$LLVM_LICENSE"; then
    cp "$LLVM_LICENSE" "$LIC/LLVM.txt"
else
    echo "give --llvm-license <LLVM project LICENSE.TXT (Apache-2.0 with LLVM exceptions)>" >&2
    exit 2
fi
for p in libc6 libc6-dev linux-libc-dev libgcc-11-dev libgcc-s1 libstdc++6; do
    c="$TMP/debs/usr/share/doc/$p/copyright"
    [ -f "$c" ] || c="$(find "$TMP/debs/usr/share/doc" -path "*/${p%%[0-9]*}*/copyright" -type f | sed -n 1p)"
    if [ -n "$c" ] && [ -f "$c" ]; then
        cp "$c" "$LIC/$p.copyright.txt"
    else
        # Ubuntu links the GCC runtime's doc folder to gcc-<n>-base (not among the debs): the same GPL-3 + GCC Runtime
        # Library Exception 3.1 texts from the build host (Debian: gcc-*-base/copyright; Arch: licenses/gcc-libs)
        { echo "# $p (Ubuntu 22.04) ships no copyright file of its own: GPL-3 with the GCC Runtime Library Exception 3.1."
          for g in $(ls /usr/share/doc/gcc-*-base/copyright 2>/dev/null | sort -V | tail -1) \
                   /usr/share/licenses/gcc-libs/RUNTIME.LIBRARY.EXCEPTION /usr/share/licenses/gcc/RUNTIME.LIBRARY.EXCEPTION \
                   /usr/share/licenses/gcc-libs/COPYING3 /usr/share/licenses/spdx/GPL-3.0-or-later.txt; do
              [ -f "$g" ] && { echo; echo "# The build host's $g:"; echo; cat "$g"; }
          done; true; } > "$LIC/$p.copyright.txt"
    fi
done
cat > "$LIC/NOTICE.txt" <<EOF
Compiler bundle of a ps2recomp-option-p port's Linux release (engine/tools/linux/make_toolchain_bundle.sh).

bin/clang++, bin/ld.lld, lib/clang, include/c++, lib/x86_64-unknown-linux-gnu (libc++, libc++abi, libunwind): LLVM
  $VER_MAJOR, the official release $(basename "$LLVM_TAR"), unchanged except stripped - LLVM.txt (Apache-2.0 with LLVM
  exceptions).
sysroot/: GNU C library 2.35, Linux kernel headers, GCC 11 startup files / libgcc from Ubuntu 22.04 packages -
  libc6*.copyright.txt (LGPL-2.1+), linux-libc-dev.copyright.txt (GPL-2 headers with the syscall note),
  libgcc-11-dev.copyright.txt (GPL-3 with the GCC Runtime Library Exception).
lib/libstdc++.so.6, lib/libgcc_s.so.1: the GCC 12 runtime from Ubuntu 22.04, loaded by clang and lld only -
  libstdc++6 / libgcc-s1 copyright files (GPL-3 with the GCC Runtime Library Exception).
lib/libicu*.so.70 (if present): NOT ICU - generated stubs that answer lld's libxml2 with an error.
sysroot/usr/lib/x86_64-linux-gnu/libvulkan.so: NOT the Vulkan loader - a generated link stub, never loaded.
EOF

# ── smoke test ──────────────────────────────────────────────────────────────
echo "== smoke"
cat > "$TMP/smoke.cpp" <<'EOF'
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" void vkGetInstanceProcAddr(void);
int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "vk")) vkGetInstanceProcAddr();
    std::string got;
    std::thread t([&] { try { throw std::runtime_error("thrown"); } catch (const std::exception &e) { got = e.what(); } });
    t.join();
    std::vector<double> v{std::sqrt(2.0)};
    std::printf("smoke ok %.3f %s %s\n", v[0], got.c_str(), dlsym(RTLD_DEFAULT, "printf") ? "dl" : "NO-DL");
    return 0;
}
EOF
"$L/bin/clang++" --sysroot="$S" -fuse-ld=lld -O2 -stdlib=libc++ -o "$TMP/smoke" "$TMP/smoke.cpp" \
    -static-libstdc++ -nostdlib++ "$L/lib/x86_64-unknown-linux-gnu/libc++.a" "$L/lib/x86_64-unknown-linux-gnu/libc++abi.a" \
    "$L/lib/x86_64-unknown-linux-gnu/libunwind.a" -lpthread -ldl -lm -lvulkan
"$TMP/smoke"
max="$(objdump -T "$TMP/smoke" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1)"
echo "  highest glibc symbol version: $max"
case "$max" in
    GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|GLIBC_2.3[0-5]) ;;
    *) echo "  FAIL: needs more than glibc 2.35" >&2; exit 1 ;;
esac
if readelf -d "$TMP/smoke" | grep -qE 'NEEDED.*(libstdc\+\+|libc\+\+)'; then echo "  FAIL: a shared C++ runtime" >&2; exit 1; fi
[ -z "$(find "$OUT" -path "$TMP" -prune -o -type l -print)" ] || { echo "  FAIL: links left" >&2; exit 1; }

echo "== sizes (MB)"
du -sm "$L/bin" "$L/$RT" "$L/include" "$S" "$L" "$P" | sed "s|$OUT/||"
echo "done: $OUT"
