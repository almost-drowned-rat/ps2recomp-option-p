# Option P M0: build the engine host and its own tests (ps2host_tests) without a game.
# Same compiler settings as engine/templates/game/port/CMakeLists.txt (static runtime, path scrub,
# no fused multiply-add). Configure with clang-cl + Ninja, build only the target ps2host_tests, -j2.

cmake_minimum_required(VERSION 3.20)
project(option_p_host_tests LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(repo_root ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
if(MSVC)
    file(TO_NATIVE_PATH "${repo_root}/" repo_root_native)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options("/clang:-ffile-prefix-map=${repo_root}/=" "/clang:-ffile-prefix-map=${repo_root_native}="
                            "/clang:-ffp-contract=off")
    else()
        add_compile_options("/d1trimfile:${repo_root_native}")
    endif()
endif()

add_subdirectory(${repo_root}/host ${CMAKE_BINARY_DIR}/ps2host)
