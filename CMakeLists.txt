#
# TorqueBus Studio
# Copyright (C) TorqueBus contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.25)

# 3.25, because that is what this project actually needs. The number used to be
# 3.24, and that was wrong: CMakePresets.json is schema version 6 and
# FetchContent_Declare below takes SYSTEM, and both of those arrived in 3.25.
# Nothing showed it because CI always runs the latest CMake.
#
# The same number is in CMakePresets.json (cmakeMinimumRequired), in the README's
# requirements and in docs/development/getting-started.md. Change them together.
#
# Raising it also raises the policy baseline, so it was measured rather than
# assumed: from 3.24 to 3.25 the only thing that changes in what gets compiled is
# how MSVC's debug-information flag is spelled (-Zi for /Zi, which is the same
# flag). No link line changes. Raising it much further is another matter - from
# 3.28 CMake starts scanning every C++20-or-later source for module imports, and
# this project is C++23 throughout - so go past 3.27 on purpose, not in passing.

# C alongside CXX because of the vendored Lua: it is 32 .c files, and without C
# enabled here CMake configures happily and then fails at *generate* time with
# thirty-odd copies of
#
#     Error required internal CMake variable not set, cmake may not be built
#     correctly.  Missing variable is: CMAKE_C_COMPILE_OBJECT
#
# one per .c file. The message blames CMake's own installation, which is a red
# herring: it means "you gave me C sources in a project that never enabled C".
# Detecting a C compiler in a FetchContent sub-build does not enable it here.
#
# The wording is generator-dependent - with Makefiles the same mistake reads
# "Cannot determine link language for target torquebus_lua" - so match on the
# cause rather than on the sentence.
# The version names the last PLAN.md milestone that is *finished*, not the one
# being worked on: a build made while v0.10 is half done says 0.9.0, because
# what it contains is v0.9 plus some of the next thing. Bump it in the commit
# that completes a milestone.
#
# It reaches the application as TORQUEBUS_VERSION, and from there the Output
# panel's startup line, the About box and the installer's filename - so this is
# the only place it is written down. It sat at 0.5.0 through four milestones
# because nothing failed when it was wrong, which is what this comment is for.
project(TorqueBusStudio
    VERSION 0.17.0
    DESCRIPTION "Open Automotive Network & Diagnostics Workbench"
    HOMEPAGE_URL "https://github.com/ar2labs/torquebus"
    LANGUAGES CXX C
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
# /FS, for everything, including what FetchContent brings in
# --------------------------------------------------------------------------
#
# The Visual Studio generator builds a target's translation units in parallel,
# and every one of them writes the same compiler PDB. Without /FS that is
#
#   error C1041: cannot open program database '...\QtNodes.pdb';
#   if multiple CL.EXE write to the same .PDB file, use /FS
#
# and the build stops. Ninja never hits it, which is why this went unnoticed:
# the Ninja presets are what everybody here builds with, and windows-msvc-vs -
# the one the README calls "start here", the one meant for a contributor who has
# just cloned and has no developer prompt - had never been built at all.
#
# add_compile_options and not a target property, because the target it fails on
# is QtNodes: FetchContent builds it inside this project, so it inherits
# directory-level options and nothing else we set can reach it. This has to come
# before the dependencies are declared for that inheritance to happen.
#
# The cost on Ninja builds is nothing measurable - /FS serialises PDB writes,
# which Ninja was already doing by compiling one file per process.
if(MSVC)
    add_compile_options(/FS)
endif()

# --------------------------------------------------------------------------
# /EHsc, stated and not inherited
# --------------------------------------------------------------------------
#
# This project throws and catches: ThreadGuard is a try/catch around every thread
# body and every destructor that flushes, the plugin loader survives a plugin that
# throws, and the standard library throws on its own account. Exceptions with
# proper unwinding are an MSVC compiler option, /EHsc, and nothing here asked for
# it. It arrived anyway, because CMake puts it into CMAKE_CXX_FLAGS the first time
# a build directory is configured - and a directory whose cache holds no default
# flags compiles without it. One did: a directory whose cache read
# "CMAKE_CXX_FLAGS:STRING=" built the whole tree with
#
#   warning C4530: C++ exception handler used, but unwind semantics are not
#   enabled. Specify /EHsc
#
# on nearly every translation unit. The warnings are the visible part. What they
# say is that unwinding is off, so a destructor need not run as an exception
# passes - which is exactly what ThreadGuard is there to make safe.
#
# So the project says it, the way it says /FS and for the same reason: as a
# directory-level option, so that what FetchContent brings in is built the same
# way. Only when CMAKE_CXX_FLAGS does not already choose an exception model, so a
# healthy directory gets no duplicate and somebody who deliberately passes /EHa
# keeps it. C++ only: /EH means nothing to the vendored Lua.
if(MSVC AND NOT CMAKE_CXX_FLAGS MATCHES "[/-]EH")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
endif()

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

include(TorqueBusCompilerWarnings)
include(TorqueBusDependencies)
include(TorqueBusLayering)
include(TorqueBusTestEnvironment)

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
message(STATUS "  Scripting ............. Lua 5.5.1 (vendored)")
message(STATUS "  Kvaser backend ........ ${TORQUEBUS_HAVE_KVASER}")
message(STATUS "  PEAK backend .......... ${TORQUEBUS_HAVE_PEAK}")
message(STATUS "")
