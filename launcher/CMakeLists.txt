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
else()
    set(setup_ui ${CMAKE_CURRENT_LIST_DIR}/game_setup_ui.c)
endif()
add_executable(ps2_launcher WIN32
    ${CMAKE_CURRENT_LIST_DIR}/launcher_main.c
    ${CMAKE_CURRENT_LIST_DIR}/launcher.c
    ${setup_ui}
    ${CMAKE_CURRENT_LIST_DIR}/sha256.c
    ${CMAKE_CURRENT_LIST_DIR}/zip_read.c
    ${CMAKE_CURRENT_LIST_DIR}/third_party/miniz/miniz_tinfl.c)
target_include_directories(ps2_launcher PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/third_party/miniz
                                                ${CMAKE_CURRENT_BINARY_DIR})
# miniz (3.1.2, MIT, LICENSES/miniz-MIT.txt): only its inflate; miniz_export.h, which its own build makes, empty
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h "#define MINIZ_EXPORT\n")
target_compile_definitions(ps2_launcher PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
                           MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/third_party/miniz/miniz_tinfl.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(ps2_launcher PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(ps2_launcher PRIVATE comdlg32 gdi32 user32)
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
target_compile_definitions(ps2_launcher PRIVATE ${defs})
set_target_properties(ps2_launcher PROPERTIES OUTPUT_NAME ${PS2_LAUNCHER_OUTPUT})
if(MINGW)
    target_link_options(ps2_launcher PRIVATE -static)
endif()
