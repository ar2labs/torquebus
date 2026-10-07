# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_ui_tests DockingTests.cpp TransmitPanelTests.cpp CanvasPanelTests.cpp TracePanelTests.cpp GraphPanelTests.cpp ScriptEditorPanelTests.cpp IconImageProviderTests.cpp ClusterThemeSyncTests.cpp ClusterViewTests.cpp)
target_link_libraries(torquebus_ui_tests
    PRIVATE torquebus::ui torquebus_clusterplugin torquebus::warnings GTest::gtest
)
set_target_properties(torquebus_ui_tests PROPERTIES FOLDER "Tests")

# The cluster tests load its QML from the source tree, and Qt's own modules from
# the Qt that was found: the Qt libraries beside the test executable are copies, and Qt looks
# for its QML modules relative to the libraries it was loaded from.
target_compile_definitions(torquebus_ui_tests PRIVATE
    TORQUEBUS_CLUSTER_QML_DIR="${CMAKE_SOURCE_DIR}/src/ui/dashboard/cluster/qml"
    TORQUEBUS_QT_QML_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}"
)

get_target_property(TORQUEBUS_OFFSCREEN_PLUGIN Qt6::QOffscreenIntegrationPlugin LOCATION)
get_filename_component(TORQUEBUS_PLATFORM_PLUGIN_DIR "${TORQUEBUS_OFFSCREEN_PLUGIN}" DIRECTORY)

# The Qt DLLs come from the PATH that tests/CMakeLists.txt gives ctest. The
# platform plugin is a different matter: Qt loads it by path and nothing links it,
# while that only puts the DLLs on PATH - hence the variable below. Enumeration
# (--gtest_list_tests) returns before a QApplication exists, so it needs the
# DLLs and not the plugin.
gtest_discover_tests(torquebus_ui_tests
    PROPERTIES TIMEOUT 60 LABELS ui
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${TORQUEBUS_PLATFORM_PLUGIN_DIR}"
)
