// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TracePanel.h"

#include "ui/theme/ThemeManager.h"
#include "ui/trace/TraceModel.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

/// Starting width per column, in pixels. Sized to the widest value each column
/// actually holds - an eight-digit extended identifier, sixty-four hex bytes -
/// so the table stops reflowing after the first screenful.
constexpr int kColumnWidths[TraceModel::ColumnCount] = {
    96, // Time
    72, // Delta
    56, // Channel
    40, // Direction
    88, // Identifier
    140, // Name
    64, // Type
    40, // DLC
    260, // Data
    280, // Signals - the widest column, because a decoded row is the point
    72, // Cycle
    72, // Count
    64, // Flags
};

/// How close to the bottom still counts as "at the bottom".
///
/// Not zero: a scrollbar lands a pixel or two short after a programmatic
/// scroll, and treating that as "the user scrolled away" would switch following
/// off at random.
constexpr int kBottomTolerance = 4;

} // namespace

TracePanel::TracePanel(QWidget* parent)
    : QWidget{parent}
{
    createToolBar();
    createView();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_view, 1);

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, &TracePanel::onThemeChanged);
    }

    // The counters are cheap and only need to look live, not be exact.
    auto* statusTimer = new QTimer(this);
    statusTimer->setInterval(250);
    connect(statusTimer, &QTimer::timeout, this, &TracePanel::refreshStatus);
    statusTimer->start();
}

void TracePanel::createToolBar()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.trace"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    ThemeManager* themes = ThemeManager::instance();
    const auto icon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    m_actionFreeze = new QAction(icon("pause"), tr("Freeze"), this);
    m_actionFreeze->setCheckable(true);
    m_actionFreeze->setToolTip(tr("Stop adding new frames to this view. The measurement, "
                                  "the logger and the counters keep running."));
    connect(m_actionFreeze, &QAction::toggled, this, &TracePanel::onFreezeToggled);

    m_actionClear = new QAction(icon("clear"), tr("Clear"), this);
    m_actionClear->setToolTip(tr("Discard the frames currently held by this view"));
    connect(m_actionClear, &QAction::triggered, this, &TracePanel::onClear);

    m_actionFollow = new QAction(icon("trace"), tr("Follow"), this);
    m_actionFollow->setCheckable(true);
    m_actionFollow->setChecked(true);
    m_actionFollow->setToolTip(tr("Scroll to the newest frame as it arrives. Turns itself "
                                  "off when you scroll up to read."));
    connect(m_actionFollow, &QAction::toggled, this, [this](bool follow) {
        m_following = follow;
        if (follow && m_view != nullptr) {
            m_scrollingProgrammatically = true;
            m_view->scrollToBottom();
            m_scrollingProgrammatically = false;
        }
    });

    m_toolBar->addAction(m_actionFreeze);
    m_toolBar->addAction(m_actionClear);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_actionFollow);

    auto* spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

    m_statusLabel = new QLabel(m_toolBar);
    m_statusLabel->setProperty("torquebusState", "ready");
    m_toolBar->addWidget(m_statusLabel);
}

void TracePanel::createView()
{
    m_model = new TraceModel(this);

    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setFrameShape(QFrame::NoFrame);

    // The settings that make a million-row table survivable. Without
    // uniform row heights, Qt measures every row to size the scrollbar - which
    // at a million rows is a freeze, not a slowdown.
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setDefaultSectionSize(18);
    m_view->setShowGrid(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setWordWrap(false);
    m_view->setTextElideMode(Qt::ElideRight);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setHighlightSections(false);

    applyColumnWidths();

    connect(m_model, &TraceModel::rowsAppended, this, &TracePanel::onRowsAppended);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &TracePanel::onScrolled);
}

void TracePanel::applyColumnWidths()
{
    for (int column = 0; column < TraceModel::ColumnCount; ++column) {
        m_view->setColumnWidth(column, kColumnWidths[column]);
    }
}

void TracePanel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_model->setDatabases(std::move(databases));
}

void TracePanel::applyPreferences(int refreshMs, bool decimalIdentifiers)
{
    m_model->setRefreshIntervalMs(refreshMs);
    m_model->setDecimalIdentifiers(decimalIdentifiers);
}

void TracePanel::setStore(const TraceStore* store)
{
    m_store = store;
    m_model->setStore(store);
    refreshStatus();
}

void TracePanel::onRowsAppended(int firstRow, int lastRow)
{
    (void)firstRow;

    if (!m_following || m_view == nullptr) {
        return;
    }

    m_scrollingProgrammatically = true;
    m_view->scrollTo(m_model->index(lastRow, 0), QAbstractItemView::PositionAtBottom);
    m_scrollingProgrammatically = false;
}

void TracePanel::onScrolled()
{
    if (m_scrollingProgrammatically || m_view == nullptr) {
        return;
    }

    // Following is a consequence of where the user is looking, not a mode they
    // have to manage: scroll up to read and it stops, scroll back down and it
    // resumes. The toolbar button reflects that rather than driving it.
    const QScrollBar* bar = m_view->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - kBottomTolerance;

    if (atBottom != m_following) {
        m_following = atBottom;

        QSignalBlocker blocker{m_actionFollow};
        m_actionFollow->setChecked(atBottom);
    }
}

void TracePanel::onFreezeToggled(bool frozen)
{
    m_model->setFrozen(frozen);

    m_actionFreeze->setText(frozen ? tr("Resume") : tr("Freeze"));

    if (m_statusLabel != nullptr) {
        m_statusLabel->setProperty("torquebusState", frozen ? "warning" : "ready");
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
    }
}

void TracePanel::onClear()
{
    // Clears the view only. The store belongs to the pipeline node, and
    // emptying a measurement's data from a view's button would be a surprise -
    // that belongs to Stop, or to a new measurement.
    m_model->reset();
}

void TracePanel::onThemeChanged(const Theme& theme)
{
    (void)theme;

    if (ThemeManager* themes = ThemeManager::instance()) {
        m_actionFreeze->setIcon(themes->icon(QStringLiteral("pause")));
        m_actionClear->setIcon(themes->icon(QStringLiteral("clear")));
        m_actionFollow->setIcon(themes->icon(QStringLiteral("trace")));
    }

    // The model paints from the theme, so every visible cell needs repainting.
    if (m_view != nullptr) {
        m_view->viewport()->update();
    }
}

void TracePanel::refreshStatus()
{
    if (m_statusLabel == nullptr) {
        return;
    }

    if (m_store == nullptr) {
        m_statusLabel->setText(tr("No measurement"));
        return;
    }

    const std::uint64_t total = m_store->totalAppended();
    const std::uint64_t dropped = m_store->discarded();

    if (dropped > 0) {
        // Said plainly rather than hidden: the user is looking at a window onto
        // a longer measurement, and needs to know that scrolling to the top is
        // not the beginning.
        m_statusLabel->setText(tr("%L1 frames  ·  showing the last %L2  ·  %L3 scrolled off")
                                   .arg(total)
                                   .arg(m_store->size())
                                   .arg(dropped));
    } else {
        m_statusLabel->setText(
            tr("%L1 frames  ·  %L2 identifiers").arg(total).arg(m_store->identifiers().size()));
    }
}

} // namespace torquebus::ui
