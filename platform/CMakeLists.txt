# The platform layer (include/ps2platform): what the host and the GPU display need of the operating system
# beyond standard C++ and SDL3. One source per system; Windows is the only one so far
# (docs/option_p/portability_handoff.md: what another system's file implements, and how).
# Included by engine/host and engine/gpu, whichever comes first (the GPU's tests build without the host).

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET ps2platform)
    if(WIN32)
        add_library(ps2platform STATIC ${CMAKE_CURRENT_LIST_DIR}/src/platform_win32.cpp
                                       ${CMAKE_CURRENT_LIST_DIR}/src/audio_win32.cpp)
        # Windows 10's API (SetThreadInformation / SetProcessInformation: the power throttling turned off,
        # high-resolution waitable timers); WaitOnAddress, timeBeginPeriod, the open-file dialog.
        target_compile_definitions(ps2platform PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(ps2platform PRIVATE synchronization winmm comdlg32 gdi32 user32 ole32 advapi32)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux (agent L, 2026-11-21): fibers by a register switch, futex waits, SDL3's dialogs and sound; the
        # overlay's text by stb_truetype with the engine's Liberation Sans Bold (built in, as engine/host's).
        add_library(ps2platform STATIC ${CMAKE_CURRENT_LIST_DIR}/src/platform_linux.cpp
                                       ${CMAKE_CURRENT_LIST_DIR}/src/audio_sdl.cpp)
        set(ps2platform_gen ${CMAKE_CURRENT_BINARY_DIR}/ps2platform_generated)
        if(NOT EXISTS ${ps2platform_gen}/stb_truetype.h)
            file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h
                 ${ps2platform_gen}/stb_truetype.h
                 EXPECTED_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab TLS_VERIFY ON)
        endif()
        set(ps2platform_font ${CMAKE_CURRENT_LIST_DIR}/../data/fonts/LiberationSans-Bold.ttf)
        if(NOT EXISTS ${ps2platform_gen}/liberation_sans_bold.inc OR ${ps2platform_font} IS_NEWER_THAN
           ${ps2platform_gen}/liberation_sans_bold.inc)
            file(READ ${ps2platform_font} font_hex HEX)
            string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_bytes "${font_hex}")
            file(WRITE ${ps2platform_gen}/liberation_sans_bold.inc
                 "// From engine/data/fonts/LiberationSans-Bold.ttf (engine/platform/CMakeLists.txt): generated.\n${font_bytes}\n")
        endif()
        target_include_directories(ps2platform PRIVATE ${ps2platform_gen})
        find_package(Threads REQUIRED)
        # SDL3: the engine's own (engine/gpu's FetchContent; resolved when the build is generated).
        target_link_libraries(ps2platform PRIVATE SDL3::SDL3-static Threads::Threads)
    else()
        message(FATAL_ERROR "ps2platform: no platform layer for ${CMAKE_SYSTEM_NAME} yet - "
                            "src/platform_<system>.cpp implements include/ps2platform/platform.h and keys.h "
                            "(docs/option_p/portability_handoff.md)")
    endif()
    target_include_directories(ps2platform PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_compile_features(ps2platform PUBLIC cxx_std_17)
    if(MSVC)
        target_compile_options(ps2platform PRIVATE /EHsc /W3 /Z7)
    endif()
endif()
