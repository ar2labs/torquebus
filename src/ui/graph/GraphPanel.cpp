// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/graph/GraphPanel.h"

#include "ui/graph/PlotView.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSplitter>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <array>

namespace torquebus::ui {
namespace {

/// 20 Hz. Fast enough that a line looks live, slow enough that the executor is
/// interrupted for the store's lock fifty times a second rather than sixty -
/// and slow enough that a plot is never the reason a repaint is late.
constexpr int kRefreshMs = 50;

/// Points per trace per repaint. A plot is under a thousand pixels wide, so
/// more than this is more polyline vertices than the widget has columns to draw
/// them in - work whose result nobody can see.
constexpr std::size_t kMaximumPointsPerTrace = 2048;

constexpr int kSeriesIdRole = Qt::UserRole + 1;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// The windows offered, and what they are worth looking at.
struct WindowChoice final {
    const char* label;
    std::uint64_t nanoseconds;
};

constexpr std::array<WindowChoice, 6> kWindows{{
    {"1 s", 1'000'000'000ULL},
    {"5 s", 5'000'000'000ULL},
    {"10 s", 10'000'000'000ULL},
    {"30 s", 30'000'000'000ULL},
    {"1 min", 60'000'000'000ULL},
    {"5 min", 300'000'000'000ULL},
}};

/// The index of the default window, which is the third entry above.
constexpr int kDefaultWindowIndex = 2;

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
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.graph"));
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_actionFreeze = m_toolBar->addAction(tr("Freeze"));
    m_actionFreeze->setCheckable(true);
    m_actionFreeze->setToolTip(tr("Stop the plot from following new samples. The pipeline "
                                  "keeps recording, so unfreezing catches up."));

    m_actionClear = m_toolBar->addAction(tr("Clear"));
    m_actionClear->setToolTip(tr("Discard the samples every signal is holding. The "
                                 "signals themselves, and what is ticked, stay."));

    m_toolBar->addSeparator();

    m_window = new QComboBox(m_toolBar);
    for (const WindowChoice& choice : kWindows) {
        m_window->addItem(QString::fromLatin1(choice.label),
                          QVariant::fromValue(static_cast<qulonglong>(choice.nanoseconds)));
    }
    m_window->setCurrentIndex(kDefaultWindowIndex);
    m_window->setToolTip(tr("How much time the plot shows."));
    m_toolBar->addWidget(m_window);

    m_signals = new QListWidget(this);
    m_signals->setObjectName(QStringLiteral("torquebus.list.plotSignals"));
    m_signals->setFrameShape(QFrame::NoFrame);
    m_signals->setAlternatingRowColors(true);
    m_signals->setUniformItemSizes(true);
    m_signals->setMinimumWidth(150);
    m_signals->setMaximumWidth(340);

    m_plot = new PlotView(this);
    m_plot->setPlaceholder(tr("Tick a signal to plot it."));

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.graph"));
    m_splitter->addWidget(m_signals);
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

    connect(m_actionFreeze, &QAction::toggled, this, &GraphPanel::onFreezeToggled);
    connect(m_actionClear, &QAction::triggered, this, &GraphPanel::onClear);
    connect(m_window, &QComboBox::currentIndexChanged, this, &GraphPanel::onWindowChanged);
    connect(m_signals, &QListWidget::itemChanged, this, &GraphPanel::onSelectionChanged);
    connect(m_plot, &PlotView::cursorMoved, this, &GraphPanel::onCursorMoved);
    connect(m_plot, &PlotView::cursorLeft, this, &GraphPanel::onCursorLeft);

    m_windowNs = kWindows[kDefaultWindowIndex].nanoseconds;

    updateStatus();
}

void GraphPanel::setStore(SignalSeriesStore* store)
{
    m_store = store;

    m_signals->clear();
    m_listed.clear();
    m_selected.clear();
    m_windows.clear();

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

    if (!m_splitter->restoreState(state)) {
        qWarning("TorqueBus: the saved Graph divider position could not be restored.");
    }
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

void GraphPanel::refresh()
{
    if (m_store == nullptr || !isVisible()) {
        return;
    }

    syncSignalList();

    if (m_frozen) {
        return;
    }

    m_selected = selectedSeries();

    if (m_selected.empty()) {
        m_plot->setTraces({});
        updateStatus();
        return;
    }

    // The window ends at the newest sample in *any* series, not at the newest
    // in the ones being drawn: a signal that has gone quiet should not drag the
    // view back to when it last spoke.
    const std::uint64_t newest = m_store->newestTimestampNs();
    const std::uint64_t start = newest > m_windowNs ? newest - m_windowNs : 0;

    m_store->readWindows(m_selected, start, kMaximumPointsPerTrace, m_windows);

    std::vector<PlotTrace> traces;
    traces.reserve(m_windows.size());

    for (std::size_t index = 0; index < m_windows.size(); ++index) {
        const SeriesWindow& window = m_windows[index];

        PlotTrace trace;
        trace.name = QString::fromStdString(window.name);
        trace.unit = QString::fromStdString(window.unit);
        trace.colour = colourFor(index);
        trace.samples = window.samples;
        trace.minimum = window.minimum;
        trace.maximum = window.maximum;

        traces.push_back(std::move(trace));
    }

    m_plot->setWindow(start, std::max(newest, start + m_windowNs));
    m_plot->setTraces(std::move(traces));

    updateStatus();
}

void GraphPanel::syncSignalList()
{
    const std::vector<SeriesInfo> infos = m_store->listSeries();

    for (const SeriesInfo& info : infos) {
        const QString name = QString::fromStdString(info.name);
        if (m_listed.contains(name)) {
            continue;
        }

        auto* item = new QListWidgetItem(name, m_signals);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(kSeriesIdRole, QVariant::fromValue(static_cast<qulonglong>(info.id)));

        if (!info.unit.empty()) {
            item->setToolTip(tr("%1 in %2").arg(name, QString::fromStdString(info.unit)));
        }

        m_listed.insert(name, info.id);
    }
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

QColor GraphPanel::colourFor(std::size_t index) const
{
    const std::array<AccentColor, 8> palette = accentColors();

    // Cycled rather than run out of. Eight distinguishable lines is already
    // more than a plot stays readable with, and the ninth being the colour of
    // the first is a better answer than the ninth being invisible.
    const AccentColor accent = palette[index % palette.size()];

    return accentPairFor(accent, currentTheme().variant).accent;
}

// ---------------------------------------------------------------------------
// The toolbar
// ---------------------------------------------------------------------------

void GraphPanel::onFreezeToggled(bool frozen)
{
    m_frozen = frozen;
    updateStatus();
}

void GraphPanel::onClear()
{
    if (m_store == nullptr) {
        return;
    }

    m_store->clearSamples();

    m_plot->setTraces({});
    updateStatus();
}

void GraphPanel::onWindowChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(kWindows.size())) {
        return;
    }

    m_windowNs = kWindows[static_cast<std::size_t>(index)].nanoseconds;
    refresh();
}

void GraphPanel::onSelectionChanged(QListWidgetItem* /*item*/)
{
    // Redrawn now rather than at the next tick: a tick is a click, and 50 ms of
    // nothing happening after a click reads as a click that did not register.
    refresh();
}

// ---------------------------------------------------------------------------
// The cursor and the status line
// ---------------------------------------------------------------------------

void GraphPanel::onCursorMoved(quint64 timestampNs)
{
    QStringList parts;
    parts << tr("%1 s").arg(static_cast<double>(timestampNs) / 1'000'000'000.0, 0, 'f', 3);

    for (const SeriesWindow& window : m_windows) {
        if (window.samples.empty()) {
            continue;
        }

        // The last sample at or before the cursor - what the signal was holding
        // at that instant. Interpolating between two samples would draw a value
        // the bus never carried, which on a signal that changes in steps is a
        // reading somebody could act on.
        const auto found = std::upper_bound(
            window.samples.begin(), window.samples.end(),
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
        text += tr("  Frozen - the pipeline is still recording.");
    }

    if (const std::uint64_t aged = m_store->discarded(); aged > 0) {
        // Said out loud, because a plot that silently begins part-way through a
        // measurement is a plot somebody draws a wrong conclusion from.
        text += tr("  %1 sample(s) have aged out.").arg(aged);
    }

    m_status->setText(text);
}

void GraphPanel::onThemeChanged()
{
    // The plot reads the theme at paint time, and the line colours are derived
    // from the accent - so both follow a theme change on the next repaint,
    // which the refresh timer provides. Repainted here anyway so a change made
    // while the measurement is stopped is visible immediately.
    m_plot->update();
    refresh();
}

} // namespace torquebus::ui
