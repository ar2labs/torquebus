// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The one place where the CAN engine meets Qt.
//
// The engine is Qt-free and calls its sinks on its own dispatch thread. Qt
// widgets may only be touched from the GUI thread. This adapter is the seam
// between those two facts, and it is deliberately the *only* such seam: no
// panel ever registers a sink with the engine directly.
//
// How it bridges, and why:
//
//   - Statistics arrive at ~10 Hz and are small. They are copied and posted
//     to the GUI thread with a queued signal. Cheap, exact, no loss.
//
//   - Frames arrive at up to 100k/s. They are NOT signalled. Emitting a Qt
//     signal per frame - or even per batch at that rate - would defeat the
//     entire pipeline design (rule #5). Instead the controller accumulates a
//     count and lets the GUI thread pull on its own timer. The frames
//     themselves stay in the core, where the Trace Store will pick them up
//     from v0.4 onwards.
//
// The controller therefore exposes what the shell needs today - counters,
// rates, bus state - without opening a per-frame path into the UI that would
// have to be closed again later.

#pragma once

#include "core/Result.h"
#include "core/can/CanEngine.h"
#include "core/can/CanStatistics.h"
#include "core/can/CanTypes.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class QTimer;

namespace torquebus::ui {

/// One channel's live figures, in a form a widget can display directly.
struct ChannelStatus final {
    QString name;        ///< "CAN 1"
    QString deviceName;  ///< "TorqueBus Virtual CAN 0"
    QString stateText;   ///< "Online", "Bus off", ...

    /// Style-sheet state token: offline / ready / online / warning / error.
    QString stateToken;

    quint64 rxFrames{};
    quint64 txFrames{};
    quint64 errorFrames{};
    quint64 droppedFrames{};

    double framesPerSecond{};
    double busLoadPercent{};
    double peakBusLoadPercent{};

    quint32 bitrate{};

    [[nodiscard]] quint64 totalFrames() const { return rxFrames + txFrames; }
};

class CanEngineController final : public QObject {
    Q_OBJECT

public:
    explicit CanEngineController(QObject* parent = nullptr);
    ~CanEngineController() override;

    /// The engine this controller drives. Owned by the controller.
    [[nodiscard]] CanEngine& engine() noexcept { return *m_engine; }

    /// Binds every available channel reported by the backend registry, in
    /// enumeration order, and returns how many were bound. Only valid while
    /// stopped; replaces any previous configuration.
    std::size_t bindAvailableChannels(quint32 bitrate = 500'000);

    [[nodiscard]] bool isRunning() const;

    /// Human-readable summary of the bound channels, for the Output panel.
    [[nodiscard]] QStringList boundChannelDescriptions() const;

public Q_SLOTS:
    /// Starts the measurement. Emits started() or failed().
    void start();

    /// Stops the measurement. Emits stopped().
    void stop();

Q_SIGNALS:
    void started();
    void stopped();

    /// Something went wrong that the user must see. `message` is already
    /// user-facing - it names the channel and the reason.
    void failed(const QString& message);

    /// Emitted on the GUI thread at the refresh rate, never per frame.
    void statusUpdated(const QList<torquebus::ui::ChannelStatus>& channels);

    /// Total frames delivered since the measurement started.
    void frameCountChanged(quint64 total);

private Q_SLOTS:
    void publishToUi();

private:
    std::unique_ptr<CanEngine> m_engine;

    /// Written by the engine thread, read by the GUI thread.
    mutable std::mutex m_snapshotMutex;
    std::vector<CanStatisticsSnapshot> m_snapshot;

    /// Device names, indexed by application channel. Fixed while running.
    QList<QString> m_deviceNames;

    SinkId m_statisticsSink{0};

    QTimer* m_refreshTimer{nullptr};
    quint64 m_lastPublishedFrameCount{0};
};

} // namespace torquebus::ui

Q_DECLARE_METATYPE(torquebus::ui::ChannelStatus)
