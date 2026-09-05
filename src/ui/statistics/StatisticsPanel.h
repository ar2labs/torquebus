// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Statistics panel: what the buses and the pipeline have counted.
//
// Two tables, because there are two questions and they are not the same one.
//
// **The channels table answers "is the bus healthy?"** - load, rate, errors,
// what the driver dropped. It is about the wire, and it would read the same if
// the pipeline were empty.
//
// **The nodes table answers "is my measurement doing what I drew?"** - how many
// frames the filter refused, how many a decoder could not find in its database,
// how many signals a script emitted. Until now those counters existed and had
// nowhere to be seen, which made a whole class of mistake invisible: a decoder
// wired to the wrong database looks exactly like a quiet bus, and the number
// that tells them apart is "frames not in the database".
//
// Both are fed by CanEngineController's signals and neither touches the engine.
// That is not fussiness - the counters live on the dispatch thread and the only
// safe reading of them is the snapshot the controller hands over on the GUI
// thread (see CanEngine::nodeStatistics).

#pragma once

#include "ui/engine/CanEngineController.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QShowEvent;
class QSplitter;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus::ui {

class StatisticsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit StatisticsPanel(QWidget* parent = nullptr);

    /// The position of the divider between the two tables.
    ///
    /// Saved here because the dock layout saver only knows about docks and the
    /// boundaries between them; a splitter a panel put inside itself is
    /// invisible to it.
    [[nodiscard]] QByteArray splitterState() const;

    /// Ignores an empty or unusable state, leaving the default proportions.
    void restoreSplitterState(const QByteArray& state);

public Q_SLOTS:
    /// One row per channel. Connected to CanEngineController::statusUpdated.
    void setChannels(const QList<ChannelStatus>& channels);

    /// One branch per node. Connected to
    /// CanEngineController::nodeStatisticsUpdated.
    void setNodes(const QList<NodeStatus>& nodes);

protected:
    /// Fills the tables in from the last payload received while hidden.
    ///
    /// Without this the panel would be blank until the next tick, and when the
    /// measurement is stopped there is no next tick - so opening the Statistics
    /// tab after a run would show nothing at all, which is the moment somebody
    /// most wants to read the final counts.
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onThemeChanged();

private:
    void buildUi();

    /// Writes m_lastChannels / m_lastNodes into the widgets.
    void applyChannels();
    void applyNodes();

    void updateStatusLine();

    /// True when the tree already has exactly these nodes with these labels.
    ///
    /// The shape of the pipeline changes when a project is loaded and at no
    /// other time, while the values change ten times a second. Rebuilding the
    /// tree on every tick would work and would also close every branch the user
    /// had opened, twice a second, forever - so the values are written into the
    /// existing items and the tree is rebuilt only when the shape actually
    /// moved.
    [[nodiscard]] bool treeMatches(const QList<NodeStatus>& nodes) const;

    void rebuildTree(const QList<NodeStatus>& nodes);

    QSplitter* m_splitter{nullptr};
    QTableWidget* m_channels{nullptr};
    QTreeWidget* m_nodes{nullptr};
    QLabel* m_status{nullptr};

    /// The last figures received, whether or not they were displayed.
    ///
    /// Kept because the panel spends most of its life behind another tab and
    /// writing into a table nobody can see is work with no reader - but the
    /// numbers still have to be there the moment it is brought forward.
    QList<ChannelStatus> m_lastChannels;
    QList<NodeStatus> m_lastNodes;
};

} // namespace torquebus::ui
