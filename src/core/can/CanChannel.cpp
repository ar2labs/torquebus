// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanChannel.h"

#include <format>
#include <utility>

namespace torquebus {

CanChannel::CanChannel(std::uint8_t applicationChannel,
                       std::unique_ptr<ICanBackend> backend,
                       CanChannelConfig config)
    : m_applicationChannel{applicationChannel}
    , m_backend{std::move(backend)}
    , m_config{std::move(config)}
{
    m_config.applicationChannel = applicationChannel;
    m_statistics.setBitrate(m_config.timing.bitrate);
}

CanChannel::~CanChannel()
{
    CanChannel::close();
}

std::string CanChannel::displayName() const
{
    return std::format("CAN {}", m_applicationChannel + 1);
}

CanCapabilities CanChannel::capabilities() const
{
    return m_backend ? m_backend->capabilities() : CanCapabilities{};
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result CanChannel::open()
{
    if (!m_backend) {
        return Result::error(ErrorCode::BackendUnavailable,
                             std::format("{} has no backend bound", displayName()));
    }

    if (m_open) {
        return Result::ok();
    }

    // The handler is installed before open() so that a backend which starts
    // delivering the moment it is opened cannot drop the first frames.
    m_backend->setFrameHandler([this](std::span<const CanFrame> frames) noexcept {
        onFramesReceived(frames);
    });

    m_backend->setStatusHandler([this](const CanBusStatus& status) noexcept {
        onStatusChanged(status);
    });

    if (Result result = m_backend->open(m_config); result.failed()) {
        return result;
    }

    m_open = true;
    m_statistics.setBitrate(m_config.timing.bitrate);
    return Result::ok();
}

Result CanChannel::start()
{
    if (!m_open) {
        if (Result result = open(); result.failed()) {
            return result;
        }
    }

    if (m_running) {
        return Result::ok();
    }

    // A measurement starts from zero: stale frames and stale counters from a
    // previous run must never bleed into this one.
    m_queue.clear();
    m_queue.resetOverflows();
    m_statistics.reset();
    m_statistics.setBitrate(m_config.timing.bitrate);
    m_hardwareOverruns.store(0, std::memory_order_relaxed);

    if (Result result = m_backend->start(); result.failed()) {
        return result;
    }

    m_running = true;
    m_state.store(CanBusState::ErrorActive, std::memory_order_relaxed);
    return Result::ok();
}

void CanChannel::stop()
{
    if (!m_running) {
        return;
    }

    m_running = false;

    if (m_backend) {
        m_backend->stop();
    }

    m_state.store(CanBusState::Offline, std::memory_order_relaxed);
}

void CanChannel::close()
{
    stop();

    if (m_backend && m_open) {
        m_backend->close();
    }

    m_open = false;
}

bool CanChannel::isOpen() const noexcept
{
    return m_open;
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

void CanChannel::onFramesReceived(std::span<const CanFrame> frames) noexcept
{
    // Runs on the backend's receive thread. Everything expensive - filtering,
    // statistics, decoding, display - happens later, on the engine thread.
    // All this does is hand the batch over and return (rule #6).
    m_queue.pushBatch(frames);
}

void CanChannel::onStatusChanged(const CanBusStatus& status) noexcept
{
    m_state.store(status.state, std::memory_order_relaxed);
    m_hardwareOverruns.store(status.hardwareOverruns, std::memory_order_relaxed);
}

Result CanChannel::transmit(CanFrame frame)
{
    if (!m_backend) {
        return Result::error(ErrorCode::BackendUnavailable);
    }

    if (!m_running) {
        return Result::error(ErrorCode::ChannelNotOpen,
                             std::format("{} is not running", displayName()));
    }

    // Callers say "send this on CAN 1"; they should not have to remember to
    // stamp the channel themselves.
    frame.channel = m_applicationChannel;
    frame.direction = CanDirection::Tx;

    if (frame.length == 0 && frame.dlc > 0) {
        frame.length = payloadLengthFromDlc(frame.dlc, frame.fd);
    } else if (frame.dlc == 0 && frame.length > 0) {
        frame.dlc = dlcFromPayloadLength(frame.length, frame.fd);
    }

    return m_backend->transmit(frame);
}

std::size_t CanChannel::drain(std::vector<CanFrame>& out, std::size_t maximum)
{
    const std::size_t read = m_queue.drainInto(out, maximum);
    if (read == 0) {
        return 0;
    }

    const std::size_t kept = m_filters.retainAccepted(std::span<CanFrame>{out.data(), read});

    if (kept < read) {
        m_statistics.recordFiltered(read - kept);
        out.resize(kept);
    }

    for (std::size_t index = 0; index < kept; ++index) {
        m_statistics.recordFrame(out[index]);
    }

    return kept;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

void CanChannel::closeStatisticsWindow(std::uint64_t elapsedNs)
{
    m_statistics.setState(m_state.load(std::memory_order_relaxed));
    m_statistics.setDropped(m_queue.overflows());
    m_statistics.closeWindow(elapsedNs);
}

CanBusStatus CanChannel::status() const
{
    CanBusStatus result = m_backend ? m_backend->status() : CanBusStatus{};

    result.state = m_state.load(std::memory_order_relaxed);
    result.hardwareOverruns = m_hardwareOverruns.load(std::memory_order_relaxed);

    // The driver knows nothing about our queue, so our own overflow count is
    // merged in here. This is the number that says "TorqueBus could not keep
    // up", as opposed to "the adapter could not keep up".
    result.softwareOverruns = m_queue.overflows();

    return result;
}

} // namespace torquebus
