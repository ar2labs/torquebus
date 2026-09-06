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
#include <functional>
#include <optional>
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

    /// Frames the channel's filter refused before the pipeline saw them.
    quint64 filteredFrames{};

    quint64 droppedFrames{};

    double framesPerSecond{};
    double busLoadPercent{};
    double peakBusLoadPercent{};

    quint32 bitrate{};

    [[nodiscard]] quint64 totalFrames() const { return rxFrames + txFrames; }
};

/// One counter a pipeline node reports, with the label the node chose.
///
/// The label travels with the value rather than being a column the UI knows
/// about. A node type the UI has never heard of still displays correctly, which
/// is the property that lets a script or a plugin add a counter without a
/// matching edit here.
struct NodeCounter final {
    QString label;
    quint64 value{};
};

/// One node of the running pipeline, ready to display.
struct NodeStatus final {
    QString name;
    QString typeName;
    QList<NodeCounter> counters;
};

class CanEngineController final : public QObject {
    Q_OBJECT

public:
    explicit CanEngineController(QObject* parent = nullptr);
    ~CanEngineController() override;

    /// The engine this controller drives. Owned by the controller.
    [[nodiscard]] CanEngine& engine() noexcept { return *m_engine; }

    /// Says how one detected interface should be opened, or that it should not
    /// be.
    ///
    /// A callback rather than a map, because the answer lives in the settings
    /// file and the controller has no business reading it: this class is the
    /// seam between the engine and Qt, not between the engine and the
    /// application's preferences. Returning nullopt leaves the interface
    /// unbound - it consumes no channel number and does not shift the ones
    /// after it.
    using ChannelPlan =
        std::function<std::optional<CanChannelConfig>(const CanDeviceInfo& device)>;

    /// Binds the interfaces `plan` accepts and returns how many were bound.
    /// Only valid while stopped; replaces any previous configuration.
    ///
    /// `order` is a list of handles saying which interface is CAN 1, CAN 2 and
    /// so on. Anything detected but not named in it is bound after those, in
    /// enumeration order; anything named but not detected is skipped. An empty
    /// order is enumeration order, which is what a machine nobody has arranged
    /// gets.
    std::size_t bindAvailableChannels(const ChannelPlan& plan,
                                      const QStringList& order = {});

    [[nodiscard]] bool isRunning() const;

    /// Human-readable summary of the bound channels, for the Output panel.
    [[nodiscard]] QStringList boundChannelDescriptions() const;

    /// Device names in channel order: index 0 is CAN 1.
    ///
    /// Needed now that channel order is a decision rather than enumeration
    /// order - the status bar cannot get CAN 1 by taking the first thing the
    /// registry found any more.
    [[nodiscard]] const QList<QString>& boundDeviceNames() const noexcept
    {
        return m_deviceNames;
    }

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

    /// The pipeline's own counters, on the same tick as statusUpdated().
    ///
    /// Emitted even when nothing is listening. The engine hands out a copy of a
    /// snapshot it already took, so the cost is a few dozen small string copies
    /// ten times a second - far below the price of the machinery that would be
    /// needed to decide whether to skip it.
    void nodeStatisticsUpdated(const QList<torquebus::ui::NodeStatus>& nodes);

    /// Total frames delivered since the measurement started.
    void frameCountChanged(quint64 total);

private Q_SLOTS:
    void publishToUi();

private:
    /// Converts the engine's node reports and emits nodeStatisticsUpdated().
    void publishNodeStatistics();

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
Q_DECLARE_METATYPE(torquebus::ui::NodeCounter)
Q_DECLARE_METATYPE(torquebus::ui::NodeStatus)
