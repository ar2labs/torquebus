// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/log/LogNodes.h"

#include <algorithm>
#include <utility>

namespace torquebus {
namespace {

/// How many frames the replay reads ahead. Enough that a pass never waits on
/// the disk at any speed a person would choose, small enough that seeking to a
/// different point in the file later will not have to throw much away.
constexpr std::size_t kReadAhead = 4096;

} // namespace

// ---------------------------------------------------------------------------
// LogSinkNode
// ---------------------------------------------------------------------------

void LogSinkNode::process(NodeContext& context)
{
    const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
    if (incoming.empty()) {
        return;
    }

    if (m_writer.append(incoming).failed()) {
        // Counted rather than thrown. A disk that filled up mid-measurement
        // must not take the measurement down with it - the frames still reach
        // the trace, the plot and the bus, and the Statistics panel is where
        // the number showing what the log lost is read.
        m_refused += incoming.size();
        return;
    }

    m_recorded += incoming.size();
}

// ---------------------------------------------------------------------------
// LogSourceNode
// ---------------------------------------------------------------------------

LogSourceNode::LogSourceNode(std::unique_ptr<TraceLogReader> reader,
                             double speed,
                             std::string label)
    : m_reader{std::move(reader)}
    , m_label{std::move(label)}
    // A speed of zero or less would mean a replay that never advances, which is
    // a paused measurement rather than a configured one. Clamped to something
    // that plays.
    , m_speed{speed > 0.0 ? speed : 1.0}
{
}

Result LogSourceNode::prepare(std::size_t maximumBatchSize)
{
    m_started = std::chrono::steady_clock::now();

    m_pending.clear();
    m_pendingFirst = 0;
    m_replayed = 0;
    m_finished = false;
    m_haveEpoch = false;
    m_epochNs = 0;

    // Sized for the worst case, like every other source: a pass that had to
    // grow this would reallocate the storage the published span points into.
    m_outgoing.clear();
    m_outgoing.reserve(std::max<std::size_t>(maximumBatchSize, 1));

    return Result::ok();
}

void LogSourceNode::process(NodeContext& context)
{
    m_outgoing.clear();

    if (m_reader == nullptr || !m_reader->isOpen()) {
        return;
    }

    // Top up the read-ahead when it runs low, so the disk is touched between
    // passes rather than in the middle of one.
    if (!m_finished && m_pending.size() - m_pendingFirst < kReadAhead / 2) {
        // Everything already emitted is dropped first, so the buffer does not
        // grow for the length of the file.
        if (m_pendingFirst > 0) {
            m_pending.erase(m_pending.begin(),
                            m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingFirst));
            m_pendingFirst = 0;
        }

        const std::size_t offset = m_pending.size();
        m_pending.resize(offset + kReadAhead);

        const std::size_t got =
            m_reader->read(std::span{m_pending}.subspan(offset, kReadAhead));

        m_pending.resize(offset + got);

        if (got < kReadAhead) {
            m_finished = m_reader->atEnd();
        }
    }

    if (m_pendingFirst >= m_pending.size()) {
        return;
    }

    if (!m_haveEpoch) {
        // The first frame's own timestamp is time zero for the replay. Without
        // this, a recording whose first frame is at 12.5 s would sit silent for
        // twelve and a half seconds before anything appeared.
        m_epochNs = m_pending[m_pendingFirst].timestampNs;
        m_haveEpoch = true;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - m_started)
                             .count();

    // How far into the recording the replay has reached. Multiplying elapsed
    // time by the speed - rather than dividing each frame's timestamp - keeps
    // the comparison in integers on the frame path.
    const auto reached = static_cast<std::uint64_t>(static_cast<double>(elapsed) * m_speed);

    const std::size_t room = m_outgoing.capacity();

    while (m_pendingFirst < m_pending.size() && m_outgoing.size() < room) {
        const CanFrame& frame = m_pending[m_pendingFirst];

        const std::uint64_t offsetNs =
            frame.timestampNs > m_epochNs ? frame.timestampNs - m_epochNs : 0;

        if (offsetNs > reached) {
            // Its moment has not come round yet. Everything after it is later
            // still, so the pass is done.
            break;
        }

        m_outgoing.push_back(frame);
        ++m_pendingFirst;
        ++m_replayed;
    }

    if (!m_outgoing.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoing});
    }
}

} // namespace torquebus
