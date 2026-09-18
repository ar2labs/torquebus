# SPDX-License-Identifier: GPL-3.0-or-later
#
# Four libraries and one executable, layered strictly downwards:
#
#   app  ->  ui  ->  services  ->  drivers  ->  core
#
# The link dependencies below are the architecture rules from
# docs/ARCHITECTURE.md made mechanical: torquebus_core does not link Qt::Widgets,
# so a widget include in the core is a build error rather than a code review
# comment (rule #3), and no driver links the UI (rule #2).

# ---------------------------------------------------------------------------
# torquebus_core - vendor-neutral, UI-free domain model
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

add_library(torquebus_core STATIC
    core/Result.h
    core/ThreadGuard.h
    core/can/CanFrame.h
    core/can/CanFrame.cpp
    core/can/CanFilter.h
    core/can/CanStatistics.h
    core/can/CanTypes.h
    core/can/FrameQueue.h
    core/can/CanChannel.h
    core/can/CanChannel.cpp
    core/can/CanEngine.h
    core/can/CanEngine.cpp

    core/database/CanSignal.h
    core/database/CanSignal.cpp
    core/database/CanMessage.h
    core/database/CanMessage.cpp
    core/database/DbcParser.h
    core/database/DbcParser.cpp
    core/database/DecodedSignal.h

    core/pipeline/PortType.h
    core/pipeline/PipelineNode.h
    core/pipeline/PipelineGraph.h
    core/pipeline/PipelineGraph.cpp
    core/pipeline/nodes/FrameNodes.h
    core/pipeline/nodes/SignalNodes.h
    core/pipeline/nodes/DbcDecoderNode.h
    core/pipeline/nodes/DbcDecoderNode.cpp
    core/pipeline/NodeParameters.h
    core/pipeline/NodeCatalog.h
    core/pipeline/NodeCatalog.cpp
    core/pipeline/GraphDescription.h
    core/pipeline/GraphDescription.cpp

    core/diagnostics/DiagnosticEvent.h
    core/diagnostics/DiagnosticSession.h
    core/diagnostics/DiagnosticSession.cpp
    core/diagnostics/UdsClientNode.h
    core/diagnostics/UdsClientNode.cpp
    core/diagnostics/UdsServer.h
    core/diagnostics/UdsServer.cpp
    core/diagnostics/UdsServiceCatalog.h
    core/diagnostics/UdsServiceCatalog.cpp
    core/diagnostics/UdsTypes.h
    core/diagnostics/UdsTypes.cpp
    core/diagnostics/UdsClient.h
    core/diagnostics/UdsClient.cpp
    core/diagnostics/DiagnosticEvent.cpp
    core/isotp/IsoTpNode.h
    core/isotp/IsoTpNode.cpp
    core/isotp/IsoTpTypes.h
    core/isotp/IsoTpTypes.cpp
    core/isotp/IsoTpConnection.h
    core/isotp/IsoTpConnection.cpp

    core/j1939/J1939Id.h
    core/j1939/J1939Id.cpp
    core/j1939/J1939Name.h
    core/j1939/J1939Name.cpp
    core/j1939/J1939NameTables.h
    core/j1939/J1939NameTables.cpp
    core/j1939/J1939Transport.h
    core/j1939/J1939Transport.cpp
    core/j1939/J1939AddressTable.h
    core/j1939/J1939AddressTable.cpp
    core/j1939/J1939Diagnostics.h
    core/j1939/J1939Diagnostics.cpp
    core/j1939/J1939Network.h
    core/j1939/J1939Network.cpp
    core/j1939/J1939Node.h
    core/j1939/J1939Node.cpp
    core/log/TraceLog.h
    core/log/TraceLog.cpp
    core/log/LogNodes.h
    core/log/LogNodes.cpp
    core/log/TraceExport.h
    core/log/TraceExport.cpp

    core/plot/SignalSeries.h
    core/plot/SignalSeries.cpp
    core/plot/SignalPlotNode.h

    core/trace/TraceStore.h
    core/trace/TraceStore.cpp
    core/transmit/TransmitEntry.h
    core/transmit/TransmitList.h
    core/transmit/TransmitList.cpp
    core/transmit/TransmitListNode.h
    core/transmit/TransmitListNode.cpp
    core/trace/TraceSinkNode.h

    core/scripting/LuaPrelude.h
    core/scripting/LuaRuntime.h
    core/scripting/LuaRuntime.cpp
    core/scripting/LuaEcuNode.h
    core/scripting/LuaEcuNode.cpp
    core/scripting/ScriptLibrary.h
    core/scripting/ScriptLibrary.cpp
    core/scripting/LuaTestPrelude.h
    core/scripting/LuaTestNode.h
    core/scripting/LuaTestNode.cpp

    core/testing/TestReport.h
    core/testing/TestReport.cpp

    core/simulation/RestBusNode.h
    core/simulation/RestBusNode.cpp

    core/dashboard/SystemVariables.h
    core/dashboard/SystemVariables.cpp
    core/dashboard/DashboardDescription.h
    core/dashboard/DashboardDescription.cpp

    # The driver interface belongs to the core, not to the drivers: the core
    # defines the abstraction, and each vendor implements it (dependency
    # inversion). It lives under drivers/api/ because that is where PLAN.md
    # section 10 puts it and because that is where an implementer looks for it
    # - but the core must never depend on torquebus_drivers, which would drag
    # every vendor backend into a headless test binary.
    drivers/api/ICanBackend.h
)

add_library(torquebus::core ALIAS torquebus_core)

target_include_directories(torquebus_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Deliberately NOT Qt::Widgets, and deliberately not Qt::Gui: the core must
# stay linkable into a headless test binary and, later, into a CLI logger.
# Threads::Threads is the engine's dispatch thread - std::thread, not QThread,
# for the same reason.
target_link_libraries(torquebus_core
    PUBLIC  Threads::Threads
    PRIVATE torquebus::warnings
            torquebus::lua      # PRIVATE: only LuaRuntime.cpp includes lua.h
)

set_target_properties(torquebus_core PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# torquebus_drivers - one implementation of ICanBackend per vendor
# ---------------------------------------------------------------------------

set(TORQUEBUS_DRIVER_SOURCES
    drivers/api/CanBackendRegistry.h
    drivers/api/CanBackendRegistry.cpp
    drivers/virtual/VirtualCanBackend.h
    drivers/virtual/VirtualCanBackend.cpp

    # The virtual backend and nothing else.
    #
    # Kvaser and PEAK used to be here, and moving them out is what PLAN.md
    # section 31 actually asked for: while they were linked into this library,
    # the application image carried a dependency on CANlib and on Qt SerialBus.
    # They live in src/plugins now and are loaded rather than linked.
    #
    # What stays is the one backend that needs nothing installed - so an
    # application with no plugins at all still opens, builds a graph and runs a
    # measurement.
)

add_library(torquebus_drivers STATIC ${TORQUEBUS_DRIVER_SOURCES})
add_library(torquebus::drivers ALIAS torquebus_drivers)

target_include_directories(torquebus_drivers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_drivers
    PUBLIC  torquebus::core
    PRIVATE torquebus::warnings
)



set_target_properties(torquebus_drivers PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# torquebus_plugin_host - finding plugins, checking them, and letting them in
#
# It sits above both registries because that is what it hands a plugin: the
# backend registry and the node catalogue, the same two a built-in registers
# through. There is no second API for outsiders - if there were, it would be
# the one that rots.
#
# PluginApi.h is the one header an out-of-tree plugin includes, which is why
# both dependencies are PUBLIC: a plugin gets them by including it.
# ---------------------------------------------------------------------------

add_library(torquebus_plugin_host STATIC
    plugins/host/PluginApi.h
    plugins/host/PluginLoader.h
    plugins/host/PluginLoader.cpp
)

add_library(torquebus::plugin_host ALIAS torquebus_plugin_host)

target_include_directories(torquebus_plugin_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_plugin_host
    PUBLIC  torquebus::core
            torquebus::drivers
    PRIVATE torquebus::warnings
)

set_target_properties(torquebus_plugin_host PROPERTIES FOLDER "Libraries")

# An interface target for plugins themselves to link: the API header and the
# two libraries behind it, without the loader. A plugin that linked the loader
# would carry a second copy of it.
add_library(torquebus_plugin_api INTERFACE)
add_library(torquebus::plugin_api ALIAS torquebus_plugin_api)

target_include_directories(torquebus_plugin_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_plugin_api
    INTERFACE torquebus::core
              torquebus::drivers
)

# ---------------------------------------------------------------------------
# torquebus_services - application services (settings, projects, workspaces)
# ---------------------------------------------------------------------------

add_library(torquebus_services STATIC
    services/ProjectFile.h
    services/ProjectFile.cpp
    services/HardwareProfile.h
    services/HardwareProfile.cpp
    services/RecentProjects.h
    services/RecentProjects.cpp
    services/Workspaces.h
    services/Workspaces.cpp
    services/SettingsStore.h
    services/SettingsStore.cpp
)

add_library(torquebus::services ALIAS torquebus_services)

target_include_directories(torquebus_services PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_services
    PUBLIC  torquebus::core
            Qt6::Core
    PRIVATE torquebus::warnings
)

set_target_properties(torquebus_services PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# torquebus_theme - the colour vocabulary, and nothing that draws
# ---------------------------------------------------------------------------
#
# Split out of torquebus_ui so it can be tested. The accent colour is derived
# by arithmetic - a hue, a contrast search, a pair of tones - and arithmetic
# that decides what the window looks like is arithmetic worth having tests for.
# Linking the whole UI into the test binary to reach it would drag in Widgets,
# KDDockWidgets and QtNodes to check a number.
#
# Qt6::Gui and no further: QColor is a value type and none of this needs a
# QGuiApplication, which is what lets the tests run headless in CI.

add_library(torquebus_theme STATIC
    ui/theme/Theme.h
    ui/theme/Theme.cpp
    ui/theme/AccentColor.h
    ui/theme/AccentColor.cpp
    ui/theme/Contrast.h
    ui/theme/Contrast.cpp
)

add_library(torquebus::theme ALIAS torquebus_theme)

target_include_directories(torquebus_theme PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_theme
    PUBLIC  Qt6::Gui
    PRIVATE torquebus::warnings
)

set_target_properties(torquebus_theme PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# torquebus_ui - widgets, theming and docking
# ---------------------------------------------------------------------------

qt_add_library(torquebus_ui STATIC
    ui/engine/CanEngineController.h
    ui/engine/CanEngineController.cpp
    ui/theme/ThemeManager.h
    ui/theme/ThemeManager.cpp
    ui/mainwindow/Docking.h
    ui/mainwindow/Docking.cpp
    ui/mainwindow/DockChrome.h
    ui/mainwindow/DockChrome.cpp
    ui/common/Motion.h
    ui/common/AnimatedToolButton.h
    ui/common/AnimatedToolButton.cpp
    ui/mainwindow/MainWindow.h
    ui/mainwindow/MainWindow.cpp
    ui/project/ProjectExplorerPanel.h
    ui/project/ProjectExplorerPanel.cpp
    ui/properties/NodePropertiesEditor.h
    ui/properties/NodePropertiesEditor.cpp
    ui/properties/PropertiesPanel.h
    ui/properties/PropertiesPanel.cpp
    ui/output/OutputPanel.h
    ui/output/OutputPanel.cpp
    ui/canvas/PipelineGraphModel.h
    ui/canvas/PipelineGraphModel.cpp
    ui/canvas/CanvasPanel.h
    ui/canvas/CanvasPanel.cpp
    ui/database/DatabasePanel.h
    ui/database/DatabasePanel.cpp
    ui/hardware/HardwareDialog.h
    ui/hardware/HardwareDialog.cpp
    ui/diagnostics/DiagnosticsPanel.h
    ui/diagnostics/DiagnosticsPanel.cpp
    ui/scripting/ScriptEdit.h
    ui/scripting/ScriptEdit.cpp
    ui/scripting/LuaHighlighter.h
    ui/scripting/LuaHighlighter.cpp
    ui/scripting/ScriptEditorPanel.h
    ui/scripting/ScriptEditorPanel.cpp
    ui/testing/TestPanel.h
    ui/testing/TestPanel.cpp
    ui/j1939/J1939NetworkPanel.h
    ui/j1939/J1939NetworkPanel.cpp
    ui/plugins/PluginsDialog.h
    ui/plugins/PluginsDialog.cpp
    ui/dashboard/DashboardPanel.h
    ui/dashboard/DashboardPanel.cpp
    ui/dashboard/DashboardWidgetEditor.h
    ui/dashboard/DashboardWidgetEditor.cpp
    ui/graph/PlotView.h
    ui/graph/PlotView.cpp
    ui/graph/GraphPanel.h
    ui/graph/GraphPanel.cpp
    ui/playback/TimelineBar.h
    ui/playback/TimelineBar.cpp
    ui/playback/PlaybackPanel.h
    ui/playback/PlaybackPanel.cpp
    ui/preferences/AccentSwatchRow.h
    ui/preferences/AccentSwatchRow.cpp
    ui/preferences/PreferencesDialog.h
    ui/preferences/PreferencesDialog.cpp
    ui/preferences/ThemeCard.h
    ui/preferences/ThemeCard.cpp
    ui/statistics/StatisticsPanel.h
    ui/statistics/StatisticsPanel.cpp
    ui/trace/TraceModel.h
    ui/trace/TraceModel.cpp
    ui/trace/TracePanel.h
    ui/trace/TracePanel.cpp
    ui/transmit/SignalValueDialog.h
    ui/transmit/SignalValueDialog.cpp
    ui/transmit/TransmitPanel.h
    ui/transmit/TransmitPanel.cpp
)

add_library(torquebus::ui ALIAS torquebus_ui)

qt_add_resources(torquebus_ui "torquebus_resources"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/themes/torquebus.qss"
)

# The icon set is large enough to be worth globbing, and it is data rather than
# code: a new .svg should not require editing this file.
file(GLOB TORQUEBUS_ICON_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icons/*.svg")

qt_add_resources(torquebus_ui "torquebus_icons"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES ${TORQUEBUS_ICON_FILES}
)

target_include_directories(torquebus_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_ui
    PUBLIC  torquebus::core
            torquebus::plugin_host
            torquebus::drivers
            torquebus::services
            torquebus::theme
            Qt6::Widgets
            Qt6::Svg
            Qt6::SvgWidgets
            KDAB::kddockwidgets
            QtNodes::QtNodes
    PRIVATE torquebus::warnings
)

set_target_properties(torquebus_ui PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# TorqueBusStudio - the executable
# ---------------------------------------------------------------------------

qt_add_executable(TorqueBusStudio
    app/Main.cpp
    app/ApplicationContext.h
    app/ApplicationContext.cpp
)

target_link_libraries(TorqueBusStudio
    PRIVATE torquebus::ui
            torquebus::plugin_host
            torquebus::warnings
)

# ---------------------------------------------------------------------------
# Data files that ship beside the executable.
#
# Right now that is the J1939 function names, derived from AgIsoStack++ under
# its MIT licence. Copied rather than generated: the generator needs a source
# this build does not have, and a table that regenerated itself at build time
# would change under people who never asked it to.
#
# Deliberately not here: the manufacturer table. See J1939NameTables.h.
# ---------------------------------------------------------------------------

add_custom_command(TARGET TorqueBusStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:TorqueBusStudio>/data"
    COMMENT "Copying data files beside the executable"
    VERBATIM
)

# ---------------------------------------------------------------------------
# The vendor plugins.
#
# After the application target, because each adds itself as a dependency of it:
# nothing links them, and without that they would simply never be built.
# ---------------------------------------------------------------------------

add_subdirectory(plugins/driver-kvaser)
add_subdirectory(plugins/driver-peak)

target_compile_definitions(TorqueBusStudio
    PRIVATE TORQUEBUS_VERSION="${PROJECT_VERSION}"
)

set_target_properties(TorqueBusStudio PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
    OUTPUT_NAME "TorqueBusStudio"
    FOLDER "Applications"
)

# --- What goes into a package, and what emphatically does not --------------
#
# Everything TorqueBus installs is in the `torquebus` component, and the
# packaging step asks for that component by name. The reason is that
# KDDockWidgets and QtNodes arrive through FetchContent, which means their own
# install() rules are ours to run: a plain `cmake --install` writes 139 headers,
# two import libraries and four CMake config files of somebody else's SDK into
# what is supposed to be an application for end users.
#
# A named component is the version-independent way to say "only mine".
# EXCLUDE_FROM_ALL would not do it - it excludes targets from the build, not
# install rules from the install.

# ---------------------------------------------------------------------------
# The layering rules, checked rather than trusted.
#
# ARCHITECTURE.md section 1 freezes these. They are here, at the bottom of the
# file, because every target they talk about has to exist first.
# ---------------------------------------------------------------------------

torquebus_forbid_link(torquebus_core
    RULE "rule 3: the core does not depend on widgets - it must stay linkable into a headless logger, a CLI and a test binary"
    DENY Qt6::Widgets Qt6::Gui Qt6::Core KDAB::kddockwidgets QtNodes::QtNodes
)

torquebus_forbid_link(torquebus_drivers
    RULE "rule 2: no driver knows the UI exists - a backend is a translation layer, not a feature"
    DENY Qt6::Widgets Qt6::Gui KDAB::kddockwidgets QtNodes::QtNodes torquebus::ui
)

# Rule 1 from the other side: the UI reaches hardware through the registry, so
# no concrete backend may be linked into it. The vendor backends are plugins
# now, which is what makes this checkable at all.
torquebus_forbid_link(torquebus_ui
    RULE "rule 1: no UI code talks to hardware directly"
    DENY torquebus::driver_kvaser torquebus::driver_peak
)

# ---------------------------------------------------------------------------
# Qt, beside the executable in the build tree
# ---------------------------------------------------------------------------
#
# So that the thing that was just built actually starts.
#
# Until this, running TorqueBusStudio.exe out of build/<preset>/bin needed Qt on
# PATH. Everybody here has it, so nobody noticed - and the README tells a
# newcomer to build with windows-msvc-vs "from any prompt, with no setup at
# all", which is true of the build and was not true of the result. What they got
# was a successful build and a Windows dialog: "Qt6Cored.dll was not found", or
# worse, the silent one where a *different* Qt earlier on PATH loads and the
# entry points do not match.
#
# Two copies, because they answer different questions.
#
# $<TARGET_RUNTIME_DLLS> knows what the executable links - Qt6Core, Qt6Gui,
# Qt6Widgets, Qt6Svg - and nothing else. It does not know about Qt's plugins,
# because nothing links them: Qt loads them by path at runtime. A build tree
# with the DLLs and no platform plugin starts and immediately dies with "could
# not find or load the Qt platform plugin windows", which reads like a broken Qt
# installation and is not.
#
# The plugin list is short and deliberate. The platform plugin is what makes a
# window; without the two SVG ones every icon in the application is blank, since
# the whole icon set is SVG; the style plugin is what stops it looking like
# Windows 95. Each is guarded, so a Qt built without one is a smaller
# deployment rather than a configure error.

if(WIN32)
    add_custom_command(TARGET TorqueBusStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:TorqueBusStudio>"
                "$<TARGET_FILE_DIR:TorqueBusStudio>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying the Qt runtime beside the executable"
        VERBATIM
    )

    # plugin target -> the subdirectory Qt looks for it in
    set(TORQUEBUS_QT_PLUGINS
        "Qt6::QWindowsIntegrationPlugin"     platforms
        "Qt6::QSvgPlugin"                    imageformats
        "Qt6::QSvgIconPlugin"                iconengines
        "Qt6::QModernWindowsStylePlugin"     styles
    )

    while(TORQUEBUS_QT_PLUGINS)
        list(POP_FRONT TORQUEBUS_QT_PLUGINS _plugin _directory)

        if(NOT TARGET "${_plugin}")
            continue()
        endif()

        add_custom_command(TARGET TorqueBusStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:TorqueBusStudio>/${_directory}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_plugin}>"
                    "$<TARGET_FILE_DIR:TorqueBusStudio>/${_directory}"
            COMMENT "Copying ${_plugin} into ${_directory}"
            VERBATIM
        )
    endwhile()
endif()

install(TARGETS TorqueBusStudio
    RUNTIME DESTINATION . COMPONENT torquebus
    BUNDLE  DESTINATION . COMPONENT torquebus
)

# The same data directory the build tree gets, for the same reason: a packaged
# build with no name table shows bare numbers where a function name belongs,
# and nothing about that looks like a missing file.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/"
    DESTINATION data COMPONENT torquebus

    # Except that one. j1939-names.csv is the manufacturer table, built from a
    # licensed Digital Annex or from a registry that grants no licence, and it
    # is not ours to put in a package. .gitignore keeps it out of the repository;
    # this keeps it out of a release built on a machine where somebody generated
    # it - which is a likely machine, because tools/j1939-names.py tells people
    # to write it to a directory named data.
    PATTERN "j1939-names.csv" EXCLUDE
)

# The licence travels with the binary, and it does so from here rather than
# from a line in a packaging workflow: GPLv3 compliance is a property of the
# thing we hand people, not a step somebody remembers to keep.
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION . COMPONENT torquebus
    RENAME LICENSE.txt
)

install(FILES "${CMAKE_SOURCE_DIR}/README.md"
              "${CMAKE_SOURCE_DIR}/CHANGELOG.md"
    DESTINATION . COMPONENT torquebus
)
