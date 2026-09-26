# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes the test binaries carry their own Qt, instead of trusting PATH.
#
# --- The failure this prevents ----------------------------------------------
#
# Every test executable links Qt, so the loader has to find Qt6Core.dll at
# startup - and on Windows it searches PATH. On a developer machine that is not
# a controlled list. On the machine this was found on, PATH contained:
#
#   C:\ST\STM32CubeCLT\STM32CubeProgrammer\bin\Qt6Core.dll   (Qt 6.10.2)
#   C:\Qt\6.11.2\msvc2022_64\bin\Qt6Core.dll                 (the right one)
#   ...\MiKTeX\miktex\bin\x64\Qt6Core.dll                    (another one)
#
# in that order. STM32CubeProgrammer ships its own Qt, it is earlier on PATH,
# and the binaries are built against 6.11.2 exactly - the README pins the
# version and says why. So the loader hands them 6.10.2 and they die with
# STATUS_ENTRYPOINT_NOT_FOUND before reaching main.
#
# What makes that worse than an ordinary failure is *how* it fails. Windows
# reports a missing entry point with a modal dialog, and a modal dialog nobody
# is there to dismiss does not return. The run does not fail - it stops, at zero
# CPU, for as long as anybody is willing to wait. Four test processes sat like
# that for twelve minutes before being killed, with an empty output file and no
# clue in it.
#
# So the tests stop relying on PATH being right, and get the Qt they were built
# against prepended to theirs. Same mechanism, same argument, as the compiler
# and linker pins in CMakePresets.json: this project keeps discovering that the
# first tool on PATH is not the intended one.
#
# --- And a timeout, because the net has a hole ------------------------------
#
# The environment fix cannot cover every way a test can block - a modal dialog
# from something else, a wait that never returns. A test that hangs should fail,
# and without a timeout it does not: it waits, and so does whoever started it.
#
# The timeouts are set by each test directory rather than here, because they are
# not the same number: the integration tests drive real threads and real timing
# and already carried 120 seconds, which is why the hang that started all this
# was in the unit tests - the only suite with no timeout at all.

# Gives `out_variable` the directory to put on the DLL search path.
#
# Handed to gtest_discover_tests as DL_PATHS, which covers *both* moments the
# binary runs: the enumeration at build time and the tests under ctest. That
# distinction cost a build. The first version of this set
# ENVIRONMENT_MODIFICATION on the tests, which fixed ctest and left the
# enumeration alone - and the enumeration is a run too. Building the Visual
# Studio preset from a prompt with no Qt on PATH failed there with
# STATUS_DLL_NOT_FOUND, before a single test had a chance to pass or fail.
#
# It also cannot be applied after gtest_discover_tests returns: that call does
# not create the tests at configure time, it writes a script that enumerates the
# binary's TESTs when the binary is built. At configure time the
# directory's TESTS property is empty, so a loop over it sets properties on
# nothing - which an even earlier version of this file did, silently.
function(torquebus_test_dl_paths out_variable)
    set("${out_variable}" "" PARENT_SCOPE)

    if(NOT WIN32)
        return()
    endif()

    # The bin directory of the Qt that CMake actually found, taken from the
    # imported target rather than from a variable that might describe a
    # different installation. Qt6::Core's location is the DLL itself.
    if(NOT TARGET Qt6::Core)
        message(WARNING "torquebus_test_dl_paths: Qt6::Core not found")
        return()
    endif()

    get_target_property(_qt_core_configurations Qt6::Core IMPORTED_CONFIGURATIONS)
    list(GET _qt_core_configurations 0 _qt_core_configuration)
    get_target_property(_qt_core_dll Qt6::Core "IMPORTED_LOCATION_${_qt_core_configuration}")

    if(NOT _qt_core_dll)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    endif()

    if(NOT _qt_core_dll)
        message(WARNING "torquebus_test_dl_paths: no location for Qt6::Core")
        return()
    endif()

    get_filename_component(_qt_bin "${_qt_core_dll}" DIRECTORY)

    set("${out_variable}" "${_qt_bin}" PARENT_SCOPE)

    set(TORQUEBUS_TEST_QT_BIN "${_qt_bin}" CACHE INTERNAL "Qt bin prepended to test PATH")
endfunction()
