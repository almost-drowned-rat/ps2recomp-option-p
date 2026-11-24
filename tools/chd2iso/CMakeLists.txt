# chd2iso: a CHD disc image as an .iso (chd2iso.c). Part of option-p's Android standard: a port's jni build adds
#
#   set(CHD2ISO_OUTPUT_NAME "libchd2iso_exe")      # an app execs only from nativeLibraryDir: lib*.so
#   add_subdirectory(<engine>/tools/chd2iso ${CMAKE_BINARY_DIR}/chd2iso)
#
# and ships the result next to its other setup programs. Built from the engine's own libchdr (BSD-3-Clause,
# host/third_party/libchdr), so no prebuilt libchdr is needed. Works as a plain program on the PC too.
cmake_minimum_required(VERSION 3.16)
project(ps2_chd2iso C)

set(CHD2ISO_OUTPUT_NAME "" CACHE STRING "Output name (Android: libchd2iso_exe, built with the .so suffix)")

if(NOT TARGET chdr-static)                          # (the same settings as engine/launcher's)
    set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
    set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
    set(CHDR_VERIFY_BLOCK_CRC OFF CACHE BOOL "" FORCE)
    set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../host/third_party/libchdr ${CMAKE_BINARY_DIR}/libchdr EXCLUDE_FROM_ALL)
endif()

add_executable(ps2_chd2iso chd2iso.c)
target_link_libraries(ps2_chd2iso PRIVATE chdr-static)
target_include_directories(ps2_chd2iso PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../host/third_party/libchdr/include)
if(CHD2ISO_OUTPUT_NAME)
    set_target_properties(ps2_chd2iso PROPERTIES OUTPUT_NAME "${CHD2ISO_OUTPUT_NAME}")
endif()
if(ANDROID)
    set_target_properties(ps2_chd2iso PROPERTIES SUFFIX ".so")
    target_link_options(ps2_chd2iso PRIVATE "-Wl,-z,max-page-size=16384")
    target_link_libraries(ps2_chd2iso PRIVATE m)
endif()
