// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/statistics/StatisticsPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QShowEvent>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace torquebus::ui {
namespace {

enum ChannelColumn : int {
    ColumnChannel = 0,
    ColumnInterface,
    ColumnState,
    ColumnRx,
    ColumnTx,
    ColumnErrors,
    ColumnFiltered,
    ColumnDropped,
    ColumnRate,
    ColumnLoad,
    ColumnPeak,
    ColumnBitrate,
    ColumnChannelCount
};

enum NodeColumn : int { NodeColumnName = 0, NodeColumnValue, NodeColumnCount };

/// The current theme, or a dark one if the manager is not up yet.
///
/// Returned by value and not by reference, the same way DockChrome does it: a
/// reference bound to one branch of a conditional and a temporary in the other
/// is a lifetime question nobody should have to answer while reading a paint
/// routine.
[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// The load percentage a bar is drawn from, on the cell that shows it.
constexpr int kLoadRole = Qt::UserRole + 1;

/// Node name and type, so the tree can be compared without re-reading its text.
constexpr int kNodeKeyRole = Qt::UserRole + 2;

/// Paints a proportional bar behind the bus load figure.
///
/// A percentage is a number you have to read; a bar is one you can see from
/// across the room, which is what a bus load indicator is for - nobody watches
/// this column to learn that the load is 41.6%, they watch it to notice that it
/// has doubled. Every measurement tool in this category draws one, and the
/// reason is the same one.
///
/// Painted here rather than with a QProgressBar per cell: a widget in every row
/// costs a widget in every row, and this has to repaint ten times a second.
class LoadBarDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QString text = opt.text;

        // The style still draws the row: selection, alternating colours and the
        // focus rectangle are its business, not this delegate's. Only the text
        // is taken away from it, so it can be put back on top of the bar.
        opt.text.clear();

        QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const double percent = std::clamp(index.data(kLoadRole).toDouble(), 0.0, 100.0);
        const QRect area = opt.rect.adjusted(2, 3, -2, -3);

        if (percent > 0.0 && area.width() > 0) {
            const int width = static_cast<int>(std::lround(area.width() * percent / 100.0));

            painter->save();
            painter->setPen(Qt::NoPen);
            painter->fillRect(QRect{area.left(), area.top(), std::max(width, 1), area.height()},
                              barColor(percent));
            painter->restore();
        }

        painter->save();
        painter->setPen(opt.palette.color(opt.state.testFlag(QStyle::State_Selected)
                                              ? QPalette::HighlightedText
                                              : QPalette::Text));
        painter->drawText(opt.rect.adjusted(4, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter, text);
        painter->restore();
    }

private:
    /// Green, amber, red.
    ///
    /// 80% is the same threshold, for the same reason, that the status bar
    /// already colours the bus load at: past it a CAN bus has no comfortable
    /// headroom for arbitration and latency on low-priority frames climbs
    /// sharply. Two places in one window disagreeing about when a bus is busy
    /// would be worse than either number being slightly wrong.
    ///
    /// 95% is not a second opinion but a different statement: at that load the
    /// bus is saturated and frames are being delayed, not merely at risk.
    [[nodiscard]] static QColor barColor(double percent)
    {
        const Theme theme = currentTheme();

        QColor color = theme.success;
        if (percent >= 95.0) {
            color = theme.error;
        } else if (percent >= 80.0) {
            color = theme.warning;
        }

        // Behind text, so it has to stay a background. A saturated bar would
        // win the row and make the figure on top of it unreadable.
        color.setAlpha(90);
        return color;
    }
};

[[nodiscard]] QString formatCount(quint64 value)
{
    return QString::number(value);
}

[[nodiscard]] QString formatRate(double value)
{
    return QStringLiteral("%1").arg(value, 0, 'f', 0);
}

[[nodiscard]] QString formatPercent(double value)
{
    return QStringLiteral("%1 %").arg(value, 0, 'f', 1);
}

/// Writes a right-aligned cell, creating it the first time.
///
/// Reused rather than replaced: this runs ten times a second on every cell of
/// every channel, and allocating a fresh QTableWidgetItem per cell per tick
/// would throw away the selection with it.
void setCell(QTableWidget* table, int row, int column, const QString& text, bool numeric)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr) {
        item = new QTableWidgetItem;
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setTextAlignment(numeric ? (Qt::AlignRight | Qt::AlignVCenter)
                                       : (Qt::AlignLeft | Qt::AlignVCenter));
        table->setItem(row, column, item);
    }

    if (item->text() != text) {
        item->setText(text);
    }
}

} // namespace

StatisticsPanel::StatisticsPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }
}

void StatisticsPanel::buildUi()
{
    m_channels = new QTableWidget(this);
    m_channels->setColumnCount(ColumnChannelCount);
    m_channels->setHorizontalHeaderLabels({tr("Channel"),
                                           tr("Interface"),
                                           tr("State"),
                                           tr("Rx"),
                                           tr("Tx"),
                                           tr("Errors"),
                                           tr("Filtered"),
                                           tr("Dropped"),
                                           tr("Frames/s"),
                                           tr("Load"),
                                           tr("Peak"),
                                           tr("Bitrate")});
    m_channels->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_channels->setSelectionMode(QAbstractItemView::SingleSelection);
    m_channels->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_channels->setAlternatingRowColors(true);
    m_channels->setFrameShape(QFrame::NoFrame);
    m_channels->verticalHeader()->setVisible(false);
    m_channels->horizontalHeader()->setStretchLastSection(false);
    m_channels->setColumnWidth(ColumnChannel, 70);
    m_channels->setColumnWidth(ColumnInterface, 170);
    m_channels->setColumnWidth(ColumnState, 90);
    m_channels->setColumnWidth(ColumnRx, 80);
    m_channels->setColumnWidth(ColumnTx, 80);
    m_channels->setColumnWidth(ColumnErrors, 64);
    m_channels->setColumnWidth(ColumnFiltered, 72);
    m_channels->setColumnWidth(ColumnDropped, 72);
    m_channels->setColumnWidth(ColumnRate, 74);
    m_channels->setColumnWidth(ColumnLoad, 84);
    m_channels->setColumnWidth(ColumnPeak, 66);
    m_channels->setColumnWidth(ColumnBitrate, 84);

    // The delegate outlives nothing: the table is its parent, so it goes when
    // the panel does.
    m_channels->setItemDelegateForColumn(ColumnLoad, new LoadBarDelegate(m_channels));

    // A tree and not a second table, because the data is a tree: a node reports
    // however many counters it chose, and flattening that would repeat the
    // block's name down the rows and lose which counters belong together.
    m_nodes = new QTreeWidget(this);
    m_nodes->setColumnCount(NodeColumnCount);
    m_nodes->setHeaderLabels({tr("Pipeline block"), tr("Count")});
    m_nodes->setAlternatingRowColors(true);
    m_nodes->setFrameShape(QFrame::NoFrame);
    m_nodes->setUniformRowHeights(true);
    m_nodes->setRootIsDecorated(true);
    m_nodes->setColumnWidth(NodeColumnName, 320);
    m_nodes->header()->setStretchLastSection(true);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.statistics"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_channels);
    m_splitter->addWidget(m_nodes);

    // The channels table has one row per interface and will not grow; the tree
    // grows with the project. So extra height goes to the tree.
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 6, 4);
    layout->setSpacing(2);
    layout->addWidget(m_splitter, 1);
    layout->addWidget(m_status);

    m_status->setText(tr("Not measuring."));
}

QByteArray StatisticsPanel::splitterState() const
{
    return m_splitter != nullptr ? m_splitter->saveState() : QByteArray{};
}

void StatisticsPanel::restoreSplitterState(const QByteArray& state)
{
    if (m_splitter == nullptr || state.isEmpty()) {
        return;
    }

    if (!m_splitter->restoreState(state)) {
        qWarning("TorqueBus: the saved Statistics divider position could not be restored.");
    }
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void StatisticsPanel::setChannels(const QList<ChannelStatus>& channels)
{
    m_lastChannels = channels;

    // Hidden behind another tab: the numbers are still arriving and are kept,
    // but writing them into a table nobody can see is work with no reader.
    // showEvent() puts them on screen the moment the tab is brought forward.
    if (isVisible()) {
        applyChannels();
    }
}

void StatisticsPanel::applyChannels()
{
    const QList<ChannelStatus>& channels = m_lastChannels;

    if (m_channels->rowCount() != channels.size()) {
        m_channels->setRowCount(static_cast<int>(channels.size()));
    }

    for (int row = 0; row < channels.size(); ++row) {
        const ChannelStatus& status = channels.at(row);

        setCell(m_channels, row, ColumnChannel, status.name, false);
        setCell(m_channels, row, ColumnInterface, status.deviceName, false);
        setCell(m_channels, row, ColumnState, status.stateText, false);
        setCell(m_channels, row, ColumnRx, formatCount(status.rxFrames), true);
        setCell(m_channels, row, ColumnTx, formatCount(status.txFrames), true);
        setCell(m_channels, row, ColumnErrors, formatCount(status.errorFrames), true);
        setCell(m_channels, row, ColumnFiltered, formatCount(status.filteredFrames), true);
        setCell(m_channels, row, ColumnDropped, formatCount(status.droppedFrames), true);
        setCell(m_channels, row, ColumnRate, formatRate(status.framesPerSecond), true);
        setCell(m_channels, row, ColumnLoad, formatPercent(status.busLoadPercent), true);
        setCell(m_channels, row, ColumnPeak, formatPercent(status.peakBusLoadPercent), true);
        setCell(m_channels, row, ColumnBitrate, tr("%1 kbit/s").arg(status.bitrate / 1000), true);

        // The bar reads this rather than parsing the text back out of the cell.
        if (QTableWidgetItem* load = m_channels->item(row, ColumnLoad)) {
            load->setData(kLoadRole, status.busLoadPercent);
        }

        // The state token is what the style sheet colours the row's state cell
        // by, so a bus-off channel is red here for the same reason and by the
        // same rule as in the status bar.
        if (QTableWidgetItem* state = m_channels->item(row, ColumnState)) {
            const Theme theme = currentTheme();
            if (status.stateToken == QLatin1String("error")) {
                state->setForeground(theme.error);
            } else if (status.stateToken == QLatin1String("warning")) {
                state->setForeground(theme.warning);
            } else if (status.stateToken == QLatin1String("online")) {
                state->setForeground(theme.success);
            } else {
                state->setForeground(theme.textMuted);
            }
        }
    }

    updateStatusLine();
}

// ---------------------------------------------------------------------------
// Pipeline nodes
// ---------------------------------------------------------------------------

void StatisticsPanel::setNodes(const QList<NodeStatus>& nodes)
{
    m_lastNodes = nodes;

    if (isVisible()) {
        applyNodes();
    }
}

void StatisticsPanel::applyNodes()
{
    const QList<NodeStatus>& nodes = m_lastNodes;

    if (!treeMatches(nodes)) {
        rebuildTree(nodes);
    }

    for (int index = 0; index < nodes.size(); ++index) {
        QTreeWidgetItem* parent = m_nodes->topLevelItem(index);
        if (parent == nullptr) {
            continue;
        }

        const QList<NodeCounter>& counters = nodes.at(index).counters;
        for (int child = 0; child < counters.size(); ++child) {
            QTreeWidgetItem* item = parent->child(child);
            if (item == nullptr) {
                continue;
            }

            const QString text = formatCount(counters.at(child).value);
            if (item->text(NodeColumnValue) != text) {
                item->setText(NodeColumnValue, text);
            }
        }
    }

    updateStatusLine();
}

/// One line that says what the panel is showing, in the order it matters.
///
/// The pipeline count wins when there is one: a person reading this panel with
/// a measurement running already knows how many buses they connected, and what
/// they are checking is that their blocks are running.
void StatisticsPanel::updateStatusLine()
{
    if (!m_lastNodes.isEmpty()) {
        m_status->setText(
            tr("%n pipeline block(s) reporting.", nullptr, static_cast<int>(m_lastNodes.size())));
        return;
    }

    if (m_lastChannels.isEmpty()) {
        m_status->setText(tr("No channels are bound."));
        return;
    }

    m_status->setText(tr("%n channel(s). No pipeline blocks are running.",
                         nullptr,
                         static_cast<int>(m_lastChannels.size())));
}

void StatisticsPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    applyChannels();
    applyNodes();
}

bool StatisticsPanel::treeMatches(const QList<NodeStatus>& nodes) const
{
    if (m_nodes->topLevelItemCount() != nodes.size()) {
        return false;
    }

    for (int index = 0; index < nodes.size(); ++index) {
        const QTreeWidgetItem* parent = m_nodes->topLevelItem(index);
        if (parent == nullptr) {
            return false;
        }

        const NodeStatus& node = nodes.at(index);
        if (parent->data(NodeColumnName, kNodeKeyRole).toString() != node.name) {
            return false;
        }

        if (parent->childCount() != node.counters.size()) {
            return false;
        }

        // The labels too, not only how many. A node that swaps one counter for
        // another keeps its count and would otherwise go on displaying the old
        // label against the new value.
        for (int child = 0; child < node.counters.size(); ++child) {
            const QTreeWidgetItem* item = parent->child(child);
            if (item == nullptr || item->text(NodeColumnName) != node.counters.at(child).label) {
                return false;
            }
        }
    }

    return true;
}

void StatisticsPanel::rebuildTree(const QList<NodeStatus>& nodes)
{
    m_nodes->clear();

    for (const NodeStatus& node : nodes) {
        auto* parent = new QTreeWidgetItem(m_nodes);

        // "DBC decoder  (dbc.decoder)" - the name the user gave the block, and
        // the type it is. Both, because a project with three decoders names
        // them and a project with one usually does not.
        parent->setText(NodeColumnName, tr("%1  (%2)").arg(node.name, node.typeName));
        parent->setData(NodeColumnName, kNodeKeyRole, node.name);
        parent->setFirstColumnSpanned(true);
        parent->setExpanded(true);

        for (const NodeCounter& counter : node.counters) {
            auto* item = new QTreeWidgetItem(parent);
            item->setText(NodeColumnName, counter.label);
            item->setText(NodeColumnValue, formatCount(counter.value));
            item->setTextAlignment(NodeColumnValue, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
}

void StatisticsPanel::onThemeChanged()
{
    // The bar delegate reads the theme at paint time, so it only needs telling
    // that it is out of date. The state column is re-tinted on the next tick,
    // which is at most 100 ms away - and if nothing is running, a repaint would
    // have nothing new to say anyway.
    m_channels->viewport()->update();
    m_nodes->viewport()->update();
}

} // namespace torquebus::ui
