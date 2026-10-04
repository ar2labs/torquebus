// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/graph/GraphPanel.h"

#include "ui/graph/PlotSettingsDialog.h"
#include "ui/graph/PlotView.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>

namespace torquebus::ui {
namespace {

constexpr int kRefreshMs = 50;
constexpr std::size_t kMaximumPointsPerTrace = 2048;
constexpr int kSeriesIdRole = Qt::UserRole + 1;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

struct WindowChoice final {
    const char* label;
    std::uint64_t nanoseconds;
};

constexpr std::array<WindowChoice, 10> kWindows{{
    {"1 s", 1'000'000'000ULL},
    {"2 s", 2'000'000'000ULL},
    {"5 s", 5'000'000'000ULL},
    {"10 s", 10'000'000'000ULL},
    {"30 s", 30'000'000'000ULL},
    {"1 min", 60'000'000'000ULL},
    {"2 min", 120'000'000'000ULL},
    {"5 min", 300'000'000'000ULL},
    {"10 min", 600'000'000'000ULL},
    {"All / Auto", 0ULL},
}};

constexpr int kDefaultWindowIndex = 3; // 10 s

} // namespace

GraphPanel::GraphPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &GraphPanel::refresh);
    m_timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }
}

void GraphPanel::buildUi()
{
    setupToolbar();

    // Left Sidebar: Search Filter + Signal List
    m_leftSidebar = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(m_leftSidebar);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    m_signalFilter = new QLineEdit(m_leftSidebar);
    m_signalFilter->setObjectName(QStringLiteral("torquebus.input.signalFilter"));
    m_signalFilter->setPlaceholderText(tr("Search signals..."));
    m_signalFilter->setClearButtonEnabled(true);
    connect(m_signalFilter, &QLineEdit::textChanged, this, &GraphPanel::onSignalFilterChanged);
    leftLayout->addWidget(m_signalFilter);

    m_signals = new QListWidget(m_leftSidebar);
    m_signals->setObjectName(QStringLiteral("torquebus.list.plotSignals"));
    m_signals->setFrameShape(QFrame::NoFrame);
    m_signals->setAlternatingRowColors(true);
    m_signals->setUniformItemSizes(true);
    m_signals->setMinimumWidth(160);
    m_signals->setMaximumWidth(320);
    m_signals->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_signals, &QListWidget::itemChanged, this, &GraphPanel::onSelectionChanged);
    connect(m_signals,
            &QListWidget::customContextMenuRequested,
            this,
            &GraphPanel::onSignalContextMenu);
    leftLayout->addWidget(m_signals, 1);

    // Plot Canvas
    m_plot = new PlotView(this);
    m_plot->setPlaceholder(tr("Tick a signal to plot it."));
    connect(m_plot, &PlotView::cursorMoved, this, &GraphPanel::onCursorMoved);
    connect(m_plot, &PlotView::cursorLeft, this, &GraphPanel::onCursorLeft);
    connect(m_plot, &PlotView::timeRangeChanged, this, &GraphPanel::onTimeRangeChanged);
    connect(m_plot, &PlotView::autoScaleRequested, this, &GraphPanel::onFitAll);
    connect(m_plot, &PlotView::measurementChanged, this, &GraphPanel::onMeasurementChanged);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.graph"));
    m_splitter->addWidget(m_leftSidebar);
    m_splitter->addWidget(m_plot);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 6, 4);
    layout->setSpacing(2);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_splitter, 1);
    layout->addWidget(m_status);

    m_windowNs = kWindows[kDefaultWindowIndex].nanoseconds;
    updatePlayPauseUi();
    updateStatus();
}

void GraphPanel::setupToolbar()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.graph"));
    m_toolBar->setIconSize(QSize{18, 18});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    ThemeManager* themes = ThemeManager::instance();
    const auto getIcon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };
    const QColor iconCol = currentTheme().text;

    // 1. Play / Pause / Stop / Clear
    m_actionPlay = m_toolBar->addAction(getIcon("start"), tr("Run"));
    m_actionPlay->setToolTip(tr("Run / Resume following live measurement data"));
    connect(m_actionPlay, &QAction::triggered, this, &GraphPanel::onPlay);

    m_actionPause = m_toolBar->addAction(getIcon("pause"), tr("Pause"));
    m_actionPause->setToolTip(tr("Pause live tail tracking to inspect data"));
    connect(m_actionPause, &QAction::triggered, this, &GraphPanel::onPause);

    m_actionStop = m_toolBar->addAction(getIcon("stop"), tr("Stop"));
    m_actionStop->setToolTip(tr("Stop live stream following"));
    connect(m_actionStop, &QAction::triggered, this, &GraphPanel::onStop);

    m_actionClear = m_toolBar->addAction(getIcon("clear"), tr("Clear"));
    m_actionClear->setToolTip(tr("Clear all recorded signal history"));
    connect(m_actionClear, &QAction::triggered, this, &GraphPanel::onClear);

    m_toolBar->addSeparator();

    // 2. Interactive Navigation Tools (Exclusive Group)
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);

    m_actionPointer = m_toolBar->addAction(getIcon("pointer"), tr("Pointer"));
    m_actionPointer->setCheckable(true);
    m_actionPointer->setChecked(true);
    m_actionPointer->setToolTip(tr("Pointer Tool: hover to scrub cursor, inspect sample points"));
    m_toolGroup->addAction(m_actionPointer);
    connect(m_actionPointer, &QAction::toggled, this, &GraphPanel::onToolPointerToggled);

    m_actionPan = m_toolBar->addAction(getIcon("pan"), tr("Pan"));
    m_actionPan->setCheckable(true);
    m_actionPan->setToolTip(tr("Pan Tool: click and drag horizontally to shift time window"));
    m_toolGroup->addAction(m_actionPan);
    connect(m_actionPan, &QAction::toggled, this, &GraphPanel::onToolPanToggled);

    m_actionZoomBox = m_toolBar->addAction(getIcon("zoom-box"), tr("Zoom Box"));
    m_actionZoomBox->setCheckable(true);
    m_actionZoomBox->setToolTip(
        tr("Zoom Box: drag a rectangular area to zoom into that time span"));
    m_toolGroup->addAction(m_actionZoomBox);
    connect(m_actionZoomBox, &QAction::toggled, this, &GraphPanel::onToolZoomBoxToggled);

    m_toolBar->addSeparator();

    // 3. Zoom In / Zoom Out / Fit All
    m_actionZoomIn = m_toolBar->addAction(getIcon("zoom-in"), tr("Zoom In"));
    m_actionZoomIn->setToolTip(tr("Zoom In time axis (25%)"));
    connect(m_actionZoomIn, &QAction::triggered, this, &GraphPanel::onZoomIn);

    m_actionZoomOut = m_toolBar->addAction(getIcon("zoom-out"), tr("Zoom Out"));
    m_actionZoomOut->setToolTip(tr("Zoom Out time axis (25%)"));
    connect(m_actionZoomOut, &QAction::triggered, this, &GraphPanel::onZoomOut);

    m_actionFitAll = m_toolBar->addAction(getIcon("fit"), tr("Fit to Window"));
    m_actionFitAll->setToolTip(
        tr("Fit to Window: auto-scale time window to encompass all recorded data"));
    connect(m_actionFitAll, &QAction::triggered, this, &GraphPanel::onFitAll);

    m_toolBar->addSeparator();

    // 4. Feature Toggles
    m_actionDualCursors = m_toolBar->addAction(getIcon("cursors"), tr("Cursors"));
    m_actionDualCursors->setCheckable(true);
    m_actionDualCursors->setToolTip(
        tr("Dual Measurement Cursors: measure Δt, frequency and ΔY between Cursor A and B"));
    connect(m_actionDualCursors, &QAction::toggled, this, &GraphPanel::onDualCursorsToggled);

    m_actionSubplots = m_toolBar->addAction(getIcon("subplots"), tr("Subplots"));
    m_actionSubplots->setCheckable(true);
    m_actionSubplots->setChecked(true);
    m_actionSubplots->setToolTip(
        tr("Subplots: stack separate subplots grouped by physical unit (e.g. Volts, Liters)"));
    connect(m_actionSubplots, &QAction::toggled, this, &GraphPanel::onSubplotsToggled);

    m_actionLegend = m_toolBar->addAction(getIcon("legend"), tr("Legend"));
    m_actionLegend->setCheckable(true);
    m_actionLegend->setChecked(true);
    m_actionLegend->setToolTip(tr("Legend: toggle live values table overlay in top-right corner"));
    connect(m_actionLegend, &QAction::toggled, this, &GraphPanel::onLegendToggled);

    m_actionTimeFormat = m_toolBar->addAction(getIcon("time-format"), tr("Time Format"));
    m_actionTimeFormat->setCheckable(true);
    m_actionTimeFormat->setToolTip(
        tr("Toggle between Relative Elapsed Seconds and Absolute Date & Time"));
    connect(m_actionTimeFormat, &QAction::toggled, this, &GraphPanel::onTimeFormatToggled);

    m_toolBar->addSeparator();

    // 5. Export Menu & Settings
    auto* exportBtn = new QToolButton(m_toolBar);
    exportBtn->setIcon(getIcon("export"));
    exportBtn->setToolTip(tr("Export Plot Image or CSV Data"));
    exportBtn->setPopupMode(QToolButton::InstantPopup);

    auto* exportMenu = new QMenu(exportBtn);
    auto* actPng = exportMenu->addAction(tr("Export Plot Image (PNG)..."));
    connect(actPng, &QAction::triggered, this, &GraphPanel::onExportPng);
    auto* actCopy = exportMenu->addAction(tr("Copy Plot Image to Clipboard"));
    connect(actCopy, &QAction::triggered, this, &GraphPanel::onCopyImage);
    exportMenu->addSeparator();
    auto* actCsv = exportMenu->addAction(tr("Export Signal Data to CSV..."));
    connect(actCsv, &QAction::triggered, this, &GraphPanel::onExportCsv);
    exportBtn->setMenu(exportMenu);
    m_toolBar->addWidget(exportBtn);

    m_actionSettings = m_toolBar->addAction(getIcon("settings"), tr("Settings"));
    m_actionSettings->setToolTip(tr("Configure Plot Title, Subplots, Grid and Curve Styles"));
    connect(m_actionSettings, &QAction::triggered, this, &GraphPanel::onSettings);

    m_toolBar->addSeparator();

    // 6. X-Axis Span Dropdown (Far Right, PCAN-Explorer Style)
    auto* spanLabel = new QLabel(tr("X-Axis span: "), m_toolBar);
    spanLabel->setStyleSheet(QStringLiteral("color: palette(text); padding-left: 6px;"));
    m_toolBar->addWidget(spanLabel);

    m_window = new QComboBox(m_toolBar);
    for (const WindowChoice& choice : kWindows) {
        m_window->addItem(QString::fromLatin1(choice.label),
                          QVariant::fromValue(static_cast<qulonglong>(choice.nanoseconds)));
    }
    m_window->setCurrentIndex(kDefaultWindowIndex);
    m_window->setToolTip(tr("Horizontal time window span"));
    connect(m_window, &QComboBox::currentIndexChanged, this, &GraphPanel::onWindowChanged);
    m_toolBar->addWidget(m_window);
}

void GraphPanel::setStore(SignalSeriesStore* store)
{
    m_store = store;

    {
        const QSignalBlocker blocker{m_signals};
        m_signals->clear();
    }
    m_listed.clear();
    m_selected.clear();
    m_windows.clear();
    m_customStyles.clear();
    m_tracesDirty = true;
    m_lastNewestNs = 0;
    m_userInteracting = false;

    if (m_plot != nullptr) {
        m_plot->setTraces({});
    }

    syncSignalList();
    updatePlayPauseUi();
    updateStatus();
}

QByteArray GraphPanel::splitterState() const
{
    return m_splitter != nullptr ? m_splitter->saveState() : QByteArray{};
}

void GraphPanel::restoreSplitterState(const QByteArray& state)
{
    if (m_splitter == nullptr || state.isEmpty()) {
        return;
    }
    m_splitter->restoreState(state);
}

void GraphPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_tracesDirty = true;
    refresh();
}

// ---------------------------------------------------------------------------
// The Clock & Refresh
// ---------------------------------------------------------------------------

void GraphPanel::refresh()
{
    if (m_refreshing || m_store == nullptr || !isVisible()) {
        return;
    }

    struct RefreshGuard final {
        bool& flag;
        explicit RefreshGuard(bool& target) noexcept
            : flag{target}
        {
            flag = true;
        }
        ~RefreshGuard() { flag = false; }
    } const guard{m_refreshing};

    syncSignalList();

    if (m_frozen) {
        return;
    }

    std::vector<SeriesId> currentSelection = selectedSeries();
    if (currentSelection != m_selected) {
        m_selected = std::move(currentSelection);
        m_tracesDirty = true;
    }

    if (m_selected.empty()) {
        if (m_tracesDirty) {
            m_tracesDirty = false;
            m_plot->setTraces({});
            updateStatus();
        }
        return;
    }

    const std::uint64_t newest = m_store->newestTimestampNs();
    if (!m_tracesDirty && newest == m_lastNewestNs && m_windowNs == m_lastWindowNs) {
        return;
    }

    m_lastNewestNs = newest;
    m_lastWindowNs = m_windowNs;
    m_tracesDirty = false;

    const std::uint64_t span = m_windowNs > 0 ? m_windowNs : 10'000'000'000ULL;
    const std::uint64_t start = newest > span ? newest - span : 0;

    m_store->readWindows(m_selected, start, kMaximumPointsPerTrace, m_windows);

    std::vector<PlotTrace> traces;
    traces.reserve(m_windows.size());

    for (std::size_t index = 0; index < m_windows.size(); ++index) {
        SeriesWindow& window = m_windows[index];
        const SeriesId id = window.id;

        PlotTrace trace;
        trace.name = QString::fromStdString(window.name);
        trace.unit = QString::fromStdString(window.unit);
        trace.colour = colourFor(index, id);
        trace.samples = std::move(window.samples);
        trace.minimum = window.minimum;
        trace.maximum = window.maximum;
        trace.lineWidth = m_defaultLineWidth;

        // Apply custom or default fill style
        if (m_customStyles.contains(id)) {
            trace.fillStyle = m_customStyles[id].fillStyle;
            trace.subplotIndex = m_customStyles[id].subplotIndex;
        } else {
            trace.fillStyle = m_defaultFill;
        }

        traces.push_back(std::move(trace));
    }

    m_plot->setWindow(start, std::max(newest, start + span));
    m_plot->setTraces(std::move(traces));

    updateStatus();
}

void GraphPanel::syncSignalList()
{
    if (m_store == nullptr) {
        return;
    }

    const std::vector<SeriesInfo> infos = m_store->listSeries();
    const QSignalBlocker blocker{m_signals};

    if (infos.size() < static_cast<std::size_t>(m_listed.size())) {
        m_signals->clear();
        m_listed.clear();
        m_tracesDirty = true;
    }

    const QString filterText = m_signalFilter->text().trimmed();

    for (std::size_t i = 0; i < infos.size(); ++i) {
        const SeriesInfo& info = infos[i];
        const QString name = QString::fromStdString(info.name);

        if (!m_listed.contains(name)) {
            m_listed.insert(name, info.id);
            m_tracesDirty = true;

            auto* item = new QListWidgetItem(name);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
            item->setData(kSeriesIdRole, QVariant::fromValue(static_cast<qulonglong>(info.id)));

            if (!info.unit.empty()) {
                item->setToolTip(tr("%1 [%2]").arg(name, QString::fromStdString(info.unit)));
            }

            updateSignalItemVisuals(item, info.id);
            m_signals->addItem(item);
        }
    }

    // Apply search filter visibility
    for (int row = 0; row < m_signals->count(); ++row) {
        QListWidgetItem* item = m_signals->item(row);
        if (item != nullptr) {
            const bool matches =
                filterText.isEmpty() || item->text().contains(filterText, Qt::CaseInsensitive);
            item->setHidden(!matches);
        }
    }
}

void GraphPanel::updateSignalItemVisuals(QListWidgetItem* item, SeriesId id)
{
    if (item == nullptr) {
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const int physical = static_cast<int>(std::round(14.0 * dpr));
    QImage swatch{QSize{physical, physical}, QImage::Format_ARGB32_Premultiplied};
    swatch.fill(Qt::transparent);
    {
        QPainter p{&swatch};
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor col = colourFor(static_cast<std::size_t>(id), id);
        p.setBrush(col);
        p.setPen(QPen{col.lighter(130), 1.2 * dpr});
        p.drawRoundedRect(
            QRectF{1.0 * dpr, 1.0 * dpr, 12.0 * dpr, 12.0 * dpr}, 3.0 * dpr, 3.0 * dpr);
    }
    swatch.setDevicePixelRatio(dpr);
    item->setIcon(QIcon{QPixmap::fromImage(swatch)});
}

std::vector<SeriesId> GraphPanel::selectedSeries() const
{
    std::vector<SeriesId> ids;
    for (int row = 0; row < m_signals->count(); ++row) {
        const QListWidgetItem* item = m_signals->item(row);
        if (item != nullptr && item->checkState() == Qt::Checked) {
            ids.push_back(static_cast<SeriesId>(item->data(kSeriesIdRole).toULongLong()));
        }
    }
    return ids;
}

QColor GraphPanel::colourFor(std::size_t index, SeriesId id) const
{
    if (m_customStyles.contains(id) && m_customStyles[id].hasCustomColor) {
        return m_customStyles[id].customColor;
    }

    const std::array<AccentColor, 8> palette = accentColors();
    const AccentColor accent = palette[index % palette.size()];
    return accentPairFor(accent, currentTheme().variant).accent;
}

// ---------------------------------------------------------------------------
// Tool Actions & Playback
// ---------------------------------------------------------------------------

void GraphPanel::onPlay()
{
    m_frozen = false;
    m_userInteracting = false;
    updatePlayPauseUi();
    refresh();
}

void GraphPanel::onPause()
{
    m_frozen = true;
    updatePlayPauseUi();
    updateStatus();
}

void GraphPanel::onStop()
{
    m_frozen = true;
    updatePlayPauseUi();
    updateStatus();
}

void GraphPanel::onClear()
{
    if (m_store == nullptr) {
        return;
    }
    m_store->clearSamples();
    m_tracesDirty = true;
    m_lastNewestNs = 0;
    m_plot->setTraces({});
    updateStatus();
}

void GraphPanel::updatePlayPauseUi()
{
    m_actionPlay->setEnabled(m_frozen);
    m_actionPause->setEnabled(!m_frozen);
    m_actionStop->setEnabled(!m_frozen);
}

void GraphPanel::onToolPointerToggled(bool checked)
{
    if (checked) {
        m_plot->setToolMode(PlotToolMode::Pointer);
    }
}

void GraphPanel::onToolPanToggled(bool checked)
{
    if (checked) {
        m_plot->setToolMode(PlotToolMode::Pan);
    }
}

void GraphPanel::onToolZoomBoxToggled(bool checked)
{
    if (checked) {
        m_plot->setToolMode(PlotToolMode::ZoomBox);
    }
}

void GraphPanel::onZoomIn()
{
    const std::uint64_t start = m_plot->windowStartNs();
    const std::uint64_t end = m_plot->windowEndNs();
    const std::uint64_t span = end - start;
    const std::uint64_t newSpan = std::max(1'000'000ULL, span * 3 / 4);
    const std::uint64_t center = start + span / 2;

    const std::uint64_t newStart = center > newSpan / 2 ? center - newSpan / 2 : 0;
    const std::uint64_t newEnd = newStart + newSpan;

    m_plot->setWindow(newStart, newEnd);
    m_windowNs = newSpan;
    onTimeRangeChanged(newStart, newEnd);
}

void GraphPanel::onZoomOut()
{
    const std::uint64_t start = m_plot->windowStartNs();
    const std::uint64_t end = m_plot->windowEndNs();
    const std::uint64_t span = end - start;
    const std::uint64_t newSpan = span * 5 / 4;
    const std::uint64_t center = start + span / 2;

    const std::uint64_t newStart = center > newSpan / 2 ? center - newSpan / 2 : 0;
    const std::uint64_t newEnd = newStart + newSpan;

    m_plot->setWindow(newStart, newEnd);
    m_windowNs = newSpan;
    onTimeRangeChanged(newStart, newEnd);
}

void GraphPanel::onFitAll()
{
    if (m_store == nullptr || m_selected.empty()) {
        return;
    }

    const std::uint64_t newest = m_store->newestTimestampNs();
    m_store->readWindows(m_selected, 0, kMaximumPointsPerTrace, m_windows);

    std::uint64_t earliest = newest;
    for (const SeriesWindow& win : m_windows) {
        if (!win.samples.empty()) {
            earliest = std::min(earliest, win.samples.front().timestampNs);
        }
    }

    if (earliest >= newest) {
        earliest = newest > 1'000'000'000ULL ? newest - 1'000'000'000ULL : 0;
    }

    m_frozen = true;
    m_userInteracting = true;
    updatePlayPauseUi();

    m_plot->setWindow(earliest, newest);
    m_windowNs = newest - earliest;

    std::vector<PlotTrace> traces;
    traces.reserve(m_windows.size());
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
        PlotTrace trace;
        trace.name = QString::fromStdString(m_windows[i].name);
        trace.unit = QString::fromStdString(m_windows[i].unit);
        trace.colour = colourFor(i, m_windows[i].id);
        trace.samples = std::move(m_windows[i].samples);
        trace.minimum = m_windows[i].minimum;
        trace.maximum = m_windows[i].maximum;
        trace.lineWidth = m_defaultLineWidth;
        if (m_customStyles.contains(m_windows[i].id)) {
            trace.fillStyle = m_customStyles[m_windows[i].id].fillStyle;
            trace.subplotIndex = m_customStyles[m_windows[i].id].subplotIndex;
        } else {
            trace.fillStyle = m_defaultFill;
        }
        traces.push_back(std::move(trace));
    }
    m_plot->setTraces(std::move(traces));
    updateStatus();
}

void GraphPanel::onDualCursorsToggled(bool checked)
{
    m_plot->setDualCursorsEnabled(checked);
    if (checked) {
        m_toolGroup->setEnabled(true);
    }
}

void GraphPanel::onSubplotsToggled(bool checked)
{
    m_plot->setSubplotLayoutMode(checked ? SubplotLayoutMode::AutoByUnit
                                         : SubplotLayoutMode::SinglePlot);
}

void GraphPanel::onLegendToggled(bool checked)
{
    m_plot->setShowLegend(checked);
}

void GraphPanel::onTimeFormatToggled(bool checked)
{
    m_plot->setTimeDisplayFormat(checked ? TimeDisplayFormat::AbsoluteDateTime
                                         : TimeDisplayFormat::RelativeSeconds);
}

// ---------------------------------------------------------------------------
// Export & Settings
// ---------------------------------------------------------------------------

void GraphPanel::onExportPng()
{
    const QString fileName = QFileDialog::getSaveFileName(
        this, tr("Export Plot Image"), QString{}, tr("PNG Image (*.png);;All Files (*)"));
    if (fileName.isEmpty()) {
        return;
    }

    QPixmap pixmap{m_plot->size() * 2};
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    m_plot->render(&pixmap);

    if (pixmap.save(fileName, "PNG")) {
        m_status->setText(tr("Plot saved to %1").arg(fileName));
    }
}

void GraphPanel::onCopyImage()
{
    QPixmap pixmap{m_plot->size() * 2};
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    m_plot->render(&pixmap);

    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setPixmap(pixmap);
        m_status->setText(tr("Plot image copied to clipboard."));
    }
}

void GraphPanel::onExportCsv()
{
    if (m_windows.empty()) {
        return;
    }

    const QString fileName = QFileDialog::getSaveFileName(
        this, tr("Export Signal Data to CSV"), QString{}, tr("CSV Files (*.csv);;All Files (*)"));
    if (fileName.isEmpty()) {
        return;
    }

    QFile file{fileName};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    QTextStream out{&file};
    // CSV Header
    out << "Timestamp_ns,Time_s";
    for (const SeriesWindow& win : m_windows) {
        out << "," << QString::fromStdString(win.name);
        if (!win.unit.empty()) {
            out << " [" << QString::fromStdString(win.unit) << "]";
        }
    }
    out << "\n";

    // Write samples
    if (!m_windows.front().samples.empty()) {
        for (const SignalSample& sample : m_windows.front().samples) {
            out << sample.timestampNs << ","
                << QString::number(static_cast<double>(sample.timestampNs) / 1e9, 'f', 6);
            for (const SeriesWindow& win : m_windows) {
                // Find nearest sample
                double v = 0.0;
                for (const SignalSample& s : win.samples) {
                    if (s.timestampNs <= sample.timestampNs) {
                        v = s.value;
                    } else {
                        break;
                    }
                }
                out << "," << v;
            }
            out << "\n";
        }
    }

    m_status->setText(tr("Exported data to %1").arg(fileName));
}

void GraphPanel::onSettings()
{
    PlotSettings cur;
    cur.title = m_plot->title();
    cur.layoutMode = m_plot->subplotLayoutMode();
    cur.timeFormat = m_plot->timeDisplayFormat();
    cur.showLegend = m_plot->showLegend();
    cur.showGrid = m_plot->showGrid();
    cur.defaultFill = m_defaultFill;
    cur.defaultLineWidth = m_defaultLineWidth;

    PlotSettingsDialog dlg{cur, this};
    if (dlg.exec() == QDialog::Accepted) {
        const PlotSettings next = dlg.settings();
        m_plot->setTitle(next.title);
        m_plot->setSubplotLayoutMode(next.layoutMode);
        m_plot->setTimeDisplayFormat(next.timeFormat);
        m_plot->setShowLegend(next.showLegend);
        m_plot->setShowGrid(next.showGrid);
        m_defaultFill = next.defaultFill;
        m_defaultLineWidth = next.defaultLineWidth;

        m_actionSubplots->setChecked(next.layoutMode == SubplotLayoutMode::AutoByUnit);
        m_actionLegend->setChecked(next.showLegend);
        m_actionTimeFormat->setChecked(next.timeFormat == TimeDisplayFormat::AbsoluteDateTime);

        m_tracesDirty = true;
        refresh();
    }
}

// ---------------------------------------------------------------------------
// Sidebar & Context Menu
// ---------------------------------------------------------------------------

void GraphPanel::onWindowChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(kWindows.size())) {
        return;
    }

    const std::uint64_t ns = kWindows[static_cast<std::size_t>(index)].nanoseconds;
    if (ns == 0ULL) {
        onFitAll();
        return;
    }

    m_windowNs = ns;
    m_tracesDirty = true;
    refresh();
}

void GraphPanel::onSelectionChanged(QListWidgetItem* /*item*/)
{
    m_tracesDirty = true;
    refresh();
}

void GraphPanel::onSignalFilterChanged(const QString& text)
{
    const QString trimmed = text.trimmed();
    for (int row = 0; row < m_signals->count(); ++row) {
        QListWidgetItem* item = m_signals->item(row);
        if (item != nullptr) {
            item->setHidden(!trimmed.isEmpty()
                            && !item->text().contains(trimmed, Qt::CaseInsensitive));
        }
    }
}

void GraphPanel::onSignalContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_signals->itemAt(pos);
    QMenu menu(this);

    if (item != nullptr) {
        const SeriesId id = static_cast<SeriesId>(item->data(kSeriesIdRole).toULongLong());

        auto* actColor = menu.addAction(tr("Custom Color..."));
        connect(actColor, &QAction::triggered, this, [this, id, item] {
            const QColor cur = colourFor(0, id);
            const QColor picked = QColorDialog::getColor(cur, this, tr("Pick Trace Color"));
            if (picked.isValid()) {
                m_customStyles[id].customColor = picked;
                m_customStyles[id].hasCustomColor = true;
                updateSignalItemVisuals(item, id);
                m_tracesDirty = true;
                refresh();
            }
        });

        auto* actResetColor = menu.addAction(tr("Reset Color to Default"));
        connect(actResetColor, &QAction::triggered, this, [this, id, item] {
            if (m_customStyles.contains(id)) {
                m_customStyles[id].hasCustomColor = false;
                updateSignalItemVisuals(item, id);
                m_tracesDirty = true;
                refresh();
            }
        });

        menu.addSeparator();

        auto* fillMenu = menu.addMenu(tr("Area Fill"));
        auto* actFillNone = fillMenu->addAction(tr("None (Line only)"));
        auto* actFillHatched = fillMenu->addAction(tr("Hatched (Diagonal)"));
        auto* actFillSolid = fillMenu->addAction(tr("Solid (Translucent)"));

        connect(actFillNone, &QAction::triggered, this, [this, id] {
            m_customStyles[id].fillStyle = PlotTraceFill::None;
            m_tracesDirty = true;
            refresh();
        });
        connect(actFillHatched, &QAction::triggered, this, [this, id] {
            m_customStyles[id].fillStyle = PlotTraceFill::Hatched;
            m_tracesDirty = true;
            refresh();
        });
        connect(actFillSolid, &QAction::triggered, this, [this, id] {
            m_customStyles[id].fillStyle = PlotTraceFill::Solid;
            m_tracesDirty = true;
            refresh();
        });

        menu.addSeparator();
    }

    auto* actSelectAll = menu.addAction(tr("Select All"));
    connect(actSelectAll, &QAction::triggered, this, [this] {
        const QSignalBlocker blocker{m_signals};
        for (int i = 0; i < m_signals->count(); ++i) {
            m_signals->item(i)->setCheckState(Qt::Checked);
        }
        m_tracesDirty = true;
        refresh();
    });

    auto* actDeselectAll = menu.addAction(tr("Deselect All"));
    connect(actDeselectAll, &QAction::triggered, this, [this] {
        const QSignalBlocker blocker{m_signals};
        for (int i = 0; i < m_signals->count(); ++i) {
            m_signals->item(i)->setCheckState(Qt::Unchecked);
        }
        m_tracesDirty = true;
        refresh();
    });

    menu.exec(m_signals->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// Cursors & Status
// ---------------------------------------------------------------------------

void GraphPanel::onCursorMoved(quint64 timestampNs)
{
    QStringList parts;
    parts << tr("%1 s").arg(static_cast<double>(timestampNs) / 1'000'000'000.0, 0, 'f', 3);

    for (const SeriesWindow& window : m_windows) {
        if (window.samples.empty()) {
            continue;
        }

        const auto found = std::upper_bound(
            window.samples.begin(),
            window.samples.end(),
            static_cast<std::uint64_t>(timestampNs),
            [](std::uint64_t at, const SignalSample& point) { return at < point.timestampNs; });

        if (found == window.samples.begin()) {
            continue;
        }

        const SignalSample& point = *(found - 1);
        parts << tr("%1 %2 %3")
                     .arg(QString::fromStdString(window.name))
                     .arg(point.value, 0, 'f', 2)
                     .arg(QString::fromStdString(window.unit));
    }

    m_cursorText = parts.join(QStringLiteral("   "));
    m_status->setText(m_cursorText);
}

void GraphPanel::onCursorLeft()
{
    m_cursorText.clear();
    updateStatus();
}

void GraphPanel::onTimeRangeChanged(quint64 startNs, quint64 /*endNs*/)
{
    if (m_store == nullptr || m_selected.empty()) {
        return;
    }

    // Entering interactive history viewing pauses live tail follow
    if (!m_frozen) {
        m_frozen = true;
        m_userInteracting = true;
        updatePlayPauseUi();
    }

    m_store->readWindows(m_selected, startNs, kMaximumPointsPerTrace, m_windows);

    std::vector<PlotTrace> traces;
    traces.reserve(m_windows.size());
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
        PlotTrace trace;
        trace.name = QString::fromStdString(m_windows[i].name);
        trace.unit = QString::fromStdString(m_windows[i].unit);
        trace.colour = colourFor(i, m_windows[i].id);
        trace.samples = std::move(m_windows[i].samples);
        trace.minimum = m_windows[i].minimum;
        trace.maximum = m_windows[i].maximum;
        trace.lineWidth = m_defaultLineWidth;
        if (m_customStyles.contains(m_windows[i].id)) {
            trace.fillStyle = m_customStyles[m_windows[i].id].fillStyle;
            trace.subplotIndex = m_customStyles[m_windows[i].id].subplotIndex;
        } else {
            trace.fillStyle = m_defaultFill;
        }
        traces.push_back(std::move(trace));
    }

    m_plot->setTraces(std::move(traces));
    updateStatus();
}

void GraphPanel::onMeasurementChanged(quint64 cursorANs, quint64 cursorBNs, qint64 deltaNs)
{
    const double secA = static_cast<double>(cursorANs) / 1e9;
    const double secB = static_cast<double>(cursorBNs) / 1e9;
    const double dSec = static_cast<double>(deltaNs) / 1e9;
    const double freq = std::abs(dSec) > 1e-6 ? 1.0 / std::abs(dSec) : 0.0;

    m_status->setText(tr("Cursor A: %1 s | Cursor B: %2 s | Δt: %3 s (%4 Hz)")
                          .arg(secA, 0, 'f', 3)
                          .arg(secB, 0, 'f', 3)
                          .arg(dSec, 0, 'f', 3)
                          .arg(freq, 0, 'f', 2));
}

void GraphPanel::updateStatus()
{
    if (!m_cursorText.isEmpty()) {
        return;
    }

    if (m_store == nullptr) {
        m_status->setText(tr("Not measuring."));
        return;
    }

    const int listed = m_signals->count();
    if (listed == 0) {
        m_status->setText(tr("No signals yet. Wire a DBC Decoder to a Signal Plot block "
                             "on the Pipeline canvas, then press Start."));
        return;
    }

    const int checked = static_cast<int>(selectedSeries().size());
    QString text = tr("%n signal(s), %1 plotted.", nullptr, listed).arg(checked);

    if (m_frozen) {
        text += tr("  Paused - press Run to follow live measurement stream.");
    }

    if (const std::uint64_t aged = m_store->discarded(); aged > 0) {
        text += tr("  %1 sample(s) have aged out.").arg(aged);
    }

    m_status->setText(text);
}

void GraphPanel::onThemeChanged()
{
    ThemeManager* themes = ThemeManager::instance();
    const auto getIcon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    m_actionPlay->setIcon(getIcon("start"));
    m_actionPause->setIcon(getIcon("pause"));
    m_actionStop->setIcon(getIcon("stop"));
    m_actionClear->setIcon(getIcon("clear"));

    m_actionPointer->setIcon(getIcon("pointer"));
    m_actionPan->setIcon(getIcon("pan"));
    m_actionZoomBox->setIcon(getIcon("zoom-box"));
    m_actionZoomIn->setIcon(getIcon("zoom-in"));
    m_actionZoomOut->setIcon(getIcon("zoom-out"));
    m_actionFitAll->setIcon(getIcon("fit"));
    m_actionDualCursors->setIcon(getIcon("cursors"));
    m_actionSubplots->setIcon(getIcon("subplots"));
    m_actionLegend->setIcon(getIcon("legend"));
    m_actionTimeFormat->setIcon(getIcon("time-format"));
    m_actionSettings->setIcon(getIcon("settings"));

    // Update swatches in sidebar
    for (int row = 0; row < m_signals->count(); ++row) {
        QListWidgetItem* item = m_signals->item(row);
        if (item != nullptr) {
            const SeriesId id = static_cast<SeriesId>(item->data(kSeriesIdRole).toULongLong());
            updateSignalItemVisuals(item, id);
        }
    }

    m_plot->update();
    refresh();
}

} // namespace torquebus::ui
