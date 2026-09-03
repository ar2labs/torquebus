// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The bridge between a TraceStore filled at 150k frames/s and a QTableView
// repainting at 60.
//
// Three things make that possible, and none of them is optional:
//
//   1. The model never signals per frame. It polls the store on a timer and
//      emits one beginInsertRows for the whole batch that arrived since the
//      last tick. A dataChanged per frame would put the GUI thread in the
//      frame pipeline, which is exactly what rule #5 forbids.
//
//   2. data() is a lookup, not a computation. Delta, cycle time and occurrence
//      were computed once when the frame was stored; formatting is the only
//      work done per visible cell, and only ~40 cells are visible at a time.
//
//      Signal decoding is the one exception, and deliberately so. It is a pure
//      function of the frame, so unlike delta it needs no history - and doing
//      it per visible row costs about 8 us per repaint against decoding a
//      million stored frames that nobody will scroll to. Rule #2 exists to keep
//      work off the frame path, and decoding at paint time keeps *more* of it
//      off than the alternative.
//
//   3. Rows are never renumbered. When the ring wraps, the model resets rather
//      than trying to shift a million indices - and it only does so when it
//      actually wrapped, which on a bounded trace is rare and visible.

#pragma once

#include "core/database/CanMessage.h"
#include "core/trace/TraceStore.h"
#include "ui/theme/Theme.h"

#include <QAbstractTableModel>
#include <QString>

#include <memory>
#include <vector>
#include <QVariant>

#include <cstddef>

class QTimer;

namespace torquebus::ui {

class TraceModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    /// Columns, in the order PLAN.md section 15 lists them.
    enum Column : int {
        Time = 0,
        Delta,
        Channel,
        Direction,
        Identifier,
        Name,      ///< Message name, when a loaded database knows the identifier.
        Type,
        Dlc,
        Data,
        Signals,   ///< The decoded values, in the order the database lists them.
        Cycle,
        Count,
        Flags,
        ColumnCount
    };
    Q_ENUM(Column)

    explicit TraceModel(QObject* parent = nullptr);
    ~TraceModel() override;

    /// The store to display. Not owned; must outlive the model. Passing
    /// nullptr detaches, which is what happens when a measurement's graph is
    /// torn down.
    void setStore(const TraceStore* store);
    [[nodiscard]] const TraceStore* store() const noexcept { return m_store; }

    /// How often the view picks up new rows. 25 Hz: fast enough that the trace
    /// looks live, slow enough that a saturated bus does not turn into a
    /// repaint storm.
    void setRefreshIntervalMs(int milliseconds);

    /// Stops picking up new rows. The store keeps filling - freezing the view
    /// must not lose data - so resuming catches up rather than skipping.
    void setFrozen(bool frozen);
    [[nodiscard]] bool isFrozen() const noexcept { return m_frozen; }

    /// The databases used to name messages and decode the Signals column.
    ///
    /// Held by shared_ptr, and copied rather than referenced, because the
    /// panel that loaded them can be closed while a measurement is still
    /// running - and a decoded row pointing at a freed definition is the
    /// crash that would follow.
    ///
    /// Order matters: the first database that knows an identifier wins, which
    /// makes the order they were imported in the order they take priority.
    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

    /// Discards everything and starts from an empty view.
    void reset();

    // --- QAbstractTableModel ----------------------------------------------

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role) const override;

Q_SIGNALS:
    /// Emitted after rows were appended, so the view can decide whether to
    /// follow the tail. The view, not the model, owns that decision - a user
    /// who scrolled up is reading, and must not be yanked to the bottom.
    void rowsAppended(int firstRow, int lastRow);

private Q_SLOTS:
    void pollStore();

private:
    [[nodiscard]] QString textFor(const TraceRow& row, int column) const;

    /// The definition for a frame, or nullptr when no loaded database has one.
    [[nodiscard]] const CanMessage* definitionFor(const CanFrame& frame) const;

    /// "SpeedKmh = 85.0 km/h, EngTemp = 70 degC", or empty.
    [[nodiscard]] QString decodedText(const CanFrame& frame) const;
    [[nodiscard]] QVariant colourFor(const TraceRow& row, int column) const;

    const TraceStore* m_store{nullptr};
    QTimer* m_timer{nullptr};

    /// Rows the model has told the view about. Compared against the store's
    /// size on each tick to find what is new.
    std::size_t m_visibleRows{0};

    /// Discards seen last tick. A change means the ring wrapped and the row
    /// indices the view holds no longer mean what they did.
    std::uint64_t m_lastDiscarded{0};

    bool m_frozen{false};

    std::vector<std::shared_ptr<const CanDatabase>> m_databases;
};

} // namespace torquebus::ui
