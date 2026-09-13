# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_unit_tests
    AccentColorTests.cpp
    CanFrameTests.cpp
    CanSignalEncodeTests.cpp
    LuaSignalTests.cpp
    NodePathTests.cpp
    NodeStatisticsTests.cpp
    SignalSeriesTests.cpp
    TraceExportTests.cpp
    TraceLogTests.cpp
    CanSignalTests.cpp
    DbcDecoderNodeTests.cpp
    DbcParserTests.cpp
    CanFilterTests.cpp
    CanStatisticsTests.cpp
    FrameQueueTests.cpp
    GraphDescriptionTests.cpp
    LuaEcuNodeTests.cpp
    PipelineGraphTests.cpp
    ProjectFileTests.cpp
    QtMacroGuardTests.cpp
    TraceStoreTests.cpp
    TransmitListTests.cpp
    ResultTests.cpp
    HardwareProfileTests.cpp
    IsoTpNodeTests.cpp
    IsoTpTests.cpp
    J1939IdTests.cpp
    J1939NameTests.cpp
    J1939TransportTests.cpp
    J1939AddressTableTests.cpp
    J1939DiagnosticsTests.cpp
    J1939NameTablesTests.cpp
    J1939NodeTests.cpp
    PluginLoaderTests.cpp
    DashboardTests.cpp
    RestBusTests.cpp
    LuaBusAndFaultTests.cpp
    LuaHotReloadTests.cpp
    LuaTestSequenceTests.cpp
    LuaTimingTests.cpp
    LuaUdsEcuTests.cpp
    UdsClientNodeTests.cpp
    UdsClientTests.cpp
    UdsServerTests.cpp
    UdsServiceCatalogTests.cpp
    WorkspacesTests.cpp
    RecentProjectsTests.cpp
    SettingsStoreTests.cpp
)

# GraphDescriptionTests builds a graph over the virtual bus, so this binary
# needs the drivers too. Everything it uses is still headless.
target_link_libraries(torquebus_unit_tests
    PRIVATE torquebus::core
            torquebus::drivers
            torquebus::plugin_host
            torquebus::services
            torquebus::theme
            torquebus::warnings
            Catch2::Catch2WithMain
)

# The loader tests load real libraries, so those have to exist before the tests
# run - and the path comes from the build rather than being guessed here.
add_dependencies(torquebus_unit_tests ${TORQUEBUS_TEST_PLUGIN_TARGETS})

target_compile_definitions(torquebus_unit_tests
    PRIVATE TORQUEBUS_TEST_PLUGIN_DIR="${TORQUEBUS_TEST_PLUGIN_DIR}"
)

find_package(Threads REQUIRED)
target_link_libraries(torquebus_unit_tests PRIVATE Threads::Threads)

set_target_properties(torquebus_unit_tests PROPERTIES FOLDER "Tests")

# One test loads a real example script, so it needs to know where they live.
target_compile_definitions(torquebus_unit_tests
    PRIVATE TORQUEBUS_EXAMPLE_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/examples/scripts"
            TORQUEBUS_EXAMPLE_PROJECT_DIR="${CMAKE_SOURCE_DIR}/examples/projects"
)

catch_discover_tests(torquebus_unit_tests)
