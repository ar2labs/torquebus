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
            GTest::gtest_main
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_hardware_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_hardware_tests PROPERTIES FOLDER "Tests")

# This binary does link Qt - the PEAK backend reaches the vendor through Qt
# SerialBus - so it needs the Qt the build was configured against as much as the
# unit tests do, including when it is enumerated. That comes from the ctest
# environment set up in tests/CMakeLists.txt.
gtest_discover_tests(torquebus_hardware_tests
    PROPERTIES
        LABELS "hardware"
        TIMEOUT 120
)
