cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        set(DEVKITPRO "/opt/devkitpro")
    endif()
    if(NOT EXISTS "${DEVKITPRO}/cmake/Switch.cmake")
        message(FATAL_ERROR "Missing devkitPro Switch toolchain. See docs/BUILD.md; set DEVKITPRO for a custom installation.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${DEVKITPRO}/cmake/Switch.cmake" CACHE FILEPATH "Switch toolchain")
endif()

project(renpy-switch VERSION 8.5.3 LANGUAGES C CXX)
if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "This target requires the devkitPro NintendoSwitch toolchain")
endif()

set(RENPY_SOURCE "${CMAKE_SOURCE_DIR}/build/deps/renpy-8.5.3-source" CACHE PATH "Patched Ren'Py source")
set(RENPY_GENERATED "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH "Validated module manifest")
set(SWITCH_PYTHON "${CMAKE_SOURCE_DIR}/build/python/install" CACHE PATH "Target CPython prefix")

foreach(required IN ITEMS "${RENPY_GENERATED}/modules.cmake"
        "${SWITCH_PYTHON}/include/python3.13/Python.h"
        "${SWITCH_PYTHON}/include/python3.13/pyconfig.h"
        "${SWITCH_PYTHON}/lib/libpython3.13.a")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}. Run python3 tools/build.py first.")
    endif()
endforeach()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ENGINE REQUIRED IMPORTED_TARGET
    sdl2 SDL2_image SDL2_ttf egl glesv2
    libavformat libavcodec libavutil libswresample libswscale
    harfbuzz freetype2 fribidi zlib)

include("${RENPY_GENERATED}/modules.cmake")
add_executable(renpy-switch sources/main.c sources/game_picker.c sources/switch_stubs.c
    ${RENPY_MODULE_SOURCES})
set_target_properties(renpy-switch PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(renpy-switch PRIVATE Py_NO_ENABLE_SHARED _GNU_SOURCE
    SSIZE_MAX=__LONG_MAX__ HAVE_CONFIG_H=1)
target_compile_options(renpy-switch PRIVATE -O2 -fno-common)
target_include_directories(renpy-switch PRIVATE
    "${SWITCH_PYTHON}/include/python3.13" "${RENPY_GENERATED}"
    "${RENPY_SOURCE}/src" "${RENPY_SOURCE}" "${RENPY_SOURCE}/tmp/gen3-static")
# switch-pkg-config emits static transitive dependencies. The rescan group also
# resolves cycles between Python, extension modules and static multimedia libs.
target_link_libraries(renpy-switch PRIVATE
    "-Wl,--start-group" "${SWITCH_PYTHON}/lib/libpython3.13.a"
    PkgConfig::ENGINE bz2 nx m "-Wl,--end-group")
target_link_options(renpy-switch PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/renpy-switch.map")
