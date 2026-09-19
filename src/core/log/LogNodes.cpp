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

/// The most a single pass will advance the replay clock.
///
/// A pass that took a second - a breakpoint, a machine that went to sleep, a
/// disk that stalled - would otherwise make a second of the recording due at
/// once and dump it downstream in one batch. Playback resumes from where it
/// was instead. The replay is then behind the wall clock, which is the honest
/// outcome: a player that was frozen for a second is a second late, it did not
/// travel a second into the recording.
constexpr std::uint64_t kMaximumTickNs = 250'000'000;

/// Frames a seek will pass over in one pass through the graph.
///
/// The format has no index, so seeking is reading. Bounding it per pass is what
/// keeps a seek across a four-gigabyte log from stalling every other node in
/// the graph - it just takes several passes, and the timeline says so by
/// arriving where it was asked to a moment later.
constexpr std::size_t kSeekBudget = 64 * 1024;

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
                             std::string label,
                             ReplayControl* control)
    : m_reader{std::move(reader)}
    , m_label{std::move(label)} // A speed of zero or less would mean a replay that never advances,
                                // which is a paused measurement rather than a configured one.
                                // Clamped to something that plays.
    , m_speed{speed > 0.0 ? speed : 1.0}
    , m_control{control}
{
    // The block's own Speed parameter is the starting position of the panel's
    // control, not a rival to it: a project that says 0.5x should open playing
    // at 0.5x, and the moment somebody touches the transport the panel owns it.
    if (m_control != nullptr) {
        m_control->setSpeed(m_speed);
    }
}

Result LogSourceNode::prepare(std::size_t maximumBatchSize)
{
    m_lastTick = std::chrono::steady_clock::now();
    m_positionNs = 0;

    m_seekPending = false;
    m_seekTargetNs = 0;
    m_skipped = 0;

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

    if (m_control != nullptr) {
        m_control->publishPosition(0);
        m_control->publishFinished(false);

        // Says the panel has something to drive. Set here rather than at
        // construction because a node that was built but whose graph never
        // compiled is not playing anything.
        m_control->setActive(m_reader != nullptr && m_reader->isOpen());
    }

    return Result::ok();
}

void LogSourceNode::topUp()
{
    // Everything already emitted is dropped first, so the buffer does not grow
    // for the length of the file.
    if (m_pendingFirst > 0) {
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingFirst));
        m_pendingFirst = 0;
    }

    const std::size_t offset = m_pending.size();
    m_pending.resize(offset + kReadAhead);

    const std::size_t got = m_reader->read(std::span{m_pending}.subspan(offset, kReadAhead));

    m_pending.resize(offset + got);

    if (got < kReadAhead) {
        m_finished = m_reader->atEnd();
    }
}

void LogSourceNode::serviceSeek()
{
    // Backwards means starting over. There is no index to jump with - see
    // TraceLogReader::restart() for why that is a property of the format rather
    // than an omission - so going back ten seconds is reading the file again
    // from the top, quickly, and stopping at the right frame.
    if (m_seekTargetNs < m_positionNs) {
        if (m_reader->restart().failed()) {
            // Nothing sensible left to do: the file will not rewind, so the
            // replay stays where it is rather than pretending to have moved.
            m_seekPending = false;
            return;
        }

        m_pending.clear();
        m_pendingFirst = 0;
        m_finished = false;
        m_positionNs = 0;
    }

    std::size_t budget = kSeekBudget;

    while (budget > 0) {
        if (m_pendingFirst >= m_pending.size()) {
            if (m_finished) {
                // Asked to seek past the end. The replay lands on the end,
                // which is where the timeline handle will be too.
                break;
            }

            topUp();

            if (m_pendingFirst >= m_pending.size()) {
                break;
            }
        }

        const CanFrame& frame = m_pending[m_pendingFirst];

        if (!m_haveEpoch) {
            m_epochNs = frame.timestampNs;
            m_haveEpoch = true;
        }

        const std::uint64_t offsetNs =
            frame.timestampNs > m_epochNs ? frame.timestampNs - m_epochNs : 0;

        if (offsetNs >= m_seekTargetNs) {
            // Found the first frame at or after the target: the seek is done,
            // and this frame is the next one to play.
            m_positionNs = m_seekTargetNs;
            m_seekPending = false;
            return;
        }

        ++m_pendingFirst;
        ++m_skipped;
        --budget;
    }

    // Either the budget ran out or the file did. Position follows what has
    // actually been passed over, so a long seek moves the handle as it goes
    // rather than jumping when it finishes.
    if (m_pendingFirst < m_pending.size()) {
        const CanFrame& frame = m_pending[m_pendingFirst];
        m_positionNs = frame.timestampNs > m_epochNs ? frame.timestampNs - m_epochNs : 0;
        return;
    }

    if (m_finished) {
        m_positionNs = m_seekTargetNs;
        m_seekPending = false;
    }
}

void LogSourceNode::process(NodeContext& context)
{
    m_outgoing.clear();

    if (m_reader == nullptr || !m_reader->isOpen()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    auto sinceLastTick = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastTick).count());

    m_lastTick = now;
    sinceLastTick = std::min(sinceLastTick, kMaximumTickNs);

    bool paused = false;

    if (m_control != nullptr) {
        m_speed = m_control->speed();
        paused = m_control->isPaused();

        std::uint64_t target = 0;
        if (m_control->takeSeekRequest(target)) {
            m_seekPending = true;
            m_seekTargetNs = target;
        }
    }

    if (m_seekPending) {
        // Seeking is not playing: nothing is emitted while the file is being
        // wound to a new place, however many passes that takes.
        serviceSeek();

        if (m_control != nullptr) {
            m_control->publishPosition(m_positionNs);
        }
        return;
    }

    if (!m_finished && m_pending.size() - m_pendingFirst < kReadAhead / 2) {
        // Topped up when it runs low, so the disk is touched between passes
        // rather than in the middle of one.
        topUp();
    }

    if (m_pendingFirst >= m_pending.size()) {
        if (m_control != nullptr) {
            m_control->publishFinished(m_finished);
            m_control->publishPosition(m_positionNs);
        }
        return;
    }

    if (!m_haveEpoch) {
        // The first frame's own timestamp is time zero for the replay. Without
        // this, a recording whose first frame is at 12.5 s would sit silent for
        // twelve and a half seconds before anything appeared.
        m_epochNs = m_pending[m_pendingFirst].timestampNs;
        m_haveEpoch = true;
    }

    if (!paused) {
        // Integrated rather than derived from a start time: time spent paused,
        // or spent at half speed, has to not count, and no start time can say
        // that after the fact.
        m_positionNs += static_cast<std::uint64_t>(static_cast<double>(sinceLastTick) * m_speed);
    }

    const std::size_t room = m_outgoing.capacity();

    while (m_pendingFirst < m_pending.size() && m_outgoing.size() < room) {
        const CanFrame& frame = m_pending[m_pendingFirst];

        const std::uint64_t offsetNs =
            frame.timestampNs > m_epochNs ? frame.timestampNs - m_epochNs : 0;

        if (offsetNs > m_positionNs) {
            // Its moment has not come round yet. Everything after it is later
            // still, so the pass is done.
            break;
        }

        m_outgoing.push_back(frame);
        ++m_pendingFirst;
        ++m_replayed;
    }

    if (m_control != nullptr) {
        m_control->publishPosition(m_positionNs);
        m_control->publishFinished(m_finished && m_pendingFirst >= m_pending.size());
    }

    if (!m_outgoing.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoing});
    }
}

} // namespace torquebus
