# SPDX-License-Identifier: GPL-3.0-or-later
#
# The PEAK backend, in the same two shapes as the Kvaser one: a static library
# for the hardware tests, and the plugin the application loads.
#
# The route to the vendor differs - PEAK is reached through Qt SerialBus rather
# than through a vendor header - and nothing else about the arrangement does.

add_library(torquebus_driver_peak STATIC
    PeakCanBackend.h
    PeakCanBackend.cpp
)

add_library(torquebus::driver_peak ALIAS torquebus_driver_peak)

target_link_libraries(torquebus_driver_peak
    PUBLIC  torquebus::drivers
    PRIVATE torquebus::warnings
)

# Same arrangement as Kvaser: without the module the .cpp builds the stub, so
# isCompiledIn() can answer honestly and the plugin declines with a reason.
if(TORQUEBUS_HAVE_PEAK)
    target_link_libraries(torquebus_driver_peak PRIVATE Qt6::SerialBus)
    target_compile_definitions(torquebus_driver_peak PRIVATE TORQUEBUS_HAVE_PEAK=1)
endif()

set_target_properties(torquebus_driver_peak PROPERTIES
    AUTOMOC ON
    FOLDER "Plugins"
)

# --- The plugin itself -----------------------------------------------------

add_library(torquebus_plugin_peak MODULE Plugin.cpp)

target_link_libraries(torquebus_plugin_peak
    PRIVATE torquebus::driver_peak
            torquebus::plugin_api
            torquebus::warnings
)

set(TORQUEBUS_PLUGIN_OUTPUT "${CMAKE_BINARY_DIR}/bin/plugins")

set_target_properties(torquebus_plugin_peak PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${TORQUEBUS_PLUGIN_OUTPUT}"
    RUNTIME_OUTPUT_DIRECTORY "${TORQUEBUS_PLUGIN_OUTPUT}"
    OUTPUT_NAME "torquebus-driver-peak"
    FOLDER "Plugins"
)

foreach(configuration ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${configuration} configuration_upper)
    set_target_properties(torquebus_plugin_peak PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${configuration_upper} "${TORQUEBUS_PLUGIN_OUTPUT}"
        RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${TORQUEBUS_PLUGIN_OUTPUT}"
    )
endforeach()

add_dependencies(TorqueBusStudio torquebus_plugin_peak)
