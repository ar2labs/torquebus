# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Kvaser backend, built twice over: once as a static library the hardware
# tests can link, and once as the plugin the application loads.
#
# The library exists for the tests and for nothing else. Splitting it out is
# what lets a test construct a KvaserCanBackend directly - which is how the
# hardware tests were written and how they should stay - without the
# application linking CANlib, which is the whole point of moving this out of
# the binary (PLAN.md section 31).

add_library(torquebus_driver_kvaser STATIC
    KvaserCanBackend.h
    KvaserCanBackend.cpp
)

add_library(torquebus::driver_kvaser ALIAS torquebus_driver_kvaser)

target_link_libraries(torquebus_driver_kvaser
    PUBLIC  torquebus::drivers
    PRIVATE torquebus::warnings
)

# Always compiled, with or without the SDK: without it the .cpp builds a stub
# that reports itself unavailable, so isCompiledIn() has an honest answer to
# give and the plugin can decline with a reason instead of registering an
# interface that can never open.
if(TORQUEBUS_HAVE_KVASER)
    target_include_directories(torquebus_driver_kvaser PRIVATE "${KVASER_CANLIB_INCLUDE_DIR}")
    target_link_libraries(torquebus_driver_kvaser PRIVATE "${KVASER_CANLIB_LIBRARY}")
    target_compile_definitions(torquebus_driver_kvaser PRIVATE TORQUEBUS_HAVE_KVASER=1)
endif()

set_target_properties(torquebus_driver_kvaser PROPERTIES FOLDER "Plugins")

# --- The plugin itself -----------------------------------------------------

add_library(torquebus_plugin_kvaser MODULE Plugin.cpp)

target_link_libraries(torquebus_plugin_kvaser
    PRIVATE torquebus::driver_kvaser
            torquebus::plugin_api
            torquebus::warnings
)

# Beside the executable, in the one directory the loader looks in - so a build
# tree behaves the same way an installation does.
#
# Expressed against the executable's own directory rather than a path built from
# CMAKE_BINARY_DIR, because those are not the same place under every generator.
# This used to force ${CMAKE_BINARY_DIR}/bin/plugins for every configuration,
# which is right for Ninja - where the executable is in bin - and wrong for the
# Visual Studio generator, which puts it in bin/Debug. The loader looks beside
# the executable, so a VS build had the plugins on disk, in a directory nothing
# reads, and no Kvaser or PEAK support at all. Exactly the shape of the release
# that shipped without them.
set_target_properties(torquebus_plugin_kvaser PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:TorqueBusStudio>/plugins"
    OUTPUT_NAME "torquebus-driver-kvaser"
    FOLDER "Plugins"
)

# Into the installed tree too, in the same directory the loader looks in.
#
# Without this the packaged build has no hardware support at all: moving the
# vendor backends out of the executable (PLAN.md section 31) means a release
# that copies only the executable copies nothing that can open a Kvaser. The
# .lib import library is deliberately left behind - nothing links a plugin.
install(TARGETS torquebus_plugin_kvaser
    LIBRARY DESTINATION plugins COMPONENT torquebus
    RUNTIME DESTINATION plugins COMPONENT torquebus
)

# The application does not link this, but it is useless without it being built.
add_dependencies(TorqueBusStudio torquebus_plugin_kvaser)
