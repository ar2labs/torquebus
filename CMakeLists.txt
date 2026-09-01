#
# TorqueBus Studio
# Copyright (C) TorqueBus contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.24)

project(TorqueBusStudio
    VERSION 0.5.0
    DESCRIPTION "Open Automotive Network & Diagnostics Workbench"
    HOMEPAGE_URL "https://github.com/ar2labs/torquebus"
    LANGUAGES CXX
)

# --------------------------------------------------------------------------
# Global settings
# --------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------

option(TORQUEBUS_BUILD_TESTS      "Build the TorqueBus test suite"        ON)
option(TORQUEBUS_ENABLE_WARNINGS  "Enable a strict warning set"           ON)
option(TORQUEBUS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"   OFF)
option(TORQUEBUS_ENABLE_KVASER    "Build the Kvaser CANlib backend"       ON)
option(TORQUEBUS_ENABLE_PEAK      "Build the PEAK-System backend"         ON)
option(TORQUEBUS_ALLOW_ANY_COMPILER
       "Skip the MSVC toolchain check on Windows (for deliberate ports)"  OFF)

# --------------------------------------------------------------------------
# Toolchain check
# --------------------------------------------------------------------------
#
# PLAN.md section 1 freezes MSVC x64 on Windows, and CI builds with MSVC. The
# failure this guards against is quiet: an installed LLVM ahead of cl.exe on
# PATH gets picked up by CMake, and the build proceeds against MSVC-compiled Qt
# with a GNU-like driver until something fails much later, at link time or in a
# warnings-as-errors CI run that the local build never reproduced.
#
# Better to say so here, in one sentence, than to debug it there.

if(WIN32 AND NOT MSVC AND NOT TORQUEBUS_ALLOW_ANY_COMPILER)
    message(FATAL_ERROR
        "TorqueBus Studio targets MSVC x64 on Windows, but CMake selected "
        "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).\n"
        "This usually means another compiler comes first on PATH.\n"
        "Fix it in one of these ways:\n"
        "  * cmake --preset windows-msvc-vs   (works from any prompt; the\n"
        "    Visual Studio generator finds the toolchain by itself)\n"
        "  * cmake --preset windows-msvc-debug, run from an\n"
        "    'x64 Native Tools Command Prompt for VS 2022' so cl.exe is on PATH\n"
        "  * or pass it explicitly: -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl\n"
        "Delete the build directory first - CMake caches the compiler it "
        "picked and will not reconsider otherwise.\n"
        "Note that clang-cl also satisfies this check; a GNU-like clang driver "
        "does not, because it does not match the ABI of an MSVC-built Qt.\n"
        "If you are deliberately porting to another toolchain, set "
        "-DTORQUEBUS_ALLOW_ANY_COMPILER=ON."
    )
endif()

# Note on the linker, since this is where you would look for it: there is no
# equivalent guard below for "CMake picked GNU ld instead of link.exe". That
# failure happens inside project() above, during CMake's own compiler test, so
# nothing written here would ever run. It is handled where it can be - by
# pinning CMAKE_LINKER in CMakePresets.json - and diagnosed in
# docs/development/getting-started.md under Troubleshooting.

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

include(TorqueBusCompilerWarnings)
include(TorqueBusDependencies)

# --------------------------------------------------------------------------
# Project targets
# --------------------------------------------------------------------------

# Third-party sources we vendor and build ourselves, before anything uses them.
add_subdirectory(third_party/lua)

add_subdirectory(src)

if(TORQUEBUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

message(STATUS "")
if(CMAKE_CONFIGURATION_TYPES)
    set(TORQUEBUS_BUILD_TYPE_SUMMARY "${CMAKE_CONFIGURATION_TYPES} (chosen at build time)")
else()
    set(TORQUEBUS_BUILD_TYPE_SUMMARY "${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "TorqueBus Studio ${PROJECT_VERSION}")
message(STATUS "  Build type ............ ${TORQUEBUS_BUILD_TYPE_SUMMARY}")
message(STATUS "  Compiler .............. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Qt .................... ${Qt6_VERSION}")
message(STATUS "  Docking ............... KDDockWidgets (FetchContent)")
message(STATUS "  Tests ................. ${TORQUEBUS_BUILD_TESTS}")
message(STATUS "  Scripting ............. Lua 5.5.0 (vendored)")
message(STATUS "  Kvaser backend ........ ${TORQUEBUS_HAVE_KVASER}")
message(STATUS "  PEAK backend .......... ${TORQUEBUS_HAVE_PEAK}")
message(STATUS "")
