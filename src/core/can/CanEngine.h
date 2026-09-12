// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The heart of the measurement.
//
// One thread owns the whole consumer side: it drains every channel's queue on
// a fixed cadence and then runs the compiled pipeline graph, which is where
// filtering, decoding, logging and display all live as nodes.
//
// Since v0.4 the graph is the data path rather than a picture of it (rule #11).
// A channel is a source node, a panel is a sink node, and anything in between -
// DBC decoder, J1939 transport, Lua ECU - is a transform. addFrameSink() is
// kept as sugar for the common "give me the raw frames" case.
//
// Why a single dispatch thread rather than one per channel:
//
//   - Frames from different channels must reach the trace in a consistent
//     order relative to each other. Independent threads would interleave
//     non-deterministically and make a recording unreproducible.
//   - Sinks then need no locking of their own. A logger writing a file and a
//     trace store appending to a buffer are both called from the same thread,
//     always.
//   - One thread is enough: the per-frame work is a filter test and two
//     increments. The queues absorb the burstiness.
//
// The engine is Qt-free (rule #3). The UI adapter that marshals batches onto
// the GUI thread lives in the ui layer, not here.

#pragma once

#include "core/Result.h"
#include "core/can/CanChannel.h"
#include "core/can/CanFrame.h"
#include "core/can/CanStatistics.h"
#include "core/can/CanTypes.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/diagnostics/DiagnosticSession.h"
#include "core/dashboard/SystemVariables.h"
#include "core/scripting/ScriptLibrary.h"
#include "core/j1939/J1939Network.h"
#include "core/testing/TestReport.h"
#include "core/log/ReplayControl.h"
#include "core/log/TraceLog.h"
#include "core/plot/SignalSeries.h"
#include "core/trace/TraceStore.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace torquebus {

class TransmitList;

/// Receives batches of filtered frames on the engine thread.
///
/// Contract for every sink: do not block, do not throw, and do not call back
/// into the engine. A sink that needs to reach another thread (the UI, for
/// instance) posts to it and returns.
using FrameSink = std::function<void(std::span<const CanFrame>)>;

/// Called after each statistics window closes, with one snapshot per channel.
using StatisticsSink = std::function<void(std::span<const CanStatisticsSnapshot>)>;

/// Receives a line of text from a node - a Lua script's log_message(), or a
/// script error. Called on the engine thread, so the same rules apply: do not
/// block, post to the UI thread rather than touching it.
using LogSink = std::function<void(const std::string& text, bool isError)>;

/// Opaque handle used to remove a sink again.
using SinkId = std::uint64_t;

class CanEngine final {
public:
    struct Configuration final {
        /// How often the engine drains the queues. 200 Hz keeps latency to the
        /// trace imperceptible while batching enough to stay cheap.
        std::chrono::microseconds dispatchInterval{5000};

        /// Upper bound on frames drained from one channel in one pass, so a
        /// single saturated channel cannot starve the others.
        std::size_t maximumBatchSize{4096};

        /// How often statistics windows close and statistics sinks are called.
        std::chrono::milliseconds statisticsInterval{100};
    };

    CanEngine();
    explicit CanEngine(Configuration configuration);
    ~CanEngine();

    CanEngine(const CanEngine&) = delete;
    CanEngine& operator=(const CanEngine&) = delete;
    CanEngine(CanEngine&&) = delete;
    CanEngine& operator=(CanEngine&&) = delete;

    // --- Channel configuration (only while stopped) -----------------------

    /// Binds a backend to the next free application channel and returns its
    /// index. Fails while a measurement is running: rebinding hardware
    /// underneath a live trace is never what anyone means.
    [[nodiscard]] Result addChannel(std::unique_ptr<ICanBackend> backend,
                                    CanChannelConfig config,
                                    std::uint8_t* assignedIndex = nullptr);

    /// Removes every channel. Only valid while stopped.
    void clearChannels();

    [[nodiscard]] std::size_t channelCount() const;

    /// Nullptr when `index` is not bound.
    [[nodiscard]] CanChannel* channel(std::uint8_t index);
    [[nodiscard]] const CanChannel* channel(std::uint8_t index) const;

    // --- Measurement ------------------------------------------------------

    /// Opens and starts every channel, then starts the dispatch thread.
    ///
    /// All-or-nothing: if any channel fails to start, the ones already started
    /// are stopped again and the error is returned. A half-started measurement
    /// silently missing one bus is the worst possible outcome.
    [[nodiscard]] Result start();

    /// Stops the dispatch thread, then every channel. Performs one final drain
    /// so that frames already in the queues still reach the sinks.
    void stop();

    [[nodiscard]] bool isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    // --- Transmission -----------------------------------------------------

    /// Sends on the given application channel.
    [[nodiscard]] Result transmit(std::uint8_t applicationChannel, const CanFrame& frame);

    // --- Sinks ------------------------------------------------------------

    /// Registers a frame sink. Safe to call while running.
    SinkId addFrameSink(FrameSink sink);
    void removeFrameSink(SinkId id);

    SinkId addStatisticsSink(StatisticsSink sink);
    void removeStatisticsSink(SinkId id);

    /// Registers a sink for node output - the Output panel's supply. Safe to
    /// call while running.
    SinkId addLogSink(LogSink sink);
    void removeLogSink(SinkId id);

    // --- Observation ------------------------------------------------------

    /// One snapshot per channel, in channel order.
    [[nodiscard]] std::vector<CanStatisticsSnapshot> statistics() const;

    /// One node of the running pipeline, and what it has counted.
    struct NodeReport final {
        std::string name;
        std::string typeName;
        std::vector<NodeStatistic> statistics;
    };

    /// The pipeline's own counters, as of the last statistics window.
    ///
    /// A copy of a snapshot the dispatch thread took, not a live read. The
    /// counters are plain integers incremented on the hot path, and the only
    /// thread that touches them is the one running the graph - so the snapshot
    /// is taken there and this hands out a copy under a lock.
    ///
    /// Reading them directly from here would be a data race with a pleasant
    /// interface on it, and making every counter atomic would put a locked
    /// instruction on the frame path to serve a panel that repaints ten times a
    /// second.
    [[nodiscard]] std::vector<NodeReport> nodeStatistics() const;

    /// Total frames delivered to sinks since the measurement started.
    [[nodiscard]] std::uint64_t deliveredFrames() const noexcept
    {
        return m_deliveredFrames.load(std::memory_order_relaxed);
    }

    /// Runs one dispatch pass synchronously, on the calling thread.
    ///
    /// Exists for tests: it makes the pipeline deterministic without sleeping
    /// on a background thread. Never call it while the engine is running.
    std::size_t pumpOnce();

    // --- The pipeline graph -----------------------------------------------
    //
    // The graph is the data path (rule #11), not a view of it. addFrameSink()
    // above is sugar: it adds a FrameSinkNode and wires it to every channel
    // source, which is the shape almost every consumer wants and saves the
    // caller from touching the graph at all.
    //
    // Reach for the graph directly when a consumer needs to sit behind a
    // transform rather than on the raw frames.

    /// The graph this engine executes. Editing it while running does nothing:
    /// stop the measurement first (see the note on PipelineGraph).
    [[nodiscard]] PipelineGraph& graph() noexcept { return m_graph; }
    [[nodiscard]] const PipelineGraph& graph() const noexcept { return m_graph; }

    /// The source node standing for one application channel, so a caller can
    /// wire a transform to it. Invalid before start() builds the sources.
    [[nodiscard]] NodeId sourceNode(std::uint8_t applicationChannel) const;

    /// Adds the project's own nodes to the graph the engine has just built.
    ///
    /// A callback and not a list of nodes, because start() rebuilds the graph
    /// from scratch every time - a node handed over once would be destroyed on
    /// the second start, and the user would find their ECUs gone after the
    /// first stop. Re-running the builder means the graph is reconstructed the
    /// same way every time, from the project, which is the only version that
    /// stays correct when a channel is added or removed between runs.
    ///
    /// The consequence is worth stating: every start recreates the nodes, so a
    /// Lua ECU reloads its script and resets its state on Start. That is what
    /// pressing Start should mean, and it is why script edits take effect
    /// without restarting TorqueBus.
    ///
    /// `sources` are the channel source nodes in channel order, so the builder
    /// can wire to `[CAN 1]` without looking anything up. Returning a failed
    /// Result fails the start, with the message the user sees.
    using GraphBuilder =
        std::function<Result(PipelineGraph& graph, std::span<const NodeId> sources)>;

    /// Replaces the current builder. Takes effect at the next start().
    /// The transmit list nodes of type `transmit.list` will send from.
    ///
    /// Borrowed and not owned, and deliberately not copied into the graph
    /// description: the panel edits it while a measurement runs, which a copy
    /// taken at Start would freeze.
    void setTransmitList(TransmitList* list) noexcept { m_transmitList = list; }

    void setGraphBuilder(GraphBuilder builder);

    /// Builds the project's nodes from a description instead of from C++.
    ///
    /// This is the form the canvas and the project file use: the description is
    /// data the user edited, and the engine turns it into nodes on every start
    /// through the same builder as above. A copy is taken, so the caller may go
    /// on editing its own description while a measurement runs - what runs is
    /// what was set, not whatever the canvas has become since.
    /// `basePath` is the directory a relative path in a node's parameters is
    /// relative to - the project file's own folder. Empty resolves against the
    /// working directory, which is what a graph with no project behind it
    /// wants.
    void setGraphDescription(GraphDescription description,
                             NodeCatalog catalog,
                             std::string basePath = {});

    /// The trace store the default graph fills.
    ///
    /// This is the implicit default graph from ARCHITECTURE.md section 3b made
    /// concrete: detecting channels is enough to get `[CAN 1] -> [Trace]`
    /// running, with no canvas visit required. The panel reads this store; it
    /// does not create one.
    [[nodiscard]] TraceStore& traceStore() noexcept { return m_traceStore; }
    [[nodiscard]] const TraceStore& traceStore() const noexcept { return m_traceStore; }

    /// Opens `path` for recording, so a `can.log` block has somewhere to write.
    ///
    /// Called before start(), because the graph is built at start and a logger
    /// block refuses to build without an open writer - which is deliberate: a
    /// measurement that silently recorded nothing would be the worst outcome
    /// available.
    [[nodiscard]] Result startRecording(const std::string& path);
    void stopRecording();

    [[nodiscard]] bool isRecording() const noexcept { return m_logWriter.isOpen(); }

    /// The open log, for the frame and byte counts a status bar shows.
    [[nodiscard]] const TraceLogWriter& logWriter() const noexcept { return m_logWriter; }

    /// Where a `signal.plot` block's samples land, for the Graph panel.
    ///
    /// Owned here for the same reason the trace store is: it belongs to the
    /// measurement, it outlives any one compiled graph, and the panel reads it
    /// on its own timer while the executor writes.
    ///
    /// No implicit plot node, unlike the trace. A trace is what a measurement
    /// is *for* and costs nothing when nobody looks; a plot only means anything
    /// once a database is loaded and a decoder is wired, so building one into
    /// every graph would keep a store of signals nobody asked to see.
    [[nodiscard]] SignalSeriesStore& plotStore() noexcept { return m_plotStore; }
    [[nodiscard]] const SignalSeriesStore& plotStore() const noexcept { return m_plotStore; }

    /// The diagnostic conversation the console drives. Owned here for the same
    /// reason the replay transport is: it belongs to the measurement, and the
    /// GUI holds it while the executor thread reads it.
    [[nodiscard]] DiagnosticSession& diagnosticSession() noexcept { return m_diagnostics; }
    [[nodiscard]] const DiagnosticSession& diagnosticSession() const noexcept
    {
        return m_diagnostics;
    }

    /// The named values a dashboard and the scripts share.
    ///
    /// Owned here, and **not cleared between measurements**: a setpoint
    /// somebody dialled in before Start is still there when it starts and still
    /// there afterwards. A slider that reset itself every time would be the one
    /// behaviour nobody wants and everybody has met.
    [[nodiscard]] SystemVariables& variables() noexcept { return m_variables; }
    [[nodiscard]] const SystemVariables& variables() const noexcept { return m_variables; }

    /// What the J1939 block has learned about the bus.
    ///
    /// Owned here rather than by the block, like the test report and for the
    /// same reason: the block dies with the graph at Stop, and "who was on this
    /// bus" is a question somebody asks after the run as often as during it.
    [[nodiscard]] J1939Network& j1939Network() noexcept { return m_j1939Network; }
    [[nodiscard]] const J1939Network& j1939Network() const noexcept
    {
        return m_j1939Network;
    }

    /// What the running test sequence has found.
    ///
    /// Owned here rather than by the node, so a panel can go on showing the
    /// verdict after the measurement has stopped - which is the moment somebody
    /// actually reads it.
    [[nodiscard]] TestReport& testReport() noexcept { return m_testReport; }
    [[nodiscard]] const TestReport& testReport() const noexcept { return m_testReport; }

    /// Where the script editor hands edited scripts to running ECUs.
    ///
    /// Owned here, like the diagnostic session, and for the same reason: it
    /// belongs to the measurement, the GUI holds one end while the executor
    /// thread holds the other, and everything that crosses is behind the
    /// try_lock described in ScriptLibrary.h.
    [[nodiscard]] ScriptLibrary& scriptLibrary() noexcept { return m_scriptLibrary; }
    [[nodiscard]] const ScriptLibrary& scriptLibrary() const noexcept
    {
        return m_scriptLibrary;
    }

    /// The transport every replay block in this measurement obeys.
    ///
    /// Owned here for the same reason the trace store and the plot store are:
    /// it belongs to the measurement rather than to any one compiled graph, and
    /// the GUI holds it while the executor thread reads it. What crosses that
    /// boundary is atomics only - see ReplayControl.
    [[nodiscard]] ReplayControl& replayControl() noexcept { return m_replayControl; }
    [[nodiscard]] const ReplayControl& replayControl() const noexcept
    {
        return m_replayControl;
    }

    /// A registered sink and the handle that removes it again. Public only so
    /// that the dispatch helpers in the .cpp can name it.
    template <typename SinkType>
    struct Registration final {
        SinkId id{};
        SinkType sink;
    };

private:
    void dispatchLoop();
    std::size_t dispatchPass();
    void publishStatistics(std::uint64_t elapsedNs);

    /// Builds and compiles the graph for the current channels and sinks.
    [[nodiscard]] Result buildGraph();

    /// Frames the sink nodes have delivered so far, summed. Called before and
    /// after a pass to work out what that pass produced.
    [[nodiscard]] std::uint64_t totalSourceFrames() const;

    Configuration m_configuration;

    mutable std::mutex m_channelsMutex;
    std::vector<std::unique_ptr<CanChannel>> m_channels;

    /// The compiled data path. Rebuilt by start(), which creates one source
    /// node per channel and wires the registered sinks to them.
    PipelineGraph m_graph;

    /// Borrowed from the project. Null until the window hands one over.
    TransmitList* m_transmitList{nullptr};

    /// Re-run on every start; see setGraphBuilder().
    GraphBuilder m_graphBuilder;

    /// Source node per application channel, indexed by channel.
    std::vector<NodeId> m_sourceNodes;

    /// Sink id -> the node standing for it, so removeFrameSink() can take it
    /// back out of the graph.
    std::vector<std::pair<SinkId, NodeId>> m_sinkNodes;

    /// The trace, owned by the engine rather than by the panel or the graph.
    ///
    /// It has to outlive both: closing the Trace panel must not discard the
    /// measurement (the same reason closing it must not stop the logger, rule
    /// #7), and the graph is rebuilt on every start. The nodes that write into
    /// it are created per channel and hold a reference.
    TraceStore m_traceStore;
    SignalSeriesStore m_plotStore;
    TraceLogWriter m_logWriter;
    ReplayControl m_replayControl;
    DiagnosticSession m_diagnostics;
    ScriptLibrary m_scriptLibrary;
    TestReport m_testReport;
    J1939Network m_j1939Network;
    SystemVariables m_variables;

    mutable std::mutex m_sinksMutex;
    std::vector<Registration<FrameSink>> m_frameSinks;
    std::vector<Registration<StatisticsSink>> m_statisticsSinks;
    std::vector<Registration<LogSink>> m_logSinks;
    SinkId m_nextSinkId{1};

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_deliveredFrames{0};

    /// Reused across passes so a steady-state dispatch allocates nothing.
    /// Frame buffers now live in the nodes themselves; only the statistics
    /// snapshot is still gathered here.
    std::vector<CanStatisticsSnapshot> m_statisticsScratch;

    /// Written by the dispatch thread in publishStatistics, read by the UI.
    mutable std::mutex m_nodeStatisticsMutex;
    std::vector<NodeReport> m_nodeStatistics;
};

} // namespace torquebus
