// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Recording and replaying, as blocks on the canvas.
//
// This is where the log stops being a file format and becomes something the
// user can draw. A `can.log` wired downstream of a channel records what crossed
// it; a `log.source` wired upstream of anything replays a file as though it
// were a bus - and because it is a source like any other, everything that works
// on a live measurement works on a recorded one without knowing the difference.
//
// That last part is the whole point, and it is what rule #11 buys: the DBC
// decoder, the Lua ECUs, the filters, the trace and the plot were all written
// against "frames arrive on an edge". None of them had to learn what a file is.

#pragma once

#include "core/can/CanFrame.h"
#include "core/log/ReplayControl.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

/// Writes every frame it receives to a `.tblog`.
///
/// A sink and not a side effect bolted onto the channel, so that *what* gets
/// recorded is a wire somebody drew: put a filter in front of it and the log
/// holds only the identifiers that matter, which on a busy bus is the
/// difference between a 4 GB file and a 40 MB one.
class LogSinkNode final : public IPipelineNode {
public:
    /// The writer must already be open. The node records into it and does not
    /// own it: a measurement restarted without changing the graph should go on
    /// appending to the same file rather than silently starting a new one.
    explicit LogSinkNode(TraceLogWriter& writer, std::string label = "CAN Logger")
        : m_writer{writer}
        , m_label{std::move(label)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.log"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Frames recorded", m_recorded},
            // A recording that stopped because the disk filled up is the one
            // failure a logger must never keep quiet about.
            {"Frames the log refused", m_refused},
        };
    }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    TraceLogWriter& m_writer;
    std::string m_label;

    std::uint64_t m_recorded{0};
    std::uint64_t m_refused{0};
};

/// Replays a `.tblog` as a source of frames.
///
/// **In the recording's own time, not as fast as the disk allows.** A replayed
/// measurement has to look like the one that was recorded: cycle times, gaps
/// and bursts are most of what an engineer reads a trace for, and a file poured
/// through the pipeline at memory speed destroys all three. So the node holds
/// back each frame until its timestamp has come round again.
///
/// The speed multiplier is what makes that bearable: 10x to find the interesting
/// minute in a twenty-minute recording, then 1x to look at it.
///
/// **Driven, not just started.** A ReplayControl handed in at construction is
/// where pause, speed and seek arrive from, and where this node publishes how
/// far into the recording it has got. Without one the node plays the file
/// through at its configured speed, which is what it did before there was a
/// transport - the parameter stays the default so a graph built without a
/// control still works.
class LogSourceNode final : public IPipelineNode {
public:
    /// Opens `path`. A file that will not open leaves the node empty rather
    /// than failing - the catalog reports the reason at build time, and a node
    /// that threw here would take a whole measurement down for one bad path.
    explicit LogSourceNode(std::unique_ptr<TraceLogReader> reader,
                           double speed = 1.0,
                           std::string label = "Log Replay",
                           ReplayControl* control = nullptr);

    [[nodiscard]] std::string_view typeName() const noexcept override { return "log.source"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    /// True once the file has been read to its end.
    [[nodiscard]] bool finished() const noexcept { return m_finished; }

    /// How far into the recording the replay has reached, in nanoseconds from
    /// its first frame. The number the timeline is drawn from.
    [[nodiscard]] std::uint64_t positionNs() const noexcept { return m_positionNs; }

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Frames replayed", m_replayed},
            {"Frames still in the file", m_pending.size() - m_pendingFirst},
            // Frames a seek passed over. A big number here next to a small
            // "replayed" is somebody dragging the timeline, not a fault - but
            // it is also the first thing to look at if a seek felt slow.
            {"Frames skipped", m_skipped},
        };
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    /// Reads ahead and services a pending seek, both bounded per pass.
    void topUp();
    void serviceSeek();

    std::unique_ptr<TraceLogReader> m_reader;
    std::string m_label;

    /// How much faster than real time. 1.0 replays a minute in a minute.
    double m_speed{1.0};

    /// Read ahead of what has been emitted, so a pass never waits on the disk.
    /// `m_pendingFirst` is how far into it the replay has got.
    std::vector<CanFrame> m_pending;
    std::size_t m_pendingFirst{0};

    /// Published every pass; the span handed out points into here, so it must
    /// survive until the next process() (rule #12).
    std::vector<CanFrame> m_outgoing;

    /// Where the transport arrives from and where position goes back to. Null
    /// for a replay nobody is driving.
    ReplayControl* m_control{nullptr};

    /// When the last pass integrated the clock.
    ///
    /// Position is *accumulated* rather than derived from a start time, which
    /// is what makes pause and a speed changed mid-file possible at all: time
    /// that passed while paused, or at 0.5x, has to not count, and no start
    /// time can express that after the fact.
    std::chrono::steady_clock::time_point m_lastTick;

    /// How far into the recording the replay has reached, from its first frame.
    std::uint64_t m_positionNs{0};

    /// A seek that has been asked for and not finished. Served a bounded number
    /// of frames per pass: a seek to the end of a four-gigabyte log is a long
    /// read, and doing it in one pass would stall every other node in the
    /// graph - including the live channels of a measurement that is also
    /// recording.
    bool m_seekPending{false};
    std::uint64_t m_seekTargetNs{0};
    std::uint64_t m_skipped{0};

    /// The first frame's timestamp, so a recording that begins at 12.5 seconds
    /// starts playing immediately rather than after a twelve-second pause.
    std::uint64_t m_epochNs{0};
    bool m_haveEpoch{false};

    std::uint64_t m_replayed{0};
    bool m_finished{false};
};

} // namespace torquebus
