# The release launcher (launcher.h): one small exe, SDL3 only, built with llvm-mingw (static: it runs before
# anything of the port is there). A port's release script builds it with its names:
#   cmake -S engine/launcher -B <dir> -G Ninja -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang
#         -DPS2_LAUNCHER_GAME="Zone of the Enders" -DPS2_LAUNCHER_DISC="..." -DPS2_LAUNCHER_GAME_EXE=ZOE.exe
#         [-DPS2_LAUNCHER_SDL3=<an SDL3 source folder>]
cmake_minimum_required(VERSION 3.20)
project(ps2_launcher LANGUAGES C)

set(PS2_LAUNCHER_GAME "the game" CACHE STRING "the game's name in the messages")
set(PS2_LAUNCHER_DISC "" CACHE STRING "the disc the player provides")
set(PS2_LAUNCHER_TITLE "" CACHE STRING "the window title")
set(PS2_LAUNCHER_GAME_EXE "game.exe" CACHE STRING "the game's exe, built beside the launcher")
set(PS2_LAUNCHER_OUTPUT "launcher" CACHE STRING "the launcher's exe name (without .exe)")
set(PS2_LAUNCHER_SDL3 "" CACHE PATH "an SDL3 source folder (empty: fetched as the engine's gpu/ does)")
set(PS2_LAUNCHER_EXTRA_DISC "" CACHE STRING "an optional second disc's line label on the setup page")
set(PS2_LAUNCHER_EXTRA_KEY "" CACHE STRING "its pc_settings.ini key")
set(PS2_LAUNCHER_ICON "" CACHE FILEPATH "a BMP (32-bit with alpha, 64x64) for the setup window's icon (empty: none)")
set(PS2_LAUNCHER_ICO "" CACHE FILEPATH "Windows: an .ico for the launcher exe (Explorer, task bar, the window's title bar)")

if(PS2_LAUNCHER_SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${PS2_LAUNCHER_SDL3} ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    # the engine's own SDL3 (gpu/CMakeLists.txt)
    FetchContent_Declare(sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.18
                         GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(sdl3)
endif()

# The window: Windows 95's look on Windows (plain Win32 + GDI, game_setup_ui_win95.c - the maintainer's rule for
# the ports' launchers), SDL3's elsewhere.
if(WIN32)
    set(setup_ui ${CMAKE_CURRENT_LIST_DIR}/game_setup_ui_win95.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux (agent L, 2026-11-21): the same Windows 95 window drawn with SDL3 (game_setup_ui_win95_sdl.c) - its
    # font Liberation Sans (engine/data/fonts, SIL OFL 1.1) by stb_truetype, both built in here.
    set(setup_ui ${CMAKE_CURRENT_LIST_DIR}/game_setup_ui_win95_sdl.c)
    set(launcher_gen ${CMAKE_CURRENT_BINARY_DIR}/launcher_generated)
    if(NOT EXISTS ${launcher_gen}/stb_truetype.h)
        file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h
             ${launcher_gen}/stb_truetype.h
             EXPECTED_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab TLS_VERIFY ON)
    endif()
    foreach(face Regular Bold)
        string(TOLOWER ${face} face_lower)
        set(font_file ${CMAKE_CURRENT_LIST_DIR}/../data/fonts/LiberationSans-${face}.ttf)
        set(font_inc ${launcher_gen}/liberation_sans_${face_lower}.inc)
        if(NOT EXISTS ${font_inc} OR ${font_file} IS_NEWER_THAN ${font_inc})
            file(READ ${font_file} font_hex HEX)
            string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_bytes "${font_hex}")
            file(WRITE ${font_inc} "// From engine/data/fonts/LiberationSans-${face}.ttf (engine/launcher): generated.\n${font_bytes}\n")
        endif()
    endforeach()
else()
    set(setup_ui ${CMAKE_CURRENT_LIST_DIR}/game_setup_ui.c)
endif()
add_executable(ps2_launcher WIN32
    ${CMAKE_CURRENT_LIST_DIR}/launcher_main.c
    ${CMAKE_CURRENT_LIST_DIR}/launcher.c
    ${setup_ui}
    ${CMAKE_CURRENT_LIST_DIR}/sha256.c
    ${CMAKE_CURRENT_LIST_DIR}/zip_read.c)
# CHD disc images (the build reads them as an .iso, launcher.c chd_to_iso): libchdr (BSD-3-Clause, the engine's copy,
# host/third_party/libchdr) - its miniz (the same 11.3.2 as third_party/miniz's header) gives zip_read its inflate.
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_VERIFY_BLOCK_CRC OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)          # (C only: no assembler needed on any toolchain)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../host/third_party/libchdr ${CMAKE_BINARY_DIR}/libchdr EXCLUDE_FROM_ALL)
target_link_libraries(ps2_launcher PRIVATE chdr-static)
target_include_directories(ps2_launcher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../host/third_party/libchdr/include)
target_include_directories(ps2_launcher PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/third_party/miniz
                                                ${CMAKE_CURRENT_BINARY_DIR})
if(launcher_gen)
    target_include_directories(ps2_launcher PRIVATE ${launcher_gen})
endif()
# miniz (3.1.2, MIT, LICENSES/miniz-MIT.txt): only its inflate; miniz_export.h, which its own build makes, empty
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h "#define MINIZ_EXPORT\n")
target_compile_definitions(ps2_launcher PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
                           MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(ps2_launcher PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(ps2_launcher PRIVATE comdlg32 gdi32 user32 shell32 ole32)
endif()
set(defs PS2_LAUNCHER_GAME="${PS2_LAUNCHER_GAME}" PS2_LAUNCHER_GAME_EXE="${PS2_LAUNCHER_GAME_EXE}")
if(PS2_LAUNCHER_DISC)
    list(APPEND defs PS2_LAUNCHER_DISC="${PS2_LAUNCHER_DISC}")
endif()
if(PS2_LAUNCHER_TITLE)
    list(APPEND defs PS2_LAUNCHER_TITLE="${PS2_LAUNCHER_TITLE}")
endif()
# An optional second disc's line on the setup page (ZOE: -DPS2_LAUNCHER_EXTRA_DISC="Japanese disc (optional)"
# -DPS2_LAUNCHER_EXTRA_KEY=disc_ja): its label and its pc_settings.ini key
if(PS2_LAUNCHER_EXTRA_DISC AND PS2_LAUNCHER_EXTRA_KEY)
    list(APPEND defs PS2_LAUNCHER_EXTRA_DISC="${PS2_LAUNCHER_EXTRA_DISC}" PS2_LAUNCHER_EXTRA_KEY="${PS2_LAUNCHER_EXTRA_KEY}")
endif()
# An optional file of the port's own on the setup page, copied beside the game (ZOE: its subtitle pack -
# -DPS2_LAUNCHER_FILE_LABEL="Subtitles (optional):" -DPS2_LAUNCHER_FILE_FILTER_NAME="Subtitle pack (*.pak)"
# -DPS2_LAUNCHER_FILE_FILTER=*.pak -DPS2_LAUNCHER_FILE_NAME=ZOE-subtitles.pak)
if(PS2_LAUNCHER_FILE_LABEL AND PS2_LAUNCHER_FILE_NAME)
    list(APPEND defs PS2_LAUNCHER_FILE_LABEL="${PS2_LAUNCHER_FILE_LABEL}"
         PS2_LAUNCHER_FILE_FILTER_NAME="${PS2_LAUNCHER_FILE_FILTER_NAME}" PS2_LAUNCHER_FILE_FILTER="${PS2_LAUNCHER_FILE_FILTER}"
         PS2_LAUNCHER_FILE_NAME="${PS2_LAUNCHER_FILE_NAME}")
endif()
# The setup window's icon (title bar, task bar): the BMP built in as launcher_icon.inc
if(PS2_LAUNCHER_ICON)
    set(icon_inc ${CMAKE_CURRENT_BINARY_DIR}/launcher_icon.inc)
    file(READ ${PS2_LAUNCHER_ICON} icon_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
    file(WRITE ${icon_inc} "// From ${PS2_LAUNCHER_ICON} (engine/launcher): generated.\n${icon_bytes}\n")
    list(APPEND defs PS2_LAUNCHER_ICON_INC=1)
endif()
target_compile_definitions(ps2_launcher PRIVATE ${defs})
set_target_properties(ps2_launcher PROPERTIES OUTPUT_NAME ${PS2_LAUNCHER_OUTPUT})
# Windows: the exe's icon as resource 1 (game_setup_ui_win95.c loads resource 1 for the window class and the
# title bar) - the game's port/icon.ico, the same the game exe carries (ZOE v0.8.4, the maintainer's wish).
if(WIN32 AND PS2_LAUNCHER_ICO)
    enable_language(RC)
    file(TO_CMAKE_PATH "${PS2_LAUNCHER_ICO}" launcher_ico)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/launcher_icon.rc "1 ICON \"${launcher_ico}\"\n")
    target_sources(ps2_launcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/launcher_icon.rc)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/launcher_icon.rc PROPERTIES OBJECT_DEPENDS ${launcher_ico})
endif()
if(MINGW)
    target_link_options(ps2_launcher PRIVATE -static)
endif()
