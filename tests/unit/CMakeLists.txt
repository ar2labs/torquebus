# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_unit_tests
    CanFrameTests.cpp
    CanFilterTests.cpp
    CanStatisticsTests.cpp
    FrameQueueTests.cpp
    LuaEcuNodeTests.cpp
    PipelineGraphTests.cpp
    QtMacroGuardTests.cpp
    TraceStoreTests.cpp
    ResultTests.cpp
    SettingsStoreTests.cpp
)

target_link_libraries(torquebus_unit_tests
    PRIVATE torquebus::core
            torquebus::services
            torquebus::warnings
            Catch2::Catch2WithMain
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_unit_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_unit_tests PROPERTIES FOLDER "Tests")

catch_discover_tests(torquebus_unit_tests)
