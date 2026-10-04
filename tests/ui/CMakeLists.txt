# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(torquebus_ui_tests DockingTests.cpp)
target_link_libraries(torquebus_ui_tests
    PRIVATE torquebus::ui torquebus::warnings GTest::gtest
)
set_target_properties(torquebus_ui_tests PROPERTIES FOLDER "Tests")

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
