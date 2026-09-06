// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The application shell: menus, toolbar, status bar and the docking area that
// holds every panel.
//
// The window owns no measurement state. It creates panels, wires their signals
// and asks services to do work; it never talks to hardware (rule #1) and never
// blocks waiting for it (rule #6).

#pragma once

#include "core/can/CanTypes.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "services/HardwareProfile.h"
#include "services/RecentProjects.h"
#include "ui/engine/CanEngineController.h"
#include "ui/mainwindow/Docking.h"
#include "ui/theme/Theme.h"

#include <QList>
#include <QString>
#include <QVector>

#include <cstdint>

class QAction;
class QLabel;
class QMenu;

namespace torquebus::services {
class SettingsStore;
}

namespace torquebus::ui {

class CanvasPanel;
class DatabasePanel;
class GraphPanel;
class OutputPanel;
class PlaybackPanel;
class ProjectExplorerPanel;
class NodePropertiesEditor;
class PropertiesPanel;
class StatisticsPanel;
class ThemeManager;
class TracePanel;
class TransmitPanel;

class MainWindow final : public DockMainWindowBase {
    Q_OBJECT

public:
    MainWindow(services::SettingsStore& settings, ThemeManager& themes);
    ~MainWindow() override;

    /// Opens a project, reporting failure in the Output panel.
    ///
    /// Public because the command line accepts a .tbsproj, and honouring an
    /// argument the help text advertises is not optional.
    void openProject(const QString& path);

    /// Rescans every registered backend and refreshes the Hardware section.
    void refreshHardware();

    /// Arranges the panels the way a fresh installation does, discarding any
    /// saved arrangement.
    void applyDefaultLayout();

protected:
    void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
    void onThemeChanged(const torquebus::ui::Theme& theme);
    void onDeviceSelected(const torquebus::CanDeviceInfo& device);
    void onToggleTheme();
    void onPreferences();

    /// Hardware > Configuration. Edits which interfaces become CAN 1..N.
    void onHardwareConfiguration();
    void onAbout();
    void onNotImplemented();
    void onInspectChrome();

    /// A node was selected on the canvas; show its settings.
    void onCanvasNodeSelected(const QString& descriptionId);

    /// The pipeline was edited. Says in the Output panel whether it still
    /// builds, so a wire that cannot work is reported while the user is looking
    /// at it rather than at the next Start.
    void onGraphEdited();

    void onNewProject();
    void onOpenProject();

    /// File > Import Database. Loads a .dbc into the DBC Explorer.
    void onImportDatabase();

    /// File > Export Trace. Writes what the trace is holding as ASC or CSV.
    void onExportTrace();

    void onSaveProject();
    void onSaveProjectAs();

    void onRecord(bool checked);
    void onStartMeasurement();
    void onStopMeasurement();
    void onMeasurementStarted();
    void onMeasurementStopped();
    void onMeasurementFailed(const QString& message);
    void onStatusUpdated(const QList<torquebus::ui::ChannelStatus>& channels);
    void onFrameCountChanged(quint64 total);

private:
    void createPanels();
    void createActions();
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void createEngine();

    /// Rebuilds the Open Recent submenu from the stored list. Called whenever
    /// that list changes, which is on every open, save and failed open.
    void rebuildRecentMenu();

    /// Reports, to the Output panel, which theme and which resources the
    /// running binary actually loaded. See the definition for why.
    void reportThemeDiagnostics();

    /// Saves to `path`, reporting either way. Used by Save and Save As.
    bool writeProject(const QString& path);

    /// Marks the pipeline changed and refreshes the title bar.
    void markDirty();

    /// Asks about unsaved work before something discards it.
    ///
    /// Returns false when the user cancels, in which case the caller must not
    /// proceed. Returns true when there was nothing to lose, when the work was
    /// saved, or when the user chose to discard it.
    [[nodiscard]] bool confirmDiscardChanges();

    /// Puts the project's name in the title bar, so two windows on two
    /// projects are telling apart at a glance.
    void updateWindowTitle();

    void saveWindowState() const;
    void restoreWindowState();

    /// Sets one of the status-bar channel indicators. `state` matches the
    /// torquebusState property values in the style sheet.
    void setChannelIndicator(int index, const QString& text, const QString& state);

    services::SettingsStore& m_settings;
    ThemeManager& m_themes;

    CanEngineController* m_controller{nullptr};

    // --- Panels -----------------------------------------------------------
    ProjectExplorerPanel* m_projectExplorer{nullptr};
    PropertiesPanel* m_properties{nullptr};
    NodePropertiesEditor* m_nodeProperties{nullptr};
    OutputPanel* m_output{nullptr};
    TracePanel* m_tracePanel{nullptr};
    CanvasPanel* m_canvas{nullptr};
    DatabasePanel* m_databasePanel{nullptr};
    TransmitPanel* m_transmitPanel{nullptr};
    StatisticsPanel* m_statisticsPanel{nullptr};
    GraphPanel* m_graphPanel{nullptr};
    PlaybackPanel* m_playbackPanel{nullptr};

    // --- The pipeline the canvas edits ------------------------------------
    //
    // Owned by the window because it is the project's, not the canvas's: the
    // description outlives the panel, so closing the canvas does not discard
    // the pipeline, and the engine keeps building from it.
    NodeCatalog m_catalog{NodeCatalog::withBuiltinTypes()};

    /// The transmit list, owned here for the same reason the pipeline is: it is
    /// the project's, it outlives the panel that edits it, and the engine
    /// borrows it rather than copying it - so a row changed mid-measurement
    /// takes effect on the next pass.
    TransmitList m_transmitList;
    GraphDescription m_pipeline;

    /// Where the open project lives, or empty for one never saved.
    QString m_projectPath;

    /// The projects this user has opened, and the menu that shows them.
    services::RecentProjects m_recentProjects;

    /// Which interfaces become CAN 1..N, and how each is opened. Belongs to the
    /// machine rather than to the project, which is why it is here and not in
    /// the .tbsproj.
    services::HardwareProfile m_hardware;
    QMenu* m_recentMenu{nullptr};

    /// The log this run is recording into, or empty when it is not. Kept so the
    /// Output panel can name the file at Stop, which is the moment somebody
    /// wants to know where it went.
    QString m_recordingPath;

    /// Wall clock at the last Start, in microseconds since the epoch, or zero
    /// when nothing has been measured in this session. Only the ASC date line
    /// reads it; every frame carries its own timestamp.
    std::uint64_t m_measurementStartUs{0};

    /// The pipeline has changed since it was last saved or opened.
    ///
    /// Adding project files created a way to lose work that did not exist
    /// before: File > New on an unsaved pipeline used to discard it silently.
    /// This is the flag that a prompt can honestly be based on - one that only
    /// appears when something would actually be lost.
    bool m_dirty{false};

    // --- Docks, in creation order -----------------------------------------
    DockWidget* m_projectDock{nullptr};
    DockWidget* m_propertiesDock{nullptr};
    DockWidget* m_nodePropertiesDock{nullptr};
    DockWidget* m_traceDock{nullptr};
    DockWidget* m_databaseDock{nullptr};
    DockWidget* m_pipelineDock{nullptr};
    DockWidget* m_transmitDock{nullptr};
    DockWidget* m_graphDock{nullptr};
    DockWidget* m_playbackDock{nullptr};
    DockWidget* m_statisticsDock{nullptr};
    DockWidget* m_diagnosticsDock{nullptr};
    DockWidget* m_outputDock{nullptr};

    QVector<DockWidget*> m_allDocks;

    // --- Actions ----------------------------------------------------------
    QAction* m_actionNewProject{nullptr};
    QAction* m_actionOpenProject{nullptr};
    QAction* m_actionSaveProject{nullptr};
    QAction* m_actionSaveProjectAs{nullptr};
    QAction* m_actionImportDatabase{nullptr};
    QAction* m_actionExportTrace{nullptr};
    QAction* m_actionExit{nullptr};

    QAction* m_actionStart{nullptr};
    QAction* m_actionStop{nullptr};
    QAction* m_actionRecord{nullptr};
    QAction* m_actionReplay{nullptr};

    QAction* m_actionHardwareConfiguration{nullptr};
    QAction* m_actionRefreshHardware{nullptr};

    QAction* m_actionToggleTheme{nullptr};
    QAction* m_actionPreferences{nullptr};
    QAction* m_actionResetLayout{nullptr};
    QAction* m_actionInspectChrome{nullptr};
    QAction* m_actionAbout{nullptr};
    QAction* m_actionAboutQt{nullptr};

    // --- Status bar -------------------------------------------------------
    QLabel* m_channel1Label{nullptr};
    QLabel* m_channel2Label{nullptr};
    QLabel* m_frameCountLabel{nullptr};
    QLabel* m_frameRateLabel{nullptr};
    QLabel* m_busLoadLabel{nullptr};
    QLabel* m_recordingLabel{nullptr};

    CanDeviceInfoList m_devices;
};

} // namespace torquebus::ui
