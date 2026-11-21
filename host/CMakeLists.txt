# The native host: the shared runtime (PS2 instruction semantics, also used by
# verification) and the PC platform layer that serves the translated code's
# hooks. A game port adds its translated functions with ps2_add_port().

cmake_minimum_required(VERSION 3.20)

set(PS2_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(ps2rt STATIC
    ${PS2_ENGINE_DIR}/runtime/src/eefpu.cpp
    ${PS2_ENGINE_DIR}/runtime/src/eeint.cpp
    ${PS2_ENGINE_DIR}/runtime/src/vu0.cpp
    ${PS2_ENGINE_DIR}/runtime/src/lifted_support.cpp)
target_include_directories(ps2rt PUBLIC ${PS2_ENGINE_DIR}/runtime/include)
target_compile_features(ps2rt PUBLIC cxx_std_17)

add_library(ps2host STATIC
    ${CMAKE_CURRENT_LIST_DIR}/src/host.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/kernel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ipu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mpeg2_video.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vif1_worker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/threads.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sdk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/crash_report.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/disc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/cdvd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/fileio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vif.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/dma.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/loadfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/dbc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/input_bind.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mc2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mcserv.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop_vm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop_libs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iop_inet.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/iopheap.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/spu2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/music.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/vu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/timers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/pc_menu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/timeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_thread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_draw.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gs_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gpu_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/boot_video.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/profile.cpp)
# CHD disc images (disc.cpp): libchdr (BSD-3-Clause; with lzma - public domain, miniz - MIT, zstd - BSD), vendored
# unchanged from rtissera/libchdr 47d794151b2b (third_party/libchdr, its LICENSE.txt; LICENSES/libchdr-BSD-3.txt).
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_VERIFY_BLOCK_CRC OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)          # (C only: no assembler needed on any toolchain)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/libchdr ${CMAKE_BINARY_DIR}/libchdr EXCLUDE_FROM_ALL)
target_link_libraries(ps2host PRIVATE chdr-static)
target_include_directories(ps2host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party/libchdr/include)

# The boot animation's MPEG-1 decoder: pl_mpeg (MIT, vendored unchanged: third_party/pl_mpeg, LICENSES/).
target_include_directories(ps2host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party)
# The IOP interpreter names library functions from engine/data/iop_exports.csv,
# built in (iop_exports.inc: a line of it a string), so that the executable
# reads nothing from where it was built.
file(STRINGS ${PS2_ENGINE_DIR}/data/iop_exports.csv iop_export_lines)
set(iop_exports_inc "// From engine/data/iop_exports.csv (engine/host/CMakeLists.txt): generated.\n")
foreach(line IN LISTS iop_export_lines)
    string(REPLACE "\\" "\\\\" line "${line}")
    string(REPLACE "\"" "\\\"" line "${line}")
    string(APPEND iop_exports_inc "\"${line}\\n\"\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc.new "${iop_exports_inc}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc.new
               ${CMAKE_CURRENT_BINARY_DIR}/generated/iop_exports.inc COPYONLY)     # (touched only when it changes)
target_include_directories(ps2host PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PS2_ENGINE_DIR}/data/iop_exports.csv)
target_include_directories(ps2host PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(ps2host PUBLIC ps2rt)
# The operating system under the host (engine/platform: fibers, timers, threads, paths, dialogs, keys).
if(NOT TARGET ps2platform)
    add_subdirectory(${PS2_ENGINE_DIR}/platform ${CMAKE_BINARY_DIR}/ps2platform)
endif()
target_link_libraries(ps2host PUBLIC ps2platform)

# Texture packs (texture_pack.cpp): single headers, pinned by hash - xxHash
# (BSD-2-Clause; PCSX2's names are XXH3 hashes), bcdec (MIT or Unlicense; DDS
# BC1-BC7) and stb_image (MIT or public domain; PNG). And for the host's music
# (music.cpp), minimp3 (CC0: public domain; MP3).
set(third_party_dir ${CMAKE_CURRENT_BINARY_DIR}/third_party)
foreach(entry
        "xxhash.h|https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h|17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b"
        "bcdec.h|https://raw.githubusercontent.com/iOrange/bcdec/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h|134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189"
        "stb_image.h|https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h|594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"
        "minimp3.h|https://raw.githubusercontent.com/lieff/minimp3/afb604c06bc8beb145fecd42c0ceb5bda8795144/minimp3.h|5fb296a790734772b65a5514438cc06d14f24e539c13475ff2a7f737681c82c0"
        "stb_truetype.h|https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h|ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    list(GET parts 2 sha)
    if(NOT EXISTS ${third_party_dir}/${name})
        file(DOWNLOAD ${url} ${third_party_dir}/${name} EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
    endif()
endforeach()
target_include_directories(ps2host PRIVATE ${third_party_dir})
target_sources(ps2host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/texture_pack.cpp)

# A port's own menu lines in the game's picture (game_text.cpp): Liberation Sans Bold (SIL Open Font License 1.1,
# engine/data/fonts, LICENSES/LiberationFonts-OFL.txt) built in as bytes, rasterised by stb_truetype (above).
set(font_file ${PS2_ENGINE_DIR}/data/fonts/LiberationSans-Bold.ttf)
set(font_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/liberation_sans_bold.inc)
if(NOT EXISTS ${font_inc} OR ${font_file} IS_NEWER_THAN ${font_inc})
    file(READ ${font_file} font_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_bytes "${font_hex}")
    file(WRITE ${font_inc} "// From engine/data/fonts/LiberationSans-Bold.ttf (engine/host/CMakeLists.txt): generated.\n${font_bytes}\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${font_file})
target_sources(ps2host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/game_text.cpp)

# The window through SDL3 and Vulkan (engine/gpu), with GDI as the fallback.
option(PS2HOST_VULKAN "Present the picture through SDL3 and Vulkan" ON)
if(PS2HOST_VULKAN)
    add_subdirectory(${PS2_ENGINE_DIR}/gpu ${CMAKE_BINARY_DIR}/ps2gpu)
    target_link_libraries(ps2host PUBLIC ps2gpu)
    target_compile_definitions(ps2host PRIVATE PS2HOST_VULKAN=1)
endif()

# Tier E (Option P's exact reference, docs/option_p/02_design.md): the software GS rasteriser and the GPU
# renderer's shadow modes, the VU and IOP interpreters, the tier-E switches of the GPU renderer's tier N
# paths. OFF: none of it in the build - the shipped port's (the maintainer's rule: no emulator parts in a
# shipped port); every microprogram and IOP module must be compiled, the GPU draws.
option(PS2HOST_TIER_E "Build tier E (the exact reference) in; OFF for a shipped port" ON)
if(NOT PS2HOST_TIER_E)
    target_compile_definitions(ps2host PUBLIC PS2HOST_NO_TIER_E=1)
    if(PS2HOST_VULKAN)
        target_compile_definitions(ps2gpu PRIVATE PS2HOST_NO_TIER_E=1)
    endif()
endif()

# The F9 debug panel (pc_menu.cpp): internal only (the maintainer 2026-11-20) - OFF in a player's build (port.py's
# llvm-mingw build, the release toolchain, unless PS2_DEBUG_PANEL=on); the player's settings are the game's own
# RECOMP OPTIONS page.
option(PS2HOST_DEBUG_PANEL "Build the F9 debug panel in; OFF for a player's build" ON)
if(NOT PS2HOST_DEBUG_PANEL)
    target_compile_definitions(ps2host PRIVATE PS2HOST_NO_DEBUG_PANEL=1)
endif()

# AVX2: three-operand SIMD for the VU's lanes and the rest (6% of a race).
# Off: a build for processors without it (Intel's Pentium and Celeron of
# before 2021), SSE4.1 at least - a port ships both, the AVX2 one starting
# the other where it must (src/cpu_check.cpp).
option(PS2HOST_AVX2 "Build for processors with AVX2 (off: SSE4.1)" ON)
# A game executable's entry (ps2_add_port's /ENTRY), before the runtime: the
# processor checked by code built without AVX - a library of its own, apart
# from ps2rt's flags - and the SSE4 build started instead where it must be.
if(MSVC)
    add_library(ps2_cpu_check STATIC ${CMAKE_CURRENT_LIST_DIR}/src/cpu_check.cpp)
    if(PS2HOST_AVX2)
        target_compile_definitions(ps2_cpu_check PRIVATE PS2HOST_BUILT_FOR_AVX2=1)
    endif()
endif()

if(MSVC)
    # Translated code throws lifted::Abort; its chunks are large.
    target_compile_options(ps2rt PUBLIC /EHsc /bigobj /W3 /wd4100 /wd4189)
    if(PS2HOST_AVX2)
        target_compile_options(ps2rt PUBLIC /arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # MSVC takes SSE4.1's intrinsics without a flag; clang-cl needs the
        # floor named (the one cpu_check.cpp holds the SSE4 build to).
        target_compile_options(ps2rt PUBLIC /clang:-msse4.1)
    endif()
    target_compile_definitions(ps2rt PUBLIC _CRT_SECURE_NO_WARNINGS)
    # PS2HOST_PGO (clang-cl): "generate" - a build that counts what runs
    # (LLVM_PROFILE_FILE names where it writes them) - or the .profdata
    # merged from such runs (llvm-profdata), to build with what they ran
    # (engine/tools/port.py's pgo).
    set(PS2HOST_PGO "" CACHE STRING "Profile-guided build: 'generate', or the .profdata to build with")
    if(PS2HOST_PGO AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if(PS2HOST_PGO STREQUAL "generate")
            target_compile_options(ps2rt PUBLIC /clang:-fprofile-instr-generate)
            target_compile_definitions(ps2rt PUBLIC PS2HOST_PGO_GENERATE=1)    # main.cpp writes the profile
            get_filename_component(llvm_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
            file(GLOB profile_runtime ${llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib)
            if(NOT profile_runtime)
                message(FATAL_ERROR "PS2HOST_PGO=generate: LLVM's clang_rt.profile-x86_64.lib not found")
            endif()
            list(GET profile_runtime 0 profile_runtime)
            target_link_libraries(ps2rt PUBLIC ${profile_runtime})
        else()
            # The profile's time in the flags: a new profile builds everything
            # again (Ninja does not follow the file - objects from the last
            # one beside the new one's would not link with ThinLTO).
            file(TIMESTAMP ${PS2HOST_PGO} pgo_time "%Y%m%d%H%M%S")
            target_compile_definitions(ps2rt PUBLIC PS2HOST_PGO_TIME=${pgo_time})
            target_compile_options(ps2rt PUBLIC "/clang:-fprofile-instr-use=${PS2HOST_PGO}"
                                   /clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date
                                   /clang:-Wno-backend-plugin)
        endif()
    endif()
    # PS2HOST_LTO (clang-cl, lld-link): ThinLTO - the host's functions the
    # game's code calls (memory, the dispatch, the VU's) inlined across files.
    option(PS2HOST_LTO "ThinLTO (clang-cl and lld-link)" OFF)
    if(PS2HOST_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(ps2rt PUBLIC /clang:-flto=thin)
        target_link_options(ps2rt PUBLIC /opt:lldltojobs=all)
    endif()
    target_compile_options(ps2host PRIVATE /Z7)     # symbols for --sample, as the ports'
    target_compile_options(ps2rt PRIVATE /Z7)
    target_link_libraries(ps2host PRIVATE dbghelp winmm)  # ...which resolves them; 1 ms sleeps
elseif(MINGW)
    # The release toolchain (llvm-mingw, toolchain_comparison.md 5): the player's PC builds the port with it.
    # The same instruction floors as the MSVC build: AVX2 (with FMA, BMI as /arch:AVX2 implies), or SSE4.1.
    if(PS2HOST_AVX2)
        target_compile_options(ps2rt PUBLIC -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mf16c -mpopcnt)
    else()
        target_compile_options(ps2rt PUBLIC -msse4.1 -mpopcnt)
    endif()
    # Windows 10's API (SetThreadInformation / SetProcessInformation: the power throttling the host turns off).
    target_compile_definitions(ps2rt PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ps2rt PUBLIC -Wno-unused-parameter -Wno-unused-variable)
    # (MSVC takes these from #pragma comment(lib); MinGW's linker wants them named.)
    target_link_libraries(ps2host PRIVATE dbghelp winmm synchronization)
else()
    # Another system (Linux, Android: GCC or Clang; not built yet - docs/option_p/portability_handoff.md). The
    # instruction floors by processor: x86-64 as the MinGW build's; AArch64 has NEON in its base (armv8-a), the
    # host's SIMD paths taking their portable forms there (the hand-off's NEON list).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        if(PS2HOST_AVX2)
            target_compile_options(ps2rt PUBLIC -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mf16c -mpopcnt)
        else()
            target_compile_options(ps2rt PUBLIC -msse4.1 -mpopcnt)
        endif()
    endif()
    target_compile_options(ps2rt PUBLIC -Wno-unused-parameter -Wno-unused-variable)
    find_package(Threads REQUIRED)
    target_link_libraries(ps2host PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(ps2host PRIVATE ws2_32)         # the IOP's inet (iop_inet.cpp)
endif()

# The host's own tests, which need no game.
# vu_corpus.cpp: random microprograms, compiled by engine/tools/vurecomp.py
# --corpus as the build goes, which VuCompiledIsTheInterpreter runs both ways.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PS2HOST_VURECOMP ${CMAKE_CURRENT_LIST_DIR}/../tools/vurecomp.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp
    COMMAND ${Python3_EXECUTABLE} ${PS2HOST_VURECOMP} --corpus --count 120 --seed 1
            --out ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp
    DEPENDS ${PS2HOST_VURECOMP}
    COMMENT "vurecomp: the VU test corpus")
# iop_corpus.cpp: random IOP programs compiled by engine/tools/ioprecomp.py --corpus (IopCompiledIsTheInterpreter).
set(PS2HOST_IOPRECOMP ${CMAKE_CURRENT_LIST_DIR}/../tools/ioprecomp.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/iop_corpus.cpp
    COMMAND ${Python3_EXECUTABLE} ${PS2HOST_IOPRECOMP} --corpus --count 60 --seed 1
            --out ${CMAKE_CURRENT_BINARY_DIR}/iop_corpus.cpp
    DEPENDS ${PS2HOST_IOPRECOMP}
    COMMENT "ioprecomp: the IOP test corpus")
# (The host's tests check the compiled paths against tier E: built with it only.)
if(PS2HOST_TIER_E)
add_executable(ps2host_tests ${CMAKE_CURRENT_LIST_DIR}/tests/host_tests.cpp
                             ${CMAKE_CURRENT_BINARY_DIR}/vu_corpus.cpp
                             ${CMAKE_CURRENT_BINARY_DIR}/iop_corpus.cpp)
target_link_libraries(ps2host_tests PRIVATE ps2host)
if(PS2HOST_VULKAN)
    target_compile_definitions(ps2host_tests PRIVATE PS2HOST_VULKAN=1)
endif()
# The IOP interpreter's internals, for its instruction tests (and the corpus's compiled code).
target_include_directories(ps2host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
endif()

# ps2_add_port(<target>
#     GENERATED <dir with dispatch.cpp from engine/tools/port.py>
#     TRANSLATED <dir with the game's translated sources (src/lifted)>
#     SOURCES <the game's own sources: overrides, enhancements>
#     GAME <the game's directory, with game.json>)
# With GAME, the port started without arguments (from Explorer, say) plays the
# game from the player's own disc image: asked for the first time (and checked
# to be the disc game.json's executable comes from), remembered after. And the
# executable carries the game's icon (the window's and the taskbar's too): the
# port's own, port/icon.ico - or build/port/icon.ico where a player made one
# for their own build (from their disc: a game's port/tools/make_icon.py). The
# game's project needs the RC language for it.
function(ps2_add_port target)
    cmake_parse_arguments(PORT "" "GENERATED;TRANSLATED;GAME" "SOURCES" ${ARGN})
    # CONFIGURE_DEPENDS: the build checks the lists again before it starts, so sources written or removed after
    # the configure (a new lift, a bring-up round's discovered_NN.cpp) never leave it with an old list.
    file(GLOB translated CONFIGURE_DEPENDS ${PORT_TRANSLATED}/*.cpp)
    # dispatch.cpp, and discovered_NN.cpp: code found at run time, translated (globbed at each configure).
    file(GLOB generated CONFIGURE_DEPENDS ${PORT_GENERATED}/*.cpp)
    # Without them the port links without its dispatch table (undefined kGuestFunctions - seen twice on a first
    # rebuild after an engine change, 2026-11-14/15, cause not caught): stop here and say what was looked at.
    if(NOT generated OR NOT EXISTS ${PORT_GENERATED}/dispatch.cpp)
        message(FATAL_ERROR "ps2_add_port(${target}): no dispatch.cpp in GENERATED '${PORT_GENERATED}' "
                            "(PS2_GENERATED_DIR '${PS2_GENERATED_DIR}'; ${CMAKE_CURRENT_LIST_DIR}) - run engine/tools/port.py")
    endif()
    add_executable(${target}
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/main.cpp
        ${generated}
        ${translated}
        ${PORT_SOURCES})
    if(MSVC)
        target_link_libraries(${target} PRIVATE ps2_cpu_check)
    endif()
    target_link_libraries(${target} PRIVATE ps2host)
    # iop_modules.cpp / iop_code_N.cpp (ioprecomp.py) are the IOP interpreter's steps compiled: its internals.
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src)
    if(PORT_GAME)
        get_filename_component(game_dir ${PORT_GAME} ABSOLUTE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${game_dir}/game.json)
        file(READ ${game_dir}/game.json game_json)
        string(JSON game_name GET ${game_json} name)
        string(JSON game_executable GET ${game_json} executable)
        string(JSON game_title ERROR_VARIABLE no_title GET ${game_json} title)
        if(no_title)
            set(game_title ${game_name})
        endif()
        string(JSON game_version ERROR_VARIABLE no_version GET ${game_json} version)
        if(no_version)
            set(game_version "0.0.0")
        endif()
        # The crash report's (crash_report.cpp): its zip's name (the target) and the GitHub repository a report goes
        # to (game.json "release": {"github": "owner/repository"}; none: no GitHub button).
        string(JSON game_github ERROR_VARIABLE no_github GET ${game_json} release github)
        if(no_github)
            set(game_github "")
        endif()
        target_compile_definitions(${target} PRIVATE PS2HOST_GAME_NAME="${game_name}"
                                                     PS2HOST_GAME_TITLE="${game_title}"
                                                     PS2HOST_GAME_VERSION="${game_version}"
                                                     PS2HOST_GAME_EXECUTABLE="${game_executable}"
                                                     PS2HOST_GAME_TARGET="${target}"
                                                     PS2HOST_GAME_GITHUB="${game_github}")
        # (The disc's file dialog: engine/platform's PickFile, which links what it needs.)
        set(icon ${game_dir}/build/port/icon.ico)
        if(NOT EXISTS ${icon})
            set(icon ${game_dir}/port/icon.ico)
        endif()
        if(WIN32 AND EXISTS ${icon} AND CMAKE_RC_COMPILER)
            # The icon, and the version Explorer shows (game.json's "version",
            # its numbers for the fixed fields: 0.1.0-alpha is 0,1,0,0).
            string(REGEX MATCHALL "[0-9]+" version_numbers "${game_version}")
            list(APPEND version_numbers 0 0 0 0)
            list(SUBLIST version_numbers 0 4 version_numbers)
            string(REPLACE ";" "," version_fixed "${version_numbers}")
            file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc
                 CONTENT "1 ICON \"${icon}\"
1 VERSIONINFO
FILEVERSION ${version_fixed}
PRODUCTVERSION ${version_fixed}
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"${game_title} - PC port\"
      VALUE \"ProductName\", \"${game_title} - PC port\"
      VALUE \"FileVersion\", \"${game_version}\"
      VALUE \"ProductVersion\", \"${game_version}\"
      VALUE \"OriginalFilename\", \"${target}.exe\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
            target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc)
            set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc PROPERTIES
                                        OBJECT_DEPENDS ${icon})
        endif()
        if(MSVC)
            # A windowed program: started from Explorer, no console beside the
            # game (main.cpp attaches to a terminal's when there is one).
            target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:Ps2CheckedStart)
        elseif(MINGW)
            # The same with llvm-mingw (the release toolchain): windowed, and one file - its C++ runtime linked in,
            # no DLL beside it. (No processor check before the runtime here: main.cpp's, after it.)
            target_link_options(${target} PRIVATE -mwindows -static)
        endif()
    endif()
    if(MSVC)
        # Symbols in the optimised build too, for --sample (profile.cpp): /Z7
        # keeps them in the objects (no PDB contention in parallel builds),
        # /DEBUG gathers them; /OPT keeps what it does without symbols.
        target_compile_options(${target} PRIVATE /Z7)
        # The symbols' file named by its name alone in the executable, not the
        # path it was built at (a release carries no one's folders).
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
    endif()
endfunction()
