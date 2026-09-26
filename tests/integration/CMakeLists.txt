# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_integration_tests
    CanEngineTests.cpp
    DecodeThroughputTests.cpp
    ExampleScriptTests.cpp
    LuaEcuBusTests.cpp
    VirtualBusTests.cpp
)

target_link_libraries(torquebus_integration_tests
    PRIVATE torquebus::drivers
            torquebus::warnings
            GTest::gtest_main
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_integration_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_integration_tests PROPERTIES FOLDER "Tests")

# The example scripts are run from the source tree, not from a copy: the files
# people are pointed at are the files that have to work.
target_compile_definitions(torquebus_integration_tests
    PRIVATE TORQUEBUS_EXAMPLE_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/examples/scripts"
            TORQUEBUS_EXAMPLE_DATABASE_DIR="${CMAKE_SOURCE_DIR}/examples/databases"
)

# These drive real threads and real timing, so they get more headroom than the
# CTest default before being declared hung.
# The Qt this build was configured against, on the DLL search path for both
# moments the binary runs: the enumeration at build time and the tests under
# ctest. See the module's header - covering only the second cost a build.
torquebus_test_dl_paths(TORQUEBUS_TEST_DL_PATHS)

gtest_discover_tests(torquebus_integration_tests
    DL_PATHS ${TORQUEBUS_TEST_DL_PATHS}
    PROPERTIES
        LABELS "integration"
        TIMEOUT 120
)
