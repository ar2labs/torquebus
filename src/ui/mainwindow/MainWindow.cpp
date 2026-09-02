// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/mainwindow/MainWindow.h"

#include "drivers/api/CanBackendRegistry.h"
#include "services/ProjectFile.h"
#include "services/SettingsStore.h"
#include "ui/canvas/CanvasPanel.h"
#include "ui/mainwindow/PlaceholderPanel.h"
#include "ui/output/OutputPanel.h"
#include "ui/project/ProjectExplorerPanel.h"
#include "ui/properties/NodePropertiesEditor.h"
#include "ui/properties/PropertiesPanel.h"
#include "ui/theme/ThemeManager.h"
#include "ui/trace/TracePanel.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QColor>
#include <QFontMetrics>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QPair>
#include <QSize>
#include <QStringList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>

#include <algorithm>

namespace torquebus::ui {
namespace {

// Unique dock names are part of the persisted layout format. Changing one
// invalidates every workspace saved by an earlier build, so they are frozen
// here and never inlined at the call site.
constexpr auto kDockProject     = "torquebus.dock.project";
constexpr auto kDockProperties  = "torquebus.dock.properties";
constexpr auto kDockBlock       = "torquebus.dock.block";
constexpr auto kDockTrace       = "torquebus.dock.trace";
constexpr auto kDockPipeline    = "torquebus.dock.pipeline";
constexpr auto kDockTransmit    = "torquebus.dock.transmit";
constexpr auto kDockGraph       = "torquebus.dock.graph";
constexpr auto kDockStatistics  = "torquebus.dock.statistics";
constexpr auto kDockDiagnostics = "torquebus.dock.diagnostics";
constexpr auto kDockOutput      = "torquebus.dock.output";

// Starting geometry of the default arrangement. Wide enough for a channel name
// and a bitrate without wrapping, narrow enough that the trace keeps the room.
constexpr int kSidePanelWidth = 260;
constexpr int kSidePanelMinimumWidth = 180;
constexpr int kConsoleHeight = 160;

/// Status indicator bullet. The colour carries the state (see the
/// torquebusState rules in the style sheet); the glyph just gives it a shape,
/// so the indicator still reads as an indicator in a screenshot printed in
/// black and white - or to someone who cannot tell the green from the amber.
constexpr auto kBullet = "●";

/// Schema version of the default panel arrangement produced by
/// applyDefaultLayout(). Bump this whenever that arrangement changes in a way
/// existing users should actually receive - a saved layout from an older
/// version is discarded instead of being restored over the new default.
///
///   1  v0.1  first arrangement (centre panel added last - wrong)
///   2  v0.2  centre panel added first, sized side panels, full-width console
constexpr int kDockLayoutVersion = 4;

/// Converts one of the frozen dock names above into a QString.
///
/// Not QStringLiteral: that macro pastes its argument next to a `u""` prefix,
/// so it only accepts a literal spelled out at the call site. Feeding it a
/// named constant is a syntax error - and inlining the names to satisfy it
/// would scatter the persisted layout keys across the file, which is exactly
/// what keeping them in one place above is meant to prevent.
[[nodiscard]] QString dockName(const char* name)
{
    return QString::fromLatin1(name);
}

QLabel* createStatusLabel(const QString& text, const QString& state)
{
    auto* label = new QLabel(text);
    label->setProperty("torquebusState", state);
    return label;
}

/// A numeric readout: monospaced, and wide enough for the largest value it will
/// ever show.
///
/// Without the reserved width the whole status bar shifts sideways every time a
/// frame counter gains a digit. A bar that twitches cannot be read at a glance,
/// and a glance is the only way anyone reads one.
QLabel* createMetricLabel(const QString& text, int reservedCharacters)
{
    auto* label = new QLabel(text);
    label->setProperty("torquebusRole", "metric");
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    const QFontMetrics metrics{label->font()};
    label->setMinimumWidth(metrics.horizontalAdvance(QLatin1Char('0')) * reservedCharacters);

    return label;
}

/// Re-evaluates the style sheet for a widget whose dynamic property changed.
/// Qt does not do this on its own.
void repolish(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QFrame* createStatusSeparator()
{
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    return separator;
}

} // namespace

MainWindow::MainWindow(services::SettingsStore& settings, ThemeManager& themes)
    : DockMainWindowBase{QStringLiteral("torquebus.mainwindow")}
    , m_settings{settings}
    , m_themes{themes}
{
    setWindowIcon(QIcon{QStringLiteral(":/icons/torquebus.svg")});
    resize(1440, 900);

    createEngine();
    createPanels();
    createActions();
    createMenus();
    createToolBar();
    createStatusBar();

    connect(&m_themes, &ThemeManager::themeChanged, this, &MainWindow::onThemeChanged);

    // After the panels exist, because it is the project's name that goes in the
    // title and an unsaved one still has to say so.
    updateWindowTitle();

    restoreWindowState();
    refreshHardware();

    m_output->appendInfo(tr("TorqueBus Studio %1 started.")
                             .arg(QApplication::applicationVersion()));

    reportThemeDiagnostics();

    for (const QString& description : m_controller->boundChannelDescriptions()) {
        m_output->appendInfo(description);
    }

    m_output->appendInfo(tr("Press Start (F5) to go bus-on."));
}

MainWindow::~MainWindow()
{
    // m_pipeline and m_catalog are members of this window, and the canvas holds
    // a QtNodes scene built on top of them. Members are destroyed before
    // ~QWidget deletes the child widgets, so by the time the canvas goes the
    // description it is built on has already gone.
    //
    // The canvas is told to let go here, while everything it points at is
    // still alive. It is not deleted: it is a guest widget inside a
    // KDDockWidgets dock, and deleting it early would leave the dock holding a
    // view onto a destroyed widget - the same bug moved rather than fixed.
    //
    // The block editor needs nothing: its destructor touches only its own
    // widgets.
    if (m_canvas != nullptr) {
        m_canvas->releaseGraph();
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void MainWindow::reportThemeDiagnostics()
{
    // Styling lives in two places that a stale build breaks independently: the
    // palette is compiled C++, the style sheet and icons are Qt resources. When
    // an edit to either appears to have "done nothing", the first question is
    // always whether the running binary actually contains it - and that is not
    // answerable by looking at the window.
    //
    // So the application states, at startup, what it loaded. It costs three
    // lines in the Output panel and removes an entire category of guesswork.

    const Theme& theme = m_themes.theme();

    m_output->appendInfo(tr("Theme: %1  (accent %2, panel %3)")
                             .arg(theme.name,
                                  theme.accent.name(QColor::HexRgb),
                                  theme.panel.name(QColor::HexRgb)));

    const int sheetBytes = m_themes.styleSheetBytes();
    if (sheetBytes < 0) {
        m_output->appendError(
            tr("Style sheet resource could not be loaded. The window is running on the "
               "palette alone. Re-run CMake configure so the Qt resources are rebuilt."));
    } else {
        m_output->appendInfo(tr("Style sheet: %L1 bytes loaded from resources.")
                                 .arg(sheetBytes));
    }

    // The chevrons are the newest icons in the set, so they are the ones a
    // stale resource bundle will be missing.
    const QStringList requiredIcons{
        QStringLiteral("chevron-left-on-dark"),
        QStringLiteral("chevron-right-on-dark"),
        QStringLiteral("chevron-left-on-light"),
        QStringLiteral("chevron-right-on-light"),
    };

    QStringList missing;
    for (const QString& name : requiredIcons) {
        if (!ThemeManager::hasIconResource(name)) {
            missing.append(name);
        }
    }

    if (!missing.isEmpty()) {
        m_output->appendError(
            tr("%n icon(s) missing from the compiled resources, starting with '%1'. "
               "Tab scroll arrows will be blank. Re-run CMake configure: new icon files "
               "are picked up by a glob, which only re-runs at configure time.",
               nullptr, static_cast<int>(missing.size()))
                .arg(missing.first()));
    }
}

void MainWindow::createEngine()
{
    m_controller = new CanEngineController(this);

    // A script's log_message(), and any script error, reach the Output panel.
    m_controller->engine().addLogSink(
        [this](const std::string& text, bool isError) {
            // Called on the engine thread. Queued, because a panel must only be
            // touched from the GUI thread (rule #6 in spirit: the UI never
            // blocks the measurement, and the measurement never reaches into
            // the UI).
            QMetaObject::invokeMethod(
                this,
                [this, message = QString::fromStdString(text), isError] {
                    if (m_output == nullptr) {
                        return;
                    }

                    if (isError) {
                        m_output->appendError(message);
                    } else {
                        m_output->appendInfo(message);
                    }
                },
                Qt::QueuedConnection);
        });

    connect(m_controller, &CanEngineController::started,
            this, &MainWindow::onMeasurementStarted);
    connect(m_controller, &CanEngineController::stopped,
            this, &MainWindow::onMeasurementStopped);
    connect(m_controller, &CanEngineController::failed,
            this, &MainWindow::onMeasurementFailed);
    connect(m_controller, &CanEngineController::statusUpdated,
            this, &MainWindow::onStatusUpdated);
    connect(m_controller, &CanEngineController::frameCountChanged,
            this, &MainWindow::onFrameCountChanged);
}

void MainWindow::createPanels()
{
    m_projectExplorer = new ProjectExplorerPanel;
    m_properties = new PropertiesPanel;
    m_output = new OutputPanel;

    connect(m_projectExplorer, &ProjectExplorerPanel::deviceSelected,
            this, &MainWindow::onDeviceSelected);
    connect(m_projectExplorer, &ProjectExplorerPanel::selectionCleared,
            m_properties, &PropertiesPanel::clearProperties);

    const auto icon = [this](const char* name) {
        return m_themes.icon(QString::fromLatin1(name));
    };

    m_projectDock = createDockWidget(dockName(kDockProject), tr("Project Explorer"),
                                     m_projectExplorer, icon("project"));

    m_propertiesDock = createDockWidget(dockName(kDockProperties), tr("Properties"),
                                        m_properties, icon("properties"));

    m_outputDock = createDockWidget(dockName(kDockOutput), tr("Output"),
                                    m_output, icon("console"));

    // The analysis panels exist from v0.1 as placeholders so that the docking
    // arrangement, the workspaces and the saved layouts are exercised against
    // the final set of panels rather than a subset that will change.
    // The trace reads the engine's store directly. It does not own it: the
    // measurement outlives any view of it, so closing this panel loses nothing.
    m_tracePanel = new TracePanel;
    m_tracePanel->setStore(&m_controller->engine().traceStore());

    m_traceDock = createDockWidget(dockName(kDockTrace), tr("CAN Trace"),
                                   m_tracePanel, icon("trace"));

    // The canvas edits m_pipeline in place - there is no apply step, and no
    // copy. Closing this panel leaves the pipeline exactly as it was; the
    // engine builds from the same description either way.
    m_canvas = new CanvasPanel(m_pipeline, m_catalog);

    connect(m_canvas, &CanvasPanel::nodeSelected,
            this, &MainWindow::onCanvasNodeSelected);
    connect(m_canvas, &CanvasPanel::graphEdited,
            this, &MainWindow::onGraphEdited);

    m_pipelineDock = createDockWidget(dockName(kDockPipeline), tr("Pipeline"),
                                      m_canvas, icon("graph"));

    // A second properties panel, tabbed with the first rather than replacing
    // it: the read-only one describes a hardware channel picked in the Project
    // Explorer, this one edits a block picked on the canvas. They answer
    // different questions and one would have to guess which was meant.
    m_nodeProperties = new NodePropertiesEditor(m_pipeline, m_catalog);

    connect(m_nodeProperties, &NodePropertiesEditor::nodeEdited,
            this, [this](const QString&) { onGraphEdited(); });

    m_nodePropertiesDock = createDockWidget(dockName(kDockBlock), tr("Block"),
                                            m_nodeProperties, icon("properties"));

    m_transmitDock = createDockWidget(
        dockName(kDockTransmit), tr("Transmit"),
        new PlaceholderPanel(tr("CAN Transmit"),
                             tr("Manual, periodic, one-shot, burst and sequence transmission "
                                "with editable payloads and presets."),
                             QStringLiteral("transmit"), QStringLiteral("v0.6")),
        icon("transmit"));

    m_graphDock = createDockWidget(
        dockName(kDockGraph), tr("Graph"),
        new PlaceholderPanel(tr("Signal Graph"),
                             tr("Live and offline signal plotting with cursors, multiple "
                                "Y axes, zoom, measurement and export."),
                             QStringLiteral("graph"), QStringLiteral("v0.8")),
        icon("graph"));

    m_statisticsDock = createDockWidget(
        dockName(kDockStatistics), tr("Statistics"),
        new PlaceholderPanel(tr("Bus Statistics"),
                             tr("Bus load, peak load, frame rates, error counters and "
                                "controller state per channel."),
                             QStringLiteral("statistics"), QStringLiteral("v0.8")),
        icon("statistics"));

    m_diagnosticsDock = createDockWidget(
        dockName(kDockDiagnostics), tr("Diagnostics"),
        new PlaceholderPanel(tr("Diagnostics"),
                             tr("ISO-TP transport and a UDS client: sessions, DIDs, DTCs "
                                "and a request/response console."),
                             QStringLiteral("diagnostics"), QStringLiteral("v0.12")),
        icon("diagnostics"));

    m_allDocks = {m_projectDock,     m_propertiesDock,  m_nodePropertiesDock,
                  m_traceDock,       m_pipelineDock,    m_transmitDock,
                  m_graphDock,       m_statisticsDock,  m_diagnosticsDock,
                  m_outputDock};
}

void MainWindow::createActions()
{
    const auto icon = [this](const char* name) {
        return m_themes.icon(QString::fromLatin1(name));
    };

    // --- File -------------------------------------------------------------
    m_actionNewProject = new QAction(icon("new"), tr("&New Project..."), this);
    m_actionNewProject->setShortcut(QKeySequence::New);

    m_actionOpenProject = new QAction(icon("open"), tr("&Open Project..."), this);
    m_actionOpenProject->setShortcut(QKeySequence::Open);

    m_actionSaveProject = new QAction(icon("save"), tr("&Save Project"), this);
    m_actionSaveProject->setShortcut(QKeySequence::Save);

    m_actionSaveProjectAs = new QAction(icon("save"), tr("Save Project &As..."), this);
    m_actionSaveProjectAs->setShortcut(QKeySequence::SaveAs);

    m_actionExit = new QAction(tr("E&xit"), this);
    m_actionExit->setShortcut(QKeySequence::Quit);
    connect(m_actionExit, &QAction::triggered, this, &MainWindow::close);

    // --- Measurement ------------------------------------------------------
    m_actionStart = new QAction(m_themes.icon(QStringLiteral("start"),
                                              m_themes.theme().success),
                                tr("&Start"), this);
    m_actionStart->setShortcut(Qt::Key_F5);
    m_actionStart->setToolTip(tr("Go bus-on and begin the measurement (F5)"));
    connect(m_actionStart, &QAction::triggered, this, &MainWindow::onStartMeasurement);

    m_actionStop = new QAction(icon("stop"), tr("S&top"), this);
    m_actionStop->setShortcut(Qt::Key_F6);
    m_actionStop->setToolTip(tr("Stop the measurement (F6)"));
    m_actionStop->setEnabled(false);
    connect(m_actionStop, &QAction::triggered, this, &MainWindow::onStopMeasurement);

    m_actionRecord = new QAction(m_themes.icon(QStringLiteral("record"), m_themes.theme().error),
                                 tr("&Record"), this);
    m_actionRecord->setCheckable(true);
    m_actionRecord->setToolTip(tr("Log every frame to disk, independently of the Trace panel"));

    m_actionReplay = new QAction(icon("replay"), tr("Re&play..."), this);

    // --- Hardware ---------------------------------------------------------
    m_actionHardwareConfiguration =
        new QAction(icon("hardware"), tr("Hardware &Configuration..."), this);

    m_actionRefreshHardware = new QAction(tr("&Refresh Interfaces"), this);
    m_actionRefreshHardware->setShortcut(QKeySequence::Refresh);
    connect(m_actionRefreshHardware, &QAction::triggered, this, &MainWindow::refreshHardware);

    // --- Tools / View -----------------------------------------------------
    m_actionToggleTheme = new QAction(icon("theme"), tr("Toggle &Dark / Light Theme"), this);
    m_actionToggleTheme->setShortcut(QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_T});
    connect(m_actionToggleTheme, &QAction::triggered, this, &MainWindow::onToggleTheme);

    m_actionResetLayout = new QAction(tr("&Reset Window Layout"), this);
    connect(m_actionResetLayout, &QAction::triggered, this, &MainWindow::applyDefaultLayout);

    m_actionInspectChrome = new QAction(tr("&Inspect Panel Chrome"), this);
    m_actionInspectChrome->setToolTip(
        tr("Report the real widget tree behind a panel to the Output panel: class "
           "names, geometry and palette colours. For diagnosing styling that "
           "appears to be ignored."));
    connect(m_actionInspectChrome, &QAction::triggered, this, &MainWindow::onInspectChrome);

    // --- Help -------------------------------------------------------------
    m_actionAbout = new QAction(icon("help"), tr("&About TorqueBus Studio"), this);
    connect(m_actionAbout, &QAction::triggered, this, &MainWindow::onAbout);

    m_actionAboutQt = new QAction(tr("About &Qt"), this);
    connect(m_actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);

    connect(m_actionNewProject, &QAction::triggered, this, &MainWindow::onNewProject);
    connect(m_actionOpenProject, &QAction::triggered, this, &MainWindow::onOpenProject);
    connect(m_actionSaveProject, &QAction::triggered, this, &MainWindow::onSaveProject);
    connect(m_actionSaveProjectAs, &QAction::triggered, this, &MainWindow::onSaveProjectAs);

    // Everything whose module has not landed yet reports honestly instead of
    // doing nothing when clicked.
    for (QAction* action : {m_actionRecord, m_actionReplay,
                            m_actionHardwareConfiguration}) {
        connect(action, &QAction::triggered, this, &MainWindow::onNotImplemented);
    }
}

void MainWindow::createMenus()
{
    QMenuBar* bar = menuBar();

    QMenu* fileMenu = bar->addMenu(tr("&File"));
    fileMenu->addAction(m_actionNewProject);
    fileMenu->addAction(m_actionOpenProject);
    fileMenu->addAction(m_actionSaveProject);
    fileMenu->addAction(m_actionSaveProjectAs);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actionExit);

    QMenu* homeMenu = bar->addMenu(tr("&Home"));
    homeMenu->addAction(m_actionStart);
    homeMenu->addAction(m_actionStop);
    homeMenu->addSeparator();
    homeMenu->addAction(m_actionRecord);
    homeMenu->addAction(m_actionReplay);

    QMenu* hardwareMenu = bar->addMenu(tr("Hard&ware"));
    hardwareMenu->addAction(m_actionHardwareConfiguration);
    hardwareMenu->addAction(m_actionRefreshHardware);

    QMenu* analysisMenu = bar->addMenu(tr("&Analysis"));
    QMenu* simulationMenu = bar->addMenu(tr("Si&mulation"));
    QMenu* diagnosticsMenu = bar->addMenu(tr("&Diagnostics"));

    // The View menu is generated from the dock list, so a panel added later
    // shows up here automatically.
    QMenu* viewMenu = bar->addMenu(tr("&View"));
    for (DockWidget* dock : std::as_const(m_allDocks)) {
        if (dock != nullptr) {
            viewMenu->addAction(dock->toggleAction());
        }
    }
    viewMenu->addSeparator();
    viewMenu->addAction(m_actionResetLayout);

    analysisMenu->addAction(m_traceDock->toggleAction());
    analysisMenu->addAction(m_graphDock->toggleAction());
    analysisMenu->addAction(m_statisticsDock->toggleAction());

    simulationMenu->addAction(m_transmitDock->toggleAction());

    diagnosticsMenu->addAction(m_diagnosticsDock->toggleAction());

    QMenu* toolsMenu = bar->addMenu(tr("&Tools"));
    toolsMenu->addAction(m_actionToggleTheme);
    toolsMenu->addSeparator();
    toolsMenu->addAction(m_outputDock->toggleAction());
    toolsMenu->addAction(m_actionInspectChrome);

    QMenu* helpMenu = bar->addMenu(tr("&Help"));
    helpMenu->addAction(m_actionAbout);
    helpMenu->addAction(m_actionAboutQt);
}

void MainWindow::createToolBar()
{
    auto* toolBar = new QToolBar(tr("Main"), this);
    toolBar->setObjectName(QStringLiteral("torquebus.toolbar.main"));
    toolBar->setMovable(false);
    toolBar->setIconSize(QSize{16, 16});
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // One toolbar row, not two. Every pixel of chrome is a pixel the trace
    // does not get, and the trace is the reason the window is open.
    toolBar->setFloatable(false);

    // Menus get the full, explicit wording; the toolbar gets a short label.
    // "Toggle Dark / Light Theme" is the right thing to read in a menu and the
    // wrong thing to spend 180 pixels of toolbar on.
    m_actionHardwareConfiguration->setIconText(tr("Hardware"));
    m_actionToggleTheme->setIconText(tr("Theme"));
    m_actionReplay->setIconText(tr("Replay"));

    toolBar->addAction(m_actionStart);
    toolBar->addAction(m_actionStop);
    toolBar->addSeparator();
    toolBar->addAction(m_actionRecord);
    toolBar->addAction(m_actionReplay);
    toolBar->addSeparator();
    toolBar->addAction(m_actionHardwareConfiguration);
    toolBar->addSeparator();
    toolBar->addAction(m_actionToggleTheme);

    addToolBar(Qt::TopToolBarArea, toolBar);
}

void MainWindow::createStatusBar()
{
    QStatusBar* bar = statusBar();
    bar->setSizeGripEnabled(true);

    // Left: what the hardware is doing. Right: what the measurement is doing.
    // The split is deliberate - the left half changes when the user changes
    // something, the right half changes continuously while running.
    m_channel1Label = createStatusLabel(tr("%1 CAN 1  not configured").arg(kBullet),
                                        QStringLiteral("offline"));
    m_channel2Label = createStatusLabel(tr("%1 CAN 2  not configured").arg(kBullet),
                                        QStringLiteral("offline"));

    m_frameCountLabel = createMetricLabel(tr("0"), 11);
    m_frameRateLabel = createMetricLabel(tr("0 f/s"), 11);
    m_busLoadLabel = createMetricLabel(tr("0.0 %"), 8);
    m_recordingLabel = createStatusLabel(tr("%1 Idle").arg(kBullet), QStringLiteral("offline"));

    const auto caption = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setProperty("torquebusState", "ready");
        return label;
    };

    bar->addWidget(m_channel1Label);
    bar->addWidget(createStatusSeparator());
    bar->addWidget(m_channel2Label);
    bar->addWidget(new QWidget, 1); // spacer

    bar->addPermanentWidget(caption(tr("Frames")));
    bar->addPermanentWidget(m_frameCountLabel);
    bar->addPermanentWidget(createStatusSeparator());
    bar->addPermanentWidget(m_frameRateLabel);
    bar->addPermanentWidget(createStatusSeparator());
    bar->addPermanentWidget(caption(tr("Load")));
    bar->addPermanentWidget(m_busLoadLabel);
    bar->addPermanentWidget(createStatusSeparator());
    bar->addPermanentWidget(m_recordingLabel);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::applyDefaultLayout()
{
    // Reproduces PLAN.md section 37: explorer left, properties right, the
    // analysis panels in the middle, console across the bottom.
    //
    // ORDER IS THE WHOLE TRICK. In KDDockWidgets a location is relative to the
    // main window's layout as a whole, not to "whatever is in the middle": the
    // first dock added occupies everything, and each later one splits a side
    // off what is already there.
    //
    // So the central panel must go in FIRST. Adding it last with Location_OnTop
    // - as this function originally did - does not put it in the middle at all;
    // it lays a full-width band across the top of the explorer/properties row,
    // which is what made the window look like an unarranged pile of panels.

    // 1. The analysis stack becomes the whole layout, and therefore the centre.
    addDockTo(this, m_traceDock, DockLocation::Top);

    m_traceDock->addDockWidgetAsTab(m_pipelineDock);
    m_traceDock->addDockWidgetAsTab(m_transmitDock);
    m_traceDock->addDockWidgetAsTab(m_graphDock);
    m_traceDock->addDockWidgetAsTab(m_statisticsDock);
    m_traceDock->addDockWidgetAsTab(m_diagnosticsDock);
    m_traceDock->setAsCurrentTab();

    // 2. Side panels split off the centre, with explicit starting widths so the
    //    centre keeps the space - the trace is what the user actually reads.
    addDockTo(this, m_projectDock, DockLocation::Left, QSize{kSidePanelWidth, 0});
    addDockTo(this, m_propertiesDock, DockLocation::Right, QSize{kSidePanelWidth, 0});
    m_propertiesDock->addDockWidgetAsTab(m_nodePropertiesDock);
    m_propertiesDock->setAsCurrentTab();

    // 3. The console spans the full width underneath everything, the way every
    //    engineering tool of this family arranges it.
    addDockTo(this, m_outputDock, DockLocation::Bottom, QSize{0, kConsoleHeight});

    m_projectDock->setMinimumWidth(kSidePanelMinimumWidth);
    m_propertiesDock->setMinimumWidth(kSidePanelMinimumWidth);
}

void MainWindow::restoreWindowState()
{
    const QByteArray geometry = m_settings.binaryValue(
        QString::fromLatin1(services::keys::kWindowGeometry));

    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    const bool restoreLayout =
        m_settings.boolValue(QString::fromLatin1(services::keys::kRestoreLayout), true);

    // A saved layout is a snapshot of a *previous* default arrangement. When we
    // change what the default is, every stored layout still describes the old
    // one - and restoring it would faithfully reproduce the arrangement we just
    // fixed, leaving the user to conclude that nothing changed.
    //
    // So the default layout carries a version, and a saved layout from an older
    // version is discarded rather than restored. Bump kDockLayoutVersion
    // whenever applyDefaultLayout() changes in a way users should receive.
    const int savedVersion =
        m_settings.intValue(QString::fromLatin1(services::keys::kDockLayoutVersion), 0);

    const bool layoutIsCurrent = savedVersion == kDockLayoutVersion;

    const QByteArray layout = layoutIsCurrent
        ? m_settings.binaryValue(QString::fromLatin1(services::keys::kDockLayout))
        : QByteArray{};

    // The docks must exist in the layout engine before LayoutSaver can place
    // them, so the default arrangement is always built first and the saved one
    // applied on top of it.
    applyDefaultLayout();

    if (!layoutIsCurrent && savedVersion != 0) {
        m_output->appendInfo(
            tr("The default panel arrangement changed in this version, so your saved "
               "layout was replaced. Rearrange the panels and it will be remembered again."));
        return;
    }

    if (restoreLayout && !layout.isEmpty() && !restoreDockLayout(layout)) {
        m_output->appendWarning(
            tr("The saved window layout could not be restored; the default layout was applied."));
        applyDefaultLayout();
    }
}

void MainWindow::saveWindowState() const
{
    m_settings.setBinaryValue(QString::fromLatin1(services::keys::kWindowGeometry),
                              saveGeometry());
    m_settings.setBinaryValue(QString::fromLatin1(services::keys::kDockLayout),
                              saveDockLayout());
    m_settings.setIntValue(QString::fromLatin1(services::keys::kDockLayoutVersion),
                           kDockLayoutVersion);
    m_settings.setValue(QString::fromLatin1(services::keys::kTheme),
                        toString(m_themes.variant()));

    if (!m_settings.save()) {
        qWarning("TorqueBus: failed to write settings to %s",
                 qPrintable(m_settings.filePath()));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Asked before anything is torn down, so cancelling leaves the window
    // exactly as it was rather than half closed.
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }

    // Stop before saving: the engine owns a thread and a set of open hardware
    // channels, and neither should outlive the window that started them.
    if (m_controller->isRunning()) {
        m_controller->stop();
    }

    saveWindowState();
    DockMainWindowBase::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

void MainWindow::refreshHardware()
{
    if (m_controller->isRunning()) {
        m_output->appendWarning(
            tr("Interfaces cannot be rescanned while a measurement is running."));
        return;
    }

    CanBackendRegistry& registry = CanBackendRegistry::instance();
    registry.registerBuiltins();

    m_devices = registry.enumerateAll();
    m_projectExplorer->setDevices(m_devices);

    for (const CanBackendRegistry::Entry& entry : registry.backends()) {
        if (!entry.available) {
            m_output->appendWarning(
                tr("%1 is not available on this machine; install its driver to enable it.")
                    .arg(QString::fromStdString(entry.displayName)));
        }
    }

    m_output->appendInfo(tr("%n CAN channel(s) detected.", nullptr,
                            static_cast<int>(m_devices.size())));

    // Until the Hardware Manager arrives in v0.3, every detected channel is
    // bound automatically in enumeration order: CAN 1 is the first interface,
    // CAN 2 the second. The mapping becomes explicit and persisted then.
    m_controller->bindAvailableChannels();

    m_actionStart->setEnabled(!m_devices.empty());

    for (std::size_t index = 0; index < 2 && index < m_devices.size(); ++index) {
        setChannelIndicator(static_cast<int>(index),
                            QString::fromStdString(m_devices[index].name),
                            QStringLiteral("ready"));
    }
}

void MainWindow::setChannelIndicator(int index, const QString& text, const QString& state)
{
    QLabel* label = index == 0 ? m_channel1Label : m_channel2Label;
    if (label == nullptr) {
        return;
    }

    label->setText(QStringLiteral("%1 %2").arg(QString::fromUtf8(kBullet),
                                               tr("CAN %1  %2").arg(index + 1).arg(text)));
    label->setProperty("torquebusState", state);
    repolish(label);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onThemeChanged(const Theme& theme)
{
    // Every icon in the application is a monochrome SVG coloured at load time,
    // which means every cached icon is wrong the moment the theme flips. An
    // icon tinted for the dark theme is a pale glyph, and a pale glyph on the
    // light theme's white panel is invisible - which is exactly how this looked
    // before: the tab and tree icons simply vanished in TorqueBus Light.
    //
    // Anything that holds a tinted icon has to re-tint it here, or listen to
    // themeChanged itself (as the panels do).

    const QList<QPair<DockWidget*, const char*>> dockIcons{
        {m_projectDock, "project"},        {m_propertiesDock, "properties"},
        {m_outputDock, "console"},         {m_traceDock, "trace"},
        {m_transmitDock, "transmit"},      {m_graphDock, "graph"},
        {m_statisticsDock, "statistics"},  {m_diagnosticsDock, "diagnostics"},
    };

    for (const auto& [dock, name] : dockIcons) {
        setDockIcon(dock, m_themes.icon(QString::fromLatin1(name)));
    }
    m_actionNewProject->setIcon(m_themes.icon(QStringLiteral("new")));
    m_actionOpenProject->setIcon(m_themes.icon(QStringLiteral("open")));
    m_actionSaveProject->setIcon(m_themes.icon(QStringLiteral("save")));
    m_actionSaveProjectAs->setIcon(m_themes.icon(QStringLiteral("save")));
    m_actionStart->setIcon(m_themes.icon(QStringLiteral("start"), theme.success));
    m_actionStop->setIcon(m_themes.icon(QStringLiteral("stop")));
    m_actionRecord->setIcon(m_themes.icon(QStringLiteral("record"), theme.error));
    m_actionReplay->setIcon(m_themes.icon(QStringLiteral("replay")));
    m_actionHardwareConfiguration->setIcon(m_themes.icon(QStringLiteral("hardware")));
    m_actionToggleTheme->setIcon(m_themes.icon(QStringLiteral("theme")));
    m_actionAbout->setIcon(m_themes.icon(QStringLiteral("help")));

    m_output->appendInfo(tr("Theme changed to %1.").arg(theme.name));
}

void MainWindow::onDeviceSelected(const CanDeviceInfo& device)
{
    m_properties->setDevice(device);
}

void MainWindow::onToggleTheme()
{
    m_themes.toggleVariant();
}

void MainWindow::onAbout()
{
    QMessageBox::about(
        this,
        tr("About TorqueBus Studio"),
        tr("<h3>TorqueBus Studio %1</h3>"
           "<p>Open Automotive Network &amp; Diagnostics Workbench.</p>"
           "<p>An open source platform for the analysis, simulation, diagnostics "
           "and automation of automotive networks.</p>"
           "<p>Copyright &copy; TorqueBus contributors.<br/>"
           "Licensed under the GNU General Public License v3.0 or later.</p>"
           "<p>Built with Qt %2.</p>")
            .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

void MainWindow::onCanvasNodeSelected(const QString& descriptionId)
{
    // Only the Block editor. An earlier version also filled the read-only
    // Properties panel with the same values, which meant the same node
    // described in two places - and the read-only copy went stale the moment
    // anything was edited in the other one.
    //
    // The Properties panel keeps its own job: describing a hardware channel
    // chosen in the Project Explorer. Two panels answering different questions
    // is fine; two answering the same one is a bug waiting to be reported.
    if (m_nodeProperties == nullptr) {
        return;
    }

    m_nodeProperties->showNode(descriptionId);

    // Brought forward: an editor behind another tab is an editor the user does
    // not know they have.
    if (m_nodePropertiesDock != nullptr) {
        m_nodePropertiesDock->setAsCurrentTab();
    }
}

void MainWindow::onGraphEdited()
{
    markDirty();

    // Reported while the user is looking at the wire they just drew, rather
    // than at the next Start. validate() needs no engine and no nodes, which is
    // exactly why it is a separate function from build().
    const Result result = m_pipeline.validate(m_catalog);

    if (result.failed()) {
        m_output->appendWarning(tr("Pipeline: %1")
                                    .arg(QString::fromStdString(std::string{result.message()})));
        return;
    }

    m_output->appendInfo(tr("Pipeline: %1 node(s), %2 connection(s).")
                             .arg(static_cast<qulonglong>(m_pipeline.nodes().size()))
                             .arg(static_cast<qulonglong>(m_pipeline.edges().size())));
}

void MainWindow::updateWindowTitle()
{
    const QString name = m_projectPath.isEmpty()
        ? tr("Untitled project")
        : QFileInfo{m_projectPath}.completeBaseName();

    // The asterisk is the convention every editor uses, and it is the only
    // continuous signal that there is something to lose - the prompt only
    // appears at the moment it would be lost, which is too late to be a warning.
    setWindowTitle(tr("%1%2 - TorqueBus Studio")
                       .arg(name, m_dirty ? QStringLiteral("*") : QString{}));
}

void MainWindow::markDirty()
{
    if (m_dirty) {
        return;
    }

    m_dirty = true;
    updateWindowTitle();
}

bool MainWindow::confirmDiscardChanges()
{
    if (!m_dirty) {
        return true;
    }

    const QString name = m_projectPath.isEmpty()
        ? tr("this project")
        : QFileInfo{m_projectPath}.fileName();

    // Save is the default and Discard is not, because the cost of the two
    // mistakes is not symmetric: an unwanted save is undone by not saving
    // again, and a discarded pipeline is gone.
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("The pipeline in %1 has changed.\n\nSave it before continuing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    switch (answer) {
    case QMessageBox::Save:
        onSaveProject();
        // Still dirty means the save failed or its dialog was cancelled, and
        // continuing would discard the work the user just asked to keep.
        return !m_dirty;

    case QMessageBox::Discard:
        return true;

    default:
        return false;
    }
}

void MainWindow::onNewProject()
{
    if (!confirmDiscardChanges()) {
        return;
    }

    m_pipeline.clear();
    m_projectPath.clear();
    m_dirty = false;

    if (m_canvas != nullptr) {
        m_canvas->reload();
    }
    if (m_nodeProperties != nullptr) {
        m_nodeProperties->clear();
    }

    updateWindowTitle();
    m_output->appendInfo(tr("New project."));
}

void MainWindow::onOpenProject()
{
    if (!confirmDiscardChanges()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open project"), QString{}, services::ProjectFile::fileFilter());

    if (!path.isEmpty()) {
        openProject(path);
    }
}

void MainWindow::openProject(const QString& path)
{
    // Loaded into the live description. ProjectFile leaves it untouched when
    // the read fails, so a broken file cannot leave the canvas showing half a
    // pipeline that was never saved.
    if (const Result result = services::ProjectFile::load(path, m_pipeline);
        result.failed()) {
        m_output->appendError(tr("Could not open the project: %1")
                                  .arg(QString::fromStdString(std::string{result.message()})));
        return;
    }

    m_projectPath = path;
    m_dirty = false;

    // The canvas holds its own id mapping, so it has to be told the graph was
    // replaced wholesale rather than edited.
    if (m_canvas != nullptr) {
        m_canvas->reload();
    }
    if (m_nodeProperties != nullptr) {
        m_nodeProperties->clear();
    }

    updateWindowTitle();
    m_output->appendInfo(tr("Opened %1.").arg(QFileInfo{path}.fileName()));

    onGraphEdited();
}

bool MainWindow::writeProject(const QString& path)
{
    if (const Result result = services::ProjectFile::save(path, m_pipeline);
        result.failed()) {
        m_output->appendError(tr("Could not save the project: %1")
                                  .arg(QString::fromStdString(std::string{result.message()})));
        return false;
    }

    m_projectPath = path;
    m_dirty = false;
    updateWindowTitle();
    m_output->appendInfo(tr("Saved %1.").arg(QFileInfo{path}.fileName()));

    return true;
}

void MainWindow::onSaveProject()
{
    if (m_projectPath.isEmpty()) {
        onSaveProjectAs();
        return;
    }

    writeProject(m_projectPath);
}

void MainWindow::onSaveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save project"), m_projectPath, services::ProjectFile::fileFilter());

    if (path.isEmpty()) {
        return;
    }

    // The dialog does not always append the extension - it depends on the
    // platform and on whether the user typed one - and a project saved as
    // "EngineTest" with no suffix will not be offered by the Open dialog's
    // filter next time.
    if (QFileInfo{path}.suffix().isEmpty()) {
        path += QLatin1Char('.') + services::ProjectFile::extension();
    }

    writeProject(path);
}

void MainWindow::onStartMeasurement()
{
    // The snapshot is taken HERE, and not once at construction.
    //
    // setGraphDescription copies, which is what lets the canvas stay editable
    // during a measurement without changing what is currently running. The
    // corollary is easy to get wrong in practice: called once at startup it
    // copies an empty pipeline, and nothing the user later draws ever reaches
    // the engine. Taking the copy at Start is what makes "what runs is what was
    // on the canvas when you pressed Start" literally true.
    m_controller->engine().setGraphDescription(m_pipeline, m_catalog);

    // Refused before the channels are opened, with the node named, rather than
    // after - a half-started measurement is the thing start() exists to avoid.
    if (const Result result = m_pipeline.validate(m_catalog); result.failed()) {
        m_output->appendError(tr("Cannot start: %1")
                                  .arg(QString::fromStdString(std::string{result.message()})));
        return;
    }

    m_output->appendInfo(tr("Starting measurement..."));
    m_controller->start();
}

void MainWindow::onStopMeasurement()
{
    m_controller->stop();
}

void MainWindow::onMeasurementStarted()
{
    m_actionStart->setEnabled(false);
    m_actionStop->setEnabled(true);
    m_actionRefreshHardware->setEnabled(false);

    m_recordingLabel->setText(QStringLiteral("%1 %2").arg(QString::fromUtf8(kBullet),
                                                          tr("Running")));
    m_recordingLabel->setProperty("torquebusState", QStringLiteral("online"));
    repolish(m_recordingLabel);

    m_output->appendInfo(tr("Measurement running on %n channel(s).", nullptr,
                            static_cast<int>(m_controller->engine().channelCount())));
}

void MainWindow::onMeasurementStopped()
{
    m_actionStart->setEnabled(!m_devices.empty());
    m_actionStop->setEnabled(false);
    m_actionRefreshHardware->setEnabled(true);

    m_recordingLabel->setText(QStringLiteral("%1 %2").arg(QString::fromUtf8(kBullet),
                                                          tr("Idle")));
    m_recordingLabel->setProperty("torquebusState", QStringLiteral("offline"));
    repolish(m_recordingLabel);

    m_output->appendInfo(tr("Measurement stopped. %L1 frames captured.")
                             .arg(m_controller->engine().deliveredFrames()));
}

void MainWindow::onMeasurementFailed(const QString& message)
{
    m_actionStart->setEnabled(!m_devices.empty());
    m_actionStop->setEnabled(false);
    m_actionRefreshHardware->setEnabled(true);

    m_output->appendError(message);
    QMessageBox::warning(this, tr("Measurement could not start"), message);
}

void MainWindow::onStatusUpdated(const QList<ChannelStatus>& channels)
{
    for (int index = 0; index < 2; ++index) {
        if (index >= channels.size()) {
            continue;
        }

        const ChannelStatus& status = channels.at(index);

        setChannelIndicator(index,
                            tr("%1  %2").arg(status.deviceName, status.stateText),
                            status.stateToken);
    }

    // The status bar shows the busiest channel rather than a sum: a total
    // across buses is not a quantity that means anything, but "the worst load
    // on any bus right now" is exactly what an engineer is watching for.
    double peakLoad = 0.0;
    double totalRate = 0.0;

    for (const ChannelStatus& status : channels) {
        peakLoad = std::max(peakLoad, status.busLoadPercent);
        totalRate += status.framesPerSecond;
    }

    m_busLoadLabel->setText(tr("%1 %").arg(peakLoad, 0, 'f', 1));
    m_frameRateLabel->setText(tr("%L1 f/s").arg(static_cast<qint64>(totalRate)));

    // 80 % is where a CAN bus stops having comfortable headroom for
    // arbitration; past it, latency on low-priority frames climbs sharply.
    // Worth colouring, not worth interrupting anyone about.
    const bool overloaded = peakLoad > 80.0;
    m_busLoadLabel->setProperty("torquebusState",
                                overloaded ? QStringLiteral("warning")
                                           : QStringLiteral("ready"));
    repolish(m_busLoadLabel);
}

void MainWindow::onFrameCountChanged(quint64 total)
{
    m_frameCountLabel->setText(QStringLiteral("%L1").arg(total));
}

void MainWindow::onInspectChrome()
{
    m_output->appendInfo(tr("--- Panel chrome, as Qt actually sees it ---"));

    for (const QString& line : describeDockChrome(m_projectDock)) {
        m_output->appendInfo(line);
    }

    m_output->appendInfo(
        tr("A class shown as 'QWidget' has no Q_OBJECT, so no class selector in "
           "torquebus.qss can ever match it - that rule is dead, not ignored."));
}

void MainWindow::onNotImplemented()
{
    const auto* action = qobject_cast<QAction*>(sender());
    const QString name = action != nullptr
                             ? action->text().remove(QLatin1Char('&')).remove(QStringLiteral("..."))
                             : tr("This command");

    m_output->appendWarning(tr("%1 is not implemented yet in this milestone.").arg(name));
}

} // namespace torquebus::ui
