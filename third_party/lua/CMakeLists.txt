# SPDX-License-Identifier: MIT
#
# Lua 5.5.1, vendored.
#
# Lua ships a Makefile, not a CMake build, so this file is ours. The sources
# below are upstream and unmodified - the only thing TorqueBus adds is how they
# are compiled.
#
# Why vendored rather than found on the system, or linked from a prebuilt
# liblua.a: the interpreter is part of the product's behaviour, not an
# environment detail. An ECU script that runs here has to run identically on a
# colleague's machine and in CI, and "whatever Lua the machine happens to have"
# does not give that. It is 33 files and builds in seconds.
#
# lua.c, luac.c and ltests.c are deliberately not here: the first two are the
# standalone interpreter and compiler, each with its own main(), and the third
# is Lua's internal test harness, which needs LUA_DEBUG.

add_library(torquebus_lua STATIC
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

add_library(torquebus::lua ALIAS torquebus_lua)

target_include_directories(torquebus_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# No LUA_COMPAT_5_3.
#
# That flag would restore math.frexp, math.ldexp and the global unpack, which
# three of the cansim ECU scripts still call. Carrying it forever would freeze
# the scripting surface in a 5.3 shape that every future script author inherits,
# so those three scripts migrate instead - to string.pack("<f", x), which the
# newer cansim scripts already use and which does the same job in one line.
#
# See PLAN.md section 28.

if(WIN32)
    # Lua's own default for Windows. Enables the loadlib backend and the
    # os/io functions that expect Win32 rather than POSIX.
    target_compile_definitions(torquebus_lua PRIVATE LUA_USE_WINDOWS)
elseif(APPLE)
    target_compile_definitions(torquebus_lua PRIVATE LUA_USE_MACOSX)
else()
    target_compile_definitions(torquebus_lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(torquebus_lua PUBLIC m)
endif()

if(MSVC)
    # Upstream C, compiled as C. It is not our code and not our warning policy;
    # holding it to TorqueBus' warning set would produce noise nobody will act
    # on and would hide our own warnings in it.
    target_compile_options(torquebus_lua PRIVATE /W0)
else()
    target_compile_options(torquebus_lua PRIVATE -w)
endif()

set_target_properties(torquebus_lua PROPERTIES
    FOLDER "Third-party"
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)
