# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests that need a real vendor driver installed. Labelled "hardware" and
# excluded from the default ctest run, so a clean checkout on a machine with no
# CAN software still goes green.

add_executable(torquebus_hardware_tests
    KvaserVirtualTests.cpp
    PeakLoopbackTests.cpp
)

# The vendor backends are plugin libraries now, and these tests construct them
# directly - which is how they were written and how they should stay. Linking
# the static halves gives them that without the application linking either.
target_link_libraries(torquebus_hardware_tests
    PRIVATE torquebus::drivers
            torquebus::driver_kvaser
            torquebus::driver_peak
            torquebus::warnings
            Catch2::Catch2WithMain
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_hardware_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_hardware_tests PROPERTIES FOLDER "Tests")

# The Qt this build was configured against, on the DLL search path for both
# moments the binary runs: the enumeration at build time and the tests under
# ctest. See the module's header - covering only the second cost a build.
torquebus_test_dl_paths(TORQUEBUS_TEST_DL_PATHS)

catch_discover_tests(torquebus_hardware_tests
    DL_PATHS ${TORQUEBUS_TEST_DL_PATHS}
    PROPERTIES
        LABELS "hardware"
        TIMEOUT 120
)
