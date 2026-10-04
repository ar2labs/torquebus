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
# The mirror image is a machine with no Qt on PATH at all, which is what a
# newcomer has. There the tests could not start either, and not where anybody
# was looking: the first build of the Visual Studio preset stopped while
# *enumerating* the tests, with STATUS_DLL_NOT_FOUND (0xc0000135), before
# anything had copied Qt beside them. That is a different moment from running
# them, and covering one is not covering both - see below.
#
# So the tests stop relying on PATH being right, and get the Qt they were built
# against prepended to theirs. Same mechanism, same argument, as the compiler
# and linker pins in CMakePresets.json: this project keeps discovering that the
# first tool on PATH is not the intended one.
#
# --- Two moments, and one place that covers both ----------------------------
#
# A test binary runs at two moments, and both need the Qt:
#
#   1. to be asked what tests it holds (--gtest_list_tests), which
#      gtest_discover_tests does for every binary;
#   2. under ctest, once per test.
#
# Discovery used to happen at build time, as a POST_BUILD step of each test
# target - run by the build tool, with whatever PATH the build tool had, before
# anything had copied Qt beside the binary. That is where the first build fell
# over, and nothing on the test side can reach it.
#
# So discovery moves to the other moment, where there is something to hold on
# to: DISCOVERY_MODE PRE_TEST runs it inside the ctest process, when ctest reads
# the test directories. And the Qt goes on PATH in that same process: ctest
# includes a script before it reads the first directory, the script puts Qt at
# the front of PATH, and a child process inherits its parent's environment. The
# enumeration sees it, and so does every test. No process per test, no wrapper,
# nothing to keep in step between the two moments - there is one environment.
#
# A build no longer runs a test binary at all, which ends the failure instead of
# working around it. ctest does, and finds its Qt.
#
# --- What this replaced, and why nothing said so ----------------------------
#
# Three attempts, in order. The first set ENVIRONMENT_MODIFICATION on each test,
# which fixed ctest and left the enumeration alone: the build that enumerated
# the tests, from a prompt with no Qt on PATH, failed there with
# STATUS_DLL_NOT_FOUND before a single test had a chance to pass or fail.
#
# The second handed the path to gtest_discover_tests as DL_PATHS. That is an
# option of Catch2's catch_discover_tests, and it came across unchanged when the
# suite moved to GoogleTest. gtest_discover_tests has no such option - and does
# not object to one, because cmake_parse_arguments sets an unknown keyword aside
# without a word. The path was computed, passed, and went nowhere; the
# enumeration was handed an empty executor and PATH was exactly what the shell
# had. Nothing failed, which is the worst way for this to go wrong: CI installs
# Qt and puts it on PATH, so the enumeration found its DLLs there, and the
# protection against another Qt earlier on PATH - the thing in the first section
# - was not in force, which nothing could notice either, because the tests still
# passed.
#
# The third was right, and unusable. TEST_LAUNCHER (CMake 3.29) puts a command in
# front of a test executable at both moments, so cmake -E env could prepend the
# Qt to PATH for each one. It worked - and under ctest each test took 0.34 s
# where it takes 0.07 without, which took the 616 unit tests from 12 seconds to
# 125 (four jobs, same machine, same build directory, measured both ways). A
# process in front of every test is a cost paid 650 times to fix something that
# is true once.
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

# Makes every test suite below this directory find the Qt this build was
# configured against, at build time and under ctest alike.
#
# Call it once, from the directory that holds the suites (tests/), before the
# first add_subdirectory. It has two halves, and both are needed: discovery has
# to happen in ctest, and ctest has to have the Qt on PATH.
#
# On anything but Windows it does nothing: there is no PATH search for shared
# libraries to get wrong, and an rpath is the loader's business.
function(torquebus_use_built_qt_for_tests)
    if(NOT WIN32)
        return()
    endif()

    # The bin directory of the Qt that CMake actually found, taken from the
    # imported target rather than from a variable that might describe a
    # different installation. Qt6::Core's location is the DLL itself.
    #
    # Both ways out of here are errors, where they used to be a warning and a
    # return: a function that quietly does nothing is the failure this file is
    # about, and find_package(Qt6 REQUIRED) has already run, so neither can be
    # reached by a missing Qt - only by something being wrong here.
    if(NOT TARGET Qt6::Core)
        message(FATAL_ERROR "torquebus_use_built_qt_for_tests: Qt6::Core is not a target")
    endif()

    get_target_property(_qt_core_configurations Qt6::Core IMPORTED_CONFIGURATIONS)
    list(GET _qt_core_configurations 0 _qt_core_configuration)
    get_target_property(_qt_core_dll Qt6::Core "IMPORTED_LOCATION_${_qt_core_configuration}")

    if(NOT _qt_core_dll)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    endif()

    if(NOT _qt_core_dll)
        message(FATAL_ERROR "torquebus_use_built_qt_for_tests: Qt6::Core has no location to take a directory from")
    endif()

    get_filename_component(_qt_bin "${_qt_core_dll}" DIRECTORY)

    # Half one: discovery inside ctest. It is the default for every
    # gtest_discover_tests call made below this directory, so a suite added later
    # gets it without anybody remembering to ask - and one that asked for
    # POST_BUILD would be putting the build-time enumeration back.
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST PARENT_SCOPE)

    # Half two: Qt at the front of PATH, in ctest's own environment. The script
    # is written at configure time with the path baked in, because ctest reads it
    # with none of this project's variables defined. Prepended and not assigned:
    # the rest of PATH stays, since a test may need something else the shell put
    # there.
    #
    # TEST_INCLUDE_FILES of *this* directory, which ctest reads before it reads
    # any directory below it - the generated CTestTestfile.cmake includes the
    # script first and lists the subdirectories after.
    set(_script "${CMAKE_CURRENT_BINARY_DIR}/TorqueBusQtOnPath.cmake")
    file(WRITE "${_script}" "set(ENV{PATH} \"${_qt_bin};\$ENV{PATH}\")\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_script}")

    # Said at configure time because the version of this that did nothing said
    # nothing; a line in the log is cheap, and it is where somebody looks first
    # when a test binary cannot start.
    message(STATUS "Tests run with Qt from ${_qt_bin} ahead of PATH")
endfunction()
