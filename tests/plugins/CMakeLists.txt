# SPDX-License-Identifier: GPL-3.0-or-later
#
# The libraries the plugin loader tests load.
#
# Real libraries, built by this build, because the cases worth testing are the
# refusals - and a refused plugin has to be genuinely wrong rather than
# described as wrong by the test.
#
# They go in a directory of their own, not beside the application, so that
# running TorqueBus from the build tree does not load six test plugins.

set(TORQUEBUS_TEST_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/plugins")

# name -> the macro that makes it misbehave, or "" for the one that works.
set(TORQUEBUS_TEST_PLUGINS
    good      ""
    declines  TORQUEBUS_TEST_PLUGIN_DECLINES
    throws    TORQUEBUS_TEST_PLUGIN_THROWS
    badkey    TORQUEBUS_TEST_PLUGIN_BAD_KEY
    badabi    TORQUEBUS_TEST_PLUGIN_BAD_ABI
    nosymbol  TORQUEBUS_TEST_PLUGIN_NO_SYMBOL
)

list(LENGTH TORQUEBUS_TEST_PLUGINS TORQUEBUS_TEST_PLUGIN_COUNT)
math(EXPR TORQUEBUS_TEST_PLUGIN_LAST "${TORQUEBUS_TEST_PLUGIN_COUNT} / 2 - 1")

foreach(index RANGE ${TORQUEBUS_TEST_PLUGIN_LAST})
    math(EXPR name_index "${index} * 2")
    math(EXPR macro_index "${index} * 2 + 1")

    list(GET TORQUEBUS_TEST_PLUGINS ${name_index} plugin_name)
    list(GET TORQUEBUS_TEST_PLUGINS ${macro_index} plugin_macro)

    set(target "torquebus_test_plugin_${plugin_name}")

    # MODULE rather than SHARED: these are loaded at runtime and never linked
    # against, which is what MODULE means and what keeps CMake from making an
    # import library nobody uses.
    add_library(${target} MODULE TestPlugin.cpp)

    target_link_libraries(${target} PRIVATE torquebus::plugin_api)

    if(NOT plugin_macro STREQUAL "")
        target_compile_definitions(${target} PRIVATE ${plugin_macro}=1)
    endif()

    # All six in one directory, which is what the loader is pointed at.
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${TORQUEBUS_TEST_PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${TORQUEBUS_TEST_PLUGIN_DIR}"
        FOLDER "Tests/Plugins"
    )

    # Multi-config generators append the configuration to the output directory
    # unless told otherwise, and the test looks in one fixed place.
    foreach(configuration ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${configuration} configuration_upper)
        set_target_properties(${target} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${configuration_upper} "${TORQUEBUS_TEST_PLUGIN_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${TORQUEBUS_TEST_PLUGIN_DIR}"
        )
    endforeach()

    list(APPEND TORQUEBUS_TEST_PLUGIN_TARGETS ${target})
endforeach()

# Handed to the unit tests so they know where to point the loader, and so the
# path is decided by the build rather than guessed by the test.
set(TORQUEBUS_TEST_PLUGIN_DIR "${TORQUEBUS_TEST_PLUGIN_DIR}" PARENT_SCOPE)
set(TORQUEBUS_TEST_PLUGIN_TARGETS "${TORQUEBUS_TEST_PLUGIN_TARGETS}" PARENT_SCOPE)
