# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Dashboard's instrument cluster. Everything that is the cluster's lives under this directory:
# its C++ beside this file, its QML in qml/, and the tool for working on it in preview/. What cannot
# live here, and why:
#
#   - its colour roles, in ui/theme/Theme.*: the Theme is the one palette of the application;
#   - the icon provider, in ui/theme/IconImageProvider.*: ThemeManager shares the painting code;
#   - the DashboardWidgetKind it adds and where DashboardPanel handles it: those are the Dashboard's.
#
# Included from src/CMakeLists.txt after torquebus_ui, whose target it adds to.

# ---------------------------------------------------------------------------
# C++ for the Widgets side
# ---------------------------------------------------------------------------

target_sources(torquebus_ui PRIVATE
    ClusterDataSource.h
    ClusterDataSource.cpp
    ClusterHost.h
    ClusterHost.cpp
    ClusterThemeSync.h
    ClusterThemeSync.cpp
)

# ---------------------------------------------------------------------------
# The QML module, TorqueBus.Cluster
#
# A static library, like torquebus_ui itself, so the QML is compiled ahead of time and travels inside
# the executable. A static QML module comes with a plugin target, torquebus_clusterplugin, that
# whoever loads the cluster links, and names in Q_IMPORT_QML_PLUGIN(TorqueBus_ClusterPlugin) from
# one of its own sources: nothing finds a static plugin by itself.
#
# One convention is not optional: every QML file that uses a type from another directory of the
# module starts with `import TorqueBus.Cluster`. The compiled module does not need it, but the preview
# tool reads the files from the source tree, where a file sees only its own directory otherwise, and
# the reload that gives it would stop at "Lamp is not a type".
# ---------------------------------------------------------------------------

# Qt's current behaviour, asked for by name rather than by raising the project's minimum Qt:
# the module under /qt/qml, and a qmldir for each subdirectory.
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_library(torquebus_cluster STATIC)

qt_add_qml_module(torquebus_cluster
    URI TorqueBus.Cluster
    VERSION 1.0
    # Ends in the module's own path, as qmllint and the import paths of the tools expect.
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/TorqueBus/Cluster
    QML_FILES
        qml/ClusterFrame.qml
        qml/ClusterView.qml
        qml/common/ClusterTheme.qml
        qml/common/CText.qml
        qml/common/RadialGlow.qml
        qml/instruments/AiCore.qml
        qml/instruments/BarGauge.qml
        qml/instruments/RadialGauge.qml
        qml/instruments/SegmentRing.qml
        qml/telltales/Lamp.qml
        qml/telltales/TelltaleIcon.qml
        qml/telltales/TurnArrow.qml
)

target_link_libraries(torquebus_cluster PUBLIC Qt6::Quick)

set_target_properties(torquebus_cluster PROPERTIES FOLDER "Libraries")

# ---------------------------------------------------------------------------
# preview/: a window for working on the cluster without the rest of TorqueBus
#
# The real Theme, the real icon set behind the real icon provider, and a pretend vehicle
# (qml/preview/FakeVehicle.qml) where the bus will be. It reloads when a .qml file is saved. See
# preview/ClusterPreviewMain.cpp.
#
# It is part of the normal build, so CI compiles it with the same warnings as the rest and it cannot
# go stale unnoticed. It is not installed: it is a tool for whoever edits the QML.
#
# The QML is read from the source tree, which is what lets it reload; the path is fixed here, as the
# paths the tests use are. Qt's own QML modules are the other half: they are not linked, so
# $<TARGET_RUNTIME_DLLS> does not know about them, and windeployqt - the same tool that builds the
# package - puts them beside the executable, found by scanning the cluster's QML for what it imports.
# That keeps the program runnable from any terminal, and means the QML deployment the package will
# need is exercised on every build.
# ---------------------------------------------------------------------------

add_executable(torquebus_cluster_preview
    preview/ClusterPreviewMain.cpp
)

target_link_libraries(torquebus_cluster_preview
    PRIVATE torquebus::ui
            torquebus_cluster
            torquebus_clusterplugin
            Qt6::Quick
            torquebus::warnings
)

target_compile_definitions(torquebus_cluster_preview PRIVATE
    TORQUEBUS_CLUSTER_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

set_target_properties(torquebus_cluster_preview PROPERTIES FOLDER "Tools")

if(WIN32 AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET torquebus_cluster_preview POST_BUILD
        COMMAND Qt6::windeployqt
                --no-translations --no-system-d3d-compiler --no-compiler-runtime
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                "$<TARGET_FILE:torquebus_cluster_preview>"
        COMMENT "Deploying the Qt runtime and QML modules beside the cluster preview"
        VERBATIM
    )
endif()
