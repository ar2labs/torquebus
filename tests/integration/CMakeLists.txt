# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_integration_tests
    CanEngineTests.cpp
    DecodeThroughputTests.cpp
    LuaEcuBusTests.cpp
    VirtualBusTests.cpp
)

target_link_libraries(torquebus_integration_tests
    PRIVATE torquebus::drivers
            torquebus::warnings
            Catch2::Catch2WithMain
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_integration_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_integration_tests PROPERTIES FOLDER "Tests")

# These drive real threads and real timing, so they get more headroom than the
# CTest default before being declared hung.
# The Qt this build was configured against, prepended to each test's PATH, so a
# foreign Qt earlier on PATH fails the test instead of stopping it at a modal
# dialog nobody is there to dismiss. See the module's header.
torquebus_test_properties(TORQUEBUS_TEST_PROPERTIES)

catch_discover_tests(torquebus_integration_tests
    PROPERTIES
        ${TORQUEBUS_TEST_PROPERTIES}
        LABELS "integration"
        TIMEOUT 120
)
