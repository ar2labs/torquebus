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

# Beside the executable, in the one directory the loader looks in - so a build
# tree behaves the same way an installation does.
#
# Expressed against the executable's own directory rather than a path built from
# CMAKE_BINARY_DIR, because those are not the same place under every generator.
# See the note in driver-kvaser: a Visual Studio build put the executable in
# bin/Debug and the plugins in bin, so it had no hardware support at all.
set_target_properties(torquebus_plugin_peak PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    OUTPUT_NAME "torquebus-driver-peak"
    FOLDER "Plugins"
)

# --- What the plugin needs, that the application does not --------------------
#
# The executable's own deployment copies what *it* links, and it does not link
# Qt6::SerialBus - that is the whole point of moving PEAK out of the binary. So
# the SerialBus DLL never arrived beside the executable, and this plugin failed
# to load on any machine without Qt on PATH. The symptom is the one the loader
# was built to avoid: an interface list with Kvaser in it and no PEAK, and
# nothing anywhere saying why.
#
# Found by running the application with a deliberately hostile PATH and reading
# the process's loaded modules. Both generators, not just one.
#
# The canbus plugin is Qt's own, loaded by path when a PCAN channel is opened.
# It is copied for the same reason and cannot be verified here: proving it works
# needs a PEAK adapter, and that is the hardware pass.
if(WIN32 AND TARGET TorqueBusStudio)
    add_custom_command(TARGET torquebus_plugin_peak POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:torquebus_plugin_peak>"
                "$<TARGET_FILE_DIR:TorqueBusStudio>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying what the PEAK plugin links beside the executable"
        VERBATIM
    )

    if(TARGET Qt6::PeakCanBusPlugin)
        add_custom_command(TARGET torquebus_plugin_peak POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:TorqueBusStudio>/canbus"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Qt6::PeakCanBusPlugin>"
                    "$<TARGET_FILE_DIR:TorqueBusStudio>/canbus"
            COMMENT "Copying Qt's PCAN backend into canbus"
            VERBATIM
        )
    endif()
endif()

# Into the installed tree too, in the same directory the loader looks in.
#
# Without this the packaged build has no hardware support at all: moving the
# vendor backends out of the executable (PLAN.md section 31) means a release
# that copies only the executable copies nothing that can open a PCAN. The
# .lib import library is deliberately left behind - nothing links a plugin.
install(TARGETS torquebus_plugin_peak
    LIBRARY DESTINATION plugins COMPONENT torquebus
    RUNTIME DESTINATION plugins COMPONENT torquebus
)

add_dependencies(TorqueBusStudio torquebus_plugin_peak)
