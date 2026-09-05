// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Transmit panel: the first place in this window where a person, rather
// than a driver or a script, puts a frame on a bus.
//
// That shapes two things about it.
//
// **Nothing sends until asked.** A new row is Manual, and Manual means the
// frame goes out when the Send button is pressed and at no other time. A
// transmit list is filled in with the cable already connected to something
// real; a panel that started transmitting as rows were typed would put traffic
// on a vehicle nobody had prepared.
//
// **The list is not the panel's.** TransmitList belongs to the project and is
// read by the executor thread while this is being edited - which is the whole
// point, since changing a byte and watching what happens is what a transmit
// list is for. Every edit here goes through TransmitList's own API, which is
// where the locking lives; this class holds no frame data of its own.

#pragma once

#include "core/database/CanMessage.h"
#include "core/transmit/TransmitEntry.h"

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QLabel;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QToolBar;

namespace torquebus {
class TransmitList;
}

namespace torquebus::ui {

class TransmitPanel final : public QWidget {
    Q_OBJECT

public:
    explicit TransmitPanel(TransmitList& list, QWidget* parent = nullptr);

    /// Databases offered by "Add from message". Same list the trace decodes
    /// with, handed over by the window when one is imported.
    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

Q_SIGNALS:
    /// Something the Output panel should say - a frame refused by a full queue,
    /// a row that could not be parsed. Not a message box: the user is mid-edit
    /// and a modal would take the keyboard away from them.
    void reported(const QString& message);

private Q_SLOTS:
    void onAdd();
    void onAddFromMessage();
    void onRemove();
    void onSendSelected();
    void onEditSignals();
    void onItemChanged(QTableWidgetItem* item);

    /// Repaints only the columns the executor owns.
    void refreshCounters();

    void onThemeChanged();

private:
    void buildUi();

    /// Rebuilds every row from the list. Used after add, remove and load.
    void reload();

    /// Writes row `row` back into the list, parsing what the user typed.
    ///
    /// A field that does not parse is put back to what the list still holds
    /// rather than left as typed. Silently keeping an unparseable identifier in
    /// a cell that looks committed is how somebody ends up believing they are
    /// transmitting on an address they are not.
    void commitRow(int row);

    /// Enables the actions that only apply to a particular kind of row.
    void updateActionState();

    /// The row currently selected, or -1.
    [[nodiscard]] int selectedRow() const;

    /// The definition a row was built from, or nullptr.
    ///
    /// Searched by name across every loaded database, first match wins - the
    /// same rule the trace uses to decode. Two databases defining the same
    /// message name is a real situation and there is no better answer available
    /// than "the one imported first"; the entry records a name, not a file.
    [[nodiscard]] const CanMessage* messageFor(const TransmitEntry& entry) const;

    TransmitList& m_list;
    std::vector<std::shared_ptr<const CanDatabase>> m_databases;

    QTableWidget* m_table{nullptr};
    QToolBar* m_toolBar{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_timer{nullptr};

    QAction* m_actionAdd{nullptr};
    QAction* m_actionAddFromMessage{nullptr};
    QAction* m_actionRemove{nullptr};
    QAction* m_actionSend{nullptr};
    QAction* m_actionEditSignals{nullptr};

    /// True while the panel is writing into the table itself.
    ///
    /// Every setItem and setText emits itemChanged, so without this the
    /// refresh that writes the send counter would be read back as a user edit
    /// and committed to the list - a feedback loop that would overwrite a row
    /// twenty times a second with its own display text.
    bool m_populating{false};
};

} // namespace torquebus::ui
