// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Editing a transmit row by signal name instead of by hex.
//
// This is the last place the database layer pays off. A user who wants to send
// 85 km/h can type 85, and the definition decides that this means bytes 52 03
// at offset zero, little-endian, scaled by a tenth. Typing `52 03` by hand is
// the same operation performed by a person, and people get it wrong silently -
// a mis-packed frame transmits perfectly.
//
// Two things here are deliberate and easy to leave out:
//
// **The value column shows what was actually encoded**, read back out of the
// frame rather than echoed from the keyboard. Type 85.03 into a signal with a
// factor of 0.1 and the cell settles on 85. That is not the dialog being
// pedantic - it is the bus. A field that showed 85.03 while transmitting 85
// would be lying about the one thing the user came here to control.
//
// **Multiplexed signals follow the switch.** Change the selector and the rows
// that are not carried by that page grey out, because on that frame they are
// not zero, they are absent - the same distinction the decoder makes.

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/CanMessage.h"

#include <QDialog>

class QLabel;
class QTableWidget;
class QTableWidgetItem;

namespace torquebus::ui {

class SignalValueDialog final : public QDialog {
    Q_OBJECT

public:
    /// Edits `frame` in place against `message`.
    ///
    /// The frame is the dialog's working copy; the caller decides whether to
    /// keep it by looking at the dialog's result.
    SignalValueDialog(const CanMessage& message, CanFrame frame, QWidget* parent = nullptr);

    /// The frame as the signals now describe it.
    [[nodiscard]] const CanFrame& frame() const noexcept { return m_frame; }

private Q_SLOTS:
    void onItemChanged(QTableWidgetItem* item);

private:
    void buildUi();

    /// Fills every row from the frame as it currently stands.
    void reload();

    const CanMessage& m_message;
    CanFrame m_frame;

    QTableWidget* m_table{nullptr};
    QLabel* m_payload{nullptr};

    /// True while the dialog writes into its own table. Every setText emits
    /// itemChanged, and without this the read-back after an encode would be
    /// taken for a fresh edit and encoded again.
    bool m_populating{false};
};

} // namespace torquebus::ui
