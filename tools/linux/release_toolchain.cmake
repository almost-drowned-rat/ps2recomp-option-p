# The Linux release build's toolchain (agent L, 2026-11-21): the engine's libraries, the port's own objects and the
# launcher compiled as the player's first start compiles the translated code - the same clang (the official LLVM
# release make_toolchain_bundle.sh takes its bundle from), the same libc++ headers, glibc 2.35 (Ubuntu 22.04) - so a
# release runs on every distribution with glibc 2.35 or newer and asks the player's system for no C++ runtime.
#
#   cmake -S games/<name>/port -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=engine/tools/linux/release_toolchain.cmake \
#         -DPS2_LINUX_LLVM=<the official LLVM release, unpacked> -DPS2_LINUX_SYSROOT=<the bundle's sysroot plus the
#         -dev headers SDL3 is built against (X11, Wayland, PulseAudio, PipeWire, ALSA, D-Bus, udev)> ...
#
# (SDL3 loads every one of those libraries at run time: their headers are needed to build it, nothing to link.)

# (No CMAKE_SYSTEM_NAME: the build machine is x86-64 Linux too - set, CMake would take the build for a cross build
# and not run the tools it builds itself, glslang's for the shaders.)

if(NOT PS2_LINUX_LLVM OR NOT PS2_LINUX_SYSROOT)
    message(FATAL_ERROR "release_toolchain.cmake: -DPS2_LINUX_LLVM=<LLVM release folder> -DPS2_LINUX_SYSROOT=<sysroot>")
endif()
# (try_compile projects see these too)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS2_LINUX_LLVM PS2_LINUX_SYSROOT)

set(CMAKE_C_COMPILER ${PS2_LINUX_LLVM}/bin/clang)
set(CMAKE_CXX_COMPILER ${PS2_LINUX_LLVM}/bin/clang++)
set(CMAKE_ASM_COMPILER ${PS2_LINUX_LLVM}/bin/clang)
set(CMAKE_AR ${PS2_LINUX_LLVM}/bin/llvm-ar)
set(CMAKE_RANLIB ${PS2_LINUX_LLVM}/bin/llvm-ranlib)
set(CMAKE_SYSROOT ${PS2_LINUX_SYSROOT})
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-linux-gnu)

set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
# libc++, libc++abi and libunwind linked in (the LLVM release's static libraries), libgcc too: the game needs only
# glibc and the system's Vulkan loader / SDL's libraries (loaded at run time)
set(ps2_linux_cxx_libs
    "-nostdlib++ ${PS2_LINUX_LLVM}/lib/x86_64-unknown-linux-gnu/libc++.a ${PS2_LINUX_LLVM}/lib/x86_64-unknown-linux-gnu/libc++abi.a ${PS2_LINUX_LLVM}/lib/x86_64-unknown-linux-gnu/libunwind.a")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libgcc")
set(CMAKE_CXX_STANDARD_LIBRARIES "${ps2_linux_cxx_libs} -lpthread -ldl -lm")

# pkg-config (SDL3's checks) looks into the sysroot only
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${PS2_LINUX_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} "${PS2_LINUX_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${PS2_LINUX_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

set(CMAKE_FIND_ROOT_PATH ${PS2_LINUX_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)        # the build machine's tools (python, glslang, wayland-scanner)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
