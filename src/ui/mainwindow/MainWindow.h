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
#include "ui/engine/CanEngineController.h"
#include "ui/mainwindow/Docking.h"
#include "ui/theme/Theme.h"

#include <QList>
#include <QString>
#include <QVector>

class QAction;
class QLabel;

namespace torquebus::services {
class SettingsStore;
}

namespace torquebus::ui {

class CanvasPanel;
class OutputPanel;
class ProjectExplorerPanel;
class PropertiesPanel;
class ThemeManager;
class TracePanel;

class MainWindow final : public DockMainWindowBase {
    Q_OBJECT

public:
    MainWindow(services::SettingsStore& settings, ThemeManager& themes);
    ~MainWindow() override;

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
    void onAbout();
    void onNotImplemented();
    void onInspectChrome();

    /// A node was selected on the canvas; show its settings.
    void onCanvasNodeSelected(const QString& descriptionId);

    /// The pipeline was edited. Says in the Output panel whether it still
    /// builds, so a wire that cannot work is reported while the user is looking
    /// at it rather than at the next Start.
    void onGraphEdited();

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

    /// Reports, to the Output panel, which theme and which resources the
    /// running binary actually loaded. See the definition for why.
    void reportThemeDiagnostics();

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
    OutputPanel* m_output{nullptr};
    TracePanel* m_tracePanel{nullptr};
    CanvasPanel* m_canvas{nullptr};

    // --- The pipeline the canvas edits ------------------------------------
    //
    // Owned by the window because it is the project's, not the canvas's: the
    // description outlives the panel, so closing the canvas does not discard
    // the pipeline, and the engine keeps building from it.
    NodeCatalog m_catalog{NodeCatalog::withBuiltinTypes()};
    GraphDescription m_pipeline;

    // --- Docks, in creation order -----------------------------------------
    DockWidget* m_projectDock{nullptr};
    DockWidget* m_propertiesDock{nullptr};
    DockWidget* m_traceDock{nullptr};
    DockWidget* m_pipelineDock{nullptr};
    DockWidget* m_transmitDock{nullptr};
    DockWidget* m_graphDock{nullptr};
    DockWidget* m_statisticsDock{nullptr};
    DockWidget* m_diagnosticsDock{nullptr};
    DockWidget* m_outputDock{nullptr};

    QVector<DockWidget*> m_allDocks;

    // --- Actions ----------------------------------------------------------
    QAction* m_actionNewProject{nullptr};
    QAction* m_actionOpenProject{nullptr};
    QAction* m_actionSaveProject{nullptr};
    QAction* m_actionExit{nullptr};

    QAction* m_actionStart{nullptr};
    QAction* m_actionStop{nullptr};
    QAction* m_actionRecord{nullptr};
    QAction* m_actionReplay{nullptr};

    QAction* m_actionHardwareConfiguration{nullptr};
    QAction* m_actionRefreshHardware{nullptr};

    QAction* m_actionToggleTheme{nullptr};
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
