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
