# SPDX-License-Identifier: GPL-3.0-or-later
#
# The SAE J2534 Pass-Thru backend, built in two forms:
# a static library for tests, and a module plugin loaded by TorqueBusStudio.

add_library(torquebus_driver_j2534 STATIC
    J2534CanBackend.h
    J2534CanBackend.cpp
    J2534Registry.h
    J2534Registry.cpp
    J2534Types.h
)

add_library(torquebus::driver_j2534 ALIAS torquebus_driver_j2534)

target_link_libraries(torquebus_driver_j2534
    PUBLIC  torquebus::drivers
    PRIVATE torquebus::warnings
)

set_target_properties(torquebus_driver_j2534 PROPERTIES FOLDER "Plugins")

# --- The plugin itself -----------------------------------------------------

add_library(torquebus_plugin_j2534 MODULE Plugin.cpp)

target_link_libraries(torquebus_plugin_j2534
    PRIVATE torquebus::driver_j2534
            torquebus::plugin_api
            torquebus::warnings
)

set_target_properties(torquebus_plugin_j2534 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    OUTPUT_NAME "torquebus-driver-j2534"
    FOLDER "Plugins"
)

# Into the installed tree, in the directory the loader looks in.
install(TARGETS torquebus_plugin_j2534
    LIBRARY DESTINATION plugins COMPONENT torquebus
    RUNTIME DESTINATION plugins COMPONENT torquebus
)

add_dependencies(TorqueBusStudio torquebus_plugin_j2534)
