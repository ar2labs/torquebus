// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Hardware Configuration: which interfaces become CAN 1..N, and how.
//
// One table, one row per detected interface, in channel order. The first column
// is the channel number the row will be, and it moves as rows move - that is
// the whole point of the dialog, and showing it any other way would leave
// somebody counting rows to work out what CAN 2 is.
//
// The rows, not the widgets, are what the dialog knows. Every control writes
// its change straight back into the row it belongs to, so reordering can
// rebuild the table from scratch without losing a rate somebody chose thirty
// seconds ago - and so that accepting reads one list rather than interrogating
// a grid of widgets.
//
// Nothing is applied while a measurement is running: channels are opened at
// Start and the mapping is what a trace's channel column means, so changing it
// underneath a running measurement would rewrite the meaning of rows already on
// screen. The dialog says so rather than being unavailable, because "why is
// this greyed out" is a worse question than an answered one.
//
// Applied on OK, not live. This is the one dialog where a change costs a
// rebind of every interface, and half-applying that while somebody is still
// deciding is how an adapter ends up open at the wrong rate.

#pragma once

#include "core/can/CanTypes.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

class QLabel;
class QPushButton;
class QTableWidget;

namespace torquebus::services {
class HardwareProfile;
}

namespace torquebus::ui {

class HardwareDialog final : public QDialog {
    Q_OBJECT

public:
    /// `devices` is what the registry found; `profile` is what the user has
    /// said about them before. Neither is owned.
    HardwareDialog(const CanDeviceInfoList& devices,
                   services::HardwareProfile& profile,
                   bool measurementRunning,
                   QWidget* parent = nullptr);

    /// True when OK was pressed and something actually changed, so the caller
    /// knows whether a rebind is needed.
    [[nodiscard]] bool profileChanged() const noexcept { return m_changed; }

private Q_SLOTS:
    void onMoveUp();
    void onMoveDown();
    void onSelectionChanged();
    void onAccepted();

private:
    /// One interface, as the dialog currently has it. The source of truth: the
    /// widgets are a view onto these, and every control writes back here.
    struct Row final {
        QString handle;
        QString name;
        QString backend;

        bool enabled{true};
        std::uint32_t bitrate{kDefaultBitrate};
        bool canFd{false};
        bool listenOnly{false};

        /// What the adapter can actually do. A tick nothing acts on is a
        /// promise the driver breaks at Start, so these disable the controls.
        bool supportsFd{false};
        bool supportsListenOnly{false};
    };

    void buildUi(bool measurementRunning);

    /// Rebuilds every row's widgets from m_rows. Cheap - there are as many rows
    /// as the machine has CAN interfaces - and it is what makes reordering a
    /// swap in a vector rather than a shuffle of live widgets.
    void fillTable();

    /// Rewrites the channel-number column. Called when a row moves and when an
    /// interface is switched on or off, without touching anything else.
    void renumber();

    void moveSelected(int delta);

    services::HardwareProfile& m_profile;

    QVector<Row> m_rows;

    QTableWidget* m_table{nullptr};
    QPushButton* m_up{nullptr};
    QPushButton* m_down{nullptr};
    QLabel* m_hint{nullptr};

    bool m_readOnly{false};
    bool m_changed{false};
};

} // namespace torquebus::ui
