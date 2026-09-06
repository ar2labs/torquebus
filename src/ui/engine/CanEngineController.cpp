// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/engine/CanEngineController.h"

#include "drivers/api/CanBackendRegistry.h"

#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <vector>
#include <utility>

namespace torquebus::ui {
namespace {

/// How often the GUI thread refreshes the status bar. Fast enough to feel
/// live, slow enough that it costs nothing next to the frame pipeline.
constexpr int kRefreshIntervalMs = 100;

[[nodiscard]] QString stateText(CanBusState state)
{
    switch (state) {
    case CanBusState::Offline:      return CanEngineController::tr("Offline");
    case CanBusState::ErrorActive:  return CanEngineController::tr("Online");
    case CanBusState::ErrorWarning: return CanEngineController::tr("Warning");
    case CanBusState::ErrorPassive: return CanEngineController::tr("Error passive");
    case CanBusState::BusOff:       return CanEngineController::tr("Bus off");
    }
    return CanEngineController::tr("Unknown");
}

/// Maps controller state onto the torquebusState style-sheet property, so the
/// theme decides the colour rather than this file.
[[nodiscard]] QString stateToken(CanBusState state, bool running)
{
    switch (state) {
    case CanBusState::Offline:      return running ? QStringLiteral("warning")
                                                   : QStringLiteral("offline");
    case CanBusState::ErrorActive:  return QStringLiteral("online");
    case CanBusState::ErrorWarning: return QStringLiteral("warning");
    case CanBusState::ErrorPassive: return QStringLiteral("warning");
    case CanBusState::BusOff:       return QStringLiteral("error");
    }
    return QStringLiteral("offline");
}

} // namespace

CanEngineController::CanEngineController(QObject* parent)
    : QObject{parent}
    , m_engine{std::make_unique<CanEngine>()}
{
    qRegisterMetaType<torquebus::ui::ChannelStatus>();
    qRegisterMetaType<QList<torquebus::ui::ChannelStatus>>("QList<torquebus::ui::ChannelStatus>");
    qRegisterMetaType<torquebus::ui::NodeStatus>();
    qRegisterMetaType<QList<torquebus::ui::NodeStatus>>("QList<torquebus::ui::NodeStatus>");

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshIntervalMs);
    m_refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(m_refreshTimer, &QTimer::timeout, this, &CanEngineController::publishToUi);

    // The statistics sink runs on the engine thread. It does the least possible
    // work: copy the snapshot under a short lock and return. The GUI thread
    // picks it up on its own timer, so a stalled UI can never back-pressure
    // the engine.
    m_statisticsSink = m_engine->addStatisticsSink(
        [this](std::span<const CanStatisticsSnapshot> snapshot) {
            const std::lock_guard lock{m_snapshotMutex};
            m_snapshot.assign(snapshot.begin(), snapshot.end());
        });
}

CanEngineController::~CanEngineController()
{
    // The engine must stop before the sink's captured `this` goes away.
    if (m_engine) {
        m_engine->removeStatisticsSink(m_statisticsSink);
        m_engine->stop();
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

std::size_t CanEngineController::bindAvailableChannels(const ChannelPlan& plan,
                                                       const QStringList& order)
{
    if (isRunning()) {
        return m_engine->channelCount();
    }

    m_engine->clearChannels();
    m_deviceNames.clear();

    CanBackendRegistry& registry = CanBackendRegistry::instance();
    registry.registerBuiltins();

    const CanDeviceInfoList detected = registry.enumerateAll();

    // Sorted into the order the caller asked for. Channel numbers are what a
    // trace column, a transmit row and a saved project all mean, so which
    // interface is CAN 1 must be somebody's decision rather than a property of
    // which driver answered first this morning.
    std::vector<const CanDeviceInfo*> arranged;
    arranged.reserve(detected.size());

    for (const QString& handle : order) {
        for (const CanDeviceInfo& device : detected) {
            if (QString::fromStdString(device.handle) == handle) {
                arranged.push_back(&device);
                break;
            }
        }
    }

    for (const CanDeviceInfo& device : detected) {
        const auto already = std::find(arranged.begin(), arranged.end(), &device);
        if (already == arranged.end()) {
            arranged.push_back(&device);
        }
    }

    for (const CanDeviceInfo* device : arranged) {
        std::optional<CanChannelConfig> config =
            plan ? plan(*device) : std::optional<CanChannelConfig>{CanChannelConfig{}};

        if (!config.has_value()) {
            // Switched off. Not opened, and - because addChannel is what hands
            // out channel numbers - not occupying one either.
            continue;
        }

        std::unique_ptr<ICanBackend> backend = registry.create(device->backend);
        if (!backend) {
            continue;
        }

        // The handle is the controller's to fill in, not the caller's: it comes
        // from the device that was actually enumerated, so a plan that returned
        // a config for the wrong interface cannot open the wrong adapter.
        config->deviceHandle = device->handle;

        if (m_engine->addChannel(std::move(backend), std::move(*config)).succeeded()) {
            m_deviceNames.append(QString::fromStdString(device->name));
        }
    }

    // The refresh timer only runs while measuring, so without this the channels
    // that were just bound would not appear anywhere until Start was pressed -
    // and the Statistics panel would say there were none.
    publishToUi();

    return m_engine->channelCount();
}

bool CanEngineController::isRunning() const
{
    return m_engine && m_engine->isRunning();
}

QStringList CanEngineController::boundChannelDescriptions() const
{
    QStringList descriptions;

    for (std::size_t index = 0; index < m_engine->channelCount(); ++index) {
        const CanChannel* channel = m_engine->channel(static_cast<std::uint8_t>(index));
        if (channel == nullptr) {
            continue;
        }

        const QString device = index < static_cast<std::size_t>(m_deviceNames.size())
            ? m_deviceNames.at(static_cast<qsizetype>(index))
            : tr("unknown interface");

        descriptions.append(tr("%1 -> %2 at %3 kbit/s")
                                .arg(QString::fromStdString(channel->displayName()),
                                     device,
                                     QString::number(channel->config().timing.bitrate / 1000)));
    }

    return descriptions;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

void CanEngineController::start()
{
    if (isRunning()) {
        return;
    }

    if (m_engine->channelCount() == 0) {
        Q_EMIT failed(tr("No CAN channels are configured. Connect an interface, or use "
                         "Hardware > Refresh Interfaces."));
        return;
    }

    if (const Result result = m_engine->start(); result.failed()) {
        Q_EMIT failed(QString::fromStdString(std::string{result.message()}));
        return;
    }

    m_lastPublishedFrameCount = 0;

    {
        const std::lock_guard lock{m_snapshotMutex};
        m_snapshot.clear();
    }

    m_refreshTimer->start();
    Q_EMIT started();

    publishToUi();
}

void CanEngineController::stop()
{
    if (!isRunning()) {
        return;
    }

    m_engine->stop();
    m_refreshTimer->stop();

    // One last publish so the final counts are what the user is left looking
    // at, rather than whatever the last timer tick happened to catch.
    publishToUi();

    Q_EMIT stopped();
}

// ---------------------------------------------------------------------------
// Publishing to the UI
// ---------------------------------------------------------------------------

void CanEngineController::publishToUi()
{
    std::vector<CanStatisticsSnapshot> snapshot;

    {
        const std::lock_guard lock{m_snapshotMutex};
        snapshot = m_snapshot;
    }

    // Before the first statistics window closes there is nothing to copy, but
    // the channels already exist - show them as configured rather than as
    // missing.
    if (snapshot.empty()) {
        snapshot = m_engine->statistics();
    }

    const bool running = isRunning();

    QList<ChannelStatus> channels;
    channels.reserve(static_cast<qsizetype>(snapshot.size()));

    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        const CanStatisticsSnapshot& source = snapshot[index];

        ChannelStatus status;
        status.name = tr("CAN %1").arg(index + 1);
        status.deviceName = index < static_cast<std::size_t>(m_deviceNames.size())
            ? m_deviceNames.at(static_cast<qsizetype>(index))
            : QString{};

        status.stateText = running ? stateText(source.state) : tr("Ready");
        status.stateToken = running ? stateToken(source.state, running)
                                    : QStringLiteral("ready");

        status.rxFrames = source.rxFrames;
        status.txFrames = source.txFrames;
        status.errorFrames = source.errorFrames;
        status.filteredFrames = source.filteredFrames;
        status.droppedFrames = source.droppedFrames;
        status.framesPerSecond = source.framesPerSecond;
        status.busLoadPercent = source.busLoadPercent;
        status.peakBusLoadPercent = source.peakBusLoadPercent;
        status.bitrate = source.bitrate;

        channels.append(std::move(status));
    }

    Q_EMIT statusUpdated(channels);
    publishNodeStatistics();

    const quint64 total = m_engine->deliveredFrames();
    if (total != m_lastPublishedFrameCount) {
        m_lastPublishedFrameCount = total;
        Q_EMIT frameCountChanged(total);
    }
}

void CanEngineController::publishNodeStatistics()
{
    QList<NodeStatus> nodes;

    for (const CanEngine::NodeReport& report : m_engine->nodeStatistics()) {
        NodeStatus status;
        status.name = QString::fromStdString(report.name);
        status.typeName = QString::fromStdString(report.typeName);

        status.counters.reserve(static_cast<qsizetype>(report.statistics.size()));
        for (const NodeStatistic& statistic : report.statistics) {
            status.counters.append(
                NodeCounter{QString::fromStdString(statistic.label), statistic.value});
        }

        nodes.append(std::move(status));
    }

    Q_EMIT nodeStatisticsUpdated(nodes);
}

} // namespace torquebus::ui
