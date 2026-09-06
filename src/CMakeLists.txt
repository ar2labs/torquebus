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

    core/scripting/LuaRuntime.h
    core/scripting/LuaRuntime.cpp
    core/scripting/LuaEcuNode.h
    core/scripting/LuaEcuNode.cpp

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

    # Always compiled, with or without the SDK: without it the .cpp builds a
    # stub that reports itself unavailable, so the registry, the Hardware
    # Manager and the tests are identical in both builds.
    drivers/kvaser/KvaserCanBackend.h
    drivers/kvaser/KvaserCanBackend.cpp

    # Same arrangement, different route to it: without Qt SerialBus the .cpp
    # builds the stub instead of the peakcan-backed implementation.
    drivers/peak/PeakCanBackend.h
    drivers/peak/PeakCanBackend.cpp
)

add_library(torquebus_drivers STATIC ${TORQUEBUS_DRIVER_SOURCES})
add_library(torquebus::drivers ALIAS torquebus_drivers)

target_include_directories(torquebus_drivers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(torquebus_drivers
    PUBLIC  torquebus::core
    PRIVATE torquebus::warnings
)

if(TORQUEBUS_HAVE_KVASER)
    target_include_directories(torquebus_drivers PRIVATE "${KVASER_CANLIB_INCLUDE_DIR}")
    target_link_libraries(torquebus_drivers PRIVATE "${KVASER_CANLIB_LIBRARY}")
    target_compile_definitions(torquebus_drivers PRIVATE TORQUEBUS_HAVE_KVASER=1)
endif()

if(TORQUEBUS_HAVE_PEAK)
    # PCAN-Basic is reached through the Qt SerialBus peakcan plugin, which
    # loads PCANBasic.dll at runtime - nothing is linked statically.
    target_link_libraries(torquebus_drivers PRIVATE Qt6::SerialBus)
    target_compile_definitions(torquebus_drivers PRIVATE TORQUEBUS_HAVE_PEAK=1)
endif()

set_target_properties(torquebus_drivers PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# torquebus_services - application services (settings, projects, workspaces)
# ---------------------------------------------------------------------------

add_library(torquebus_services STATIC
    services/ProjectFile.h
    services/ProjectFile.cpp
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
    ui/mainwindow/PlaceholderPanel.h
    ui/mainwindow/PlaceholderPanel.cpp
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
            torquebus::warnings
)

target_compile_definitions(TorqueBusStudio
    PRIVATE TORQUEBUS_VERSION="${PROJECT_VERSION}"
)

set_target_properties(TorqueBusStudio PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
    OUTPUT_NAME "TorqueBusStudio"
    FOLDER "Applications"
)

install(TARGETS TorqueBusStudio
    RUNTIME DESTINATION .
    BUNDLE  DESTINATION .
)
