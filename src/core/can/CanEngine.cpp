// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanEngine.h"

#include "core/pipeline/nodes/FrameNodes.h"
#include "core/transmit/TransmitListNode.h"
#include "core/trace/TraceSinkNode.h"

#include <algorithm>
#include <format>
#include <utility>

namespace torquebus {
namespace {

using Clock = std::chrono::steady_clock;

/// Copies the sink list under the lock, so sinks are invoked without holding
/// it. A sink that registers or removes another sink would otherwise deadlock,
/// and a slow sink would block every registration in the application.
template <typename RegistrationType>
[[nodiscard]] auto copySinks(std::mutex& mutex, const std::vector<RegistrationType>& source)
{
    std::vector<decltype(RegistrationType::sink)> result;

    const std::lock_guard lock{mutex};
    result.reserve(source.size());
    for (const RegistrationType& registration : source) {
        result.push_back(registration.sink);
    }

    return result;
}

template <typename RegistrationType>
void eraseSink(std::mutex& mutex, std::vector<RegistrationType>& sinks, SinkId id)
{
    const std::lock_guard lock{mutex};
    std::erase_if(sinks, [id](const RegistrationType& registration) {
        return registration.id == id;
    });
}

} // namespace

CanEngine::CanEngine()
    : CanEngine{Configuration{}}
{
}

CanEngine::CanEngine(Configuration configuration)
    : m_configuration{configuration}
{
    // Frame buffers live in the pipeline nodes now; nothing to reserve here.
}

CanEngine::~CanEngine()
{
    CanEngine::stop();
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

Result CanEngine::addChannel(std::unique_ptr<ICanBackend> backend,
                             CanChannelConfig config,
                             std::uint8_t* assignedIndex)
{
    if (!backend) {
        return Result::error(ErrorCode::InvalidArgument, "No backend supplied");
    }

    if (isRunning()) {
        return Result::error(ErrorCode::InvalidState,
                             "Channels cannot be added while a measurement is running");
    }

    const std::lock_guard lock{m_channelsMutex};

    if (m_channels.size() >= 255) {
        return Result::error(ErrorCode::InvalidState, "Too many channels");
    }

    const auto index = static_cast<std::uint8_t>(m_channels.size());
    config.applicationChannel = index;

    m_channels.push_back(std::make_unique<CanChannel>(index, std::move(backend), std::move(config)));

    if (assignedIndex != nullptr) {
        *assignedIndex = index;
    }

    return Result::ok();
}

void CanEngine::clearChannels()
{
    if (isRunning()) {
        return;
    }

    const std::lock_guard lock{m_channelsMutex};
    m_channels.clear();
}

std::size_t CanEngine::channelCount() const
{
    const std::lock_guard lock{m_channelsMutex};
    return m_channels.size();
}

CanChannel* CanEngine::channel(std::uint8_t index)
{
    const std::lock_guard lock{m_channelsMutex};
    return index < m_channels.size() ? m_channels[index].get() : nullptr;
}

const CanChannel* CanEngine::channel(std::uint8_t index) const
{
    const std::lock_guard lock{m_channelsMutex};
    return index < m_channels.size() ? m_channels[index].get() : nullptr;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

Result CanEngine::start()
{
    if (isRunning()) {
        return Result::ok();
    }

    {
        const std::lock_guard lock{m_channelsMutex};

        if (m_channels.empty()) {
            return Result::error(ErrorCode::InvalidState,
                                 "No channels are configured. Map at least one application "
                                 "channel onto an interface first.");
        }

        std::size_t started = 0;
        for (const std::unique_ptr<CanChannel>& channel : m_channels) {
            if (Result result = channel->start(); result.failed()) {
                // All or nothing: unwind the channels that did start, so we
                // never leave a measurement running on a subset of the buses.
                for (std::size_t index = 0; index < started; ++index) {
                    m_channels[index]->stop();
                }

                return Result::error(
                    result.code(),
                    std::format("{} failed to start: {}",
                                channel->displayName(),
                                std::string{result.message()}));
            }
            ++started;
        }
    }

    if (Result result = buildGraph(); result.failed()) {
        const std::lock_guard lock{m_channelsMutex};
        for (const std::unique_ptr<CanChannel>& channel : m_channels) {
            channel->stop();
        }
        return result;
    }

    m_deliveredFrames.store(0, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread{[this] { dispatchLoop(); }};

    return Result::ok();
}

Result CanEngine::buildGraph()
{
    // Rebuilt from scratch on every start rather than patched incrementally.
    // The graph is small - tens of nodes - and a stale edge pointing at a
    // channel that no longer exists is the kind of bug that survives for
    // months. Rebuilding makes that impossible by construction.
    m_graph.clear();
    m_sourceNodes.clear();

    std::vector<std::pair<SinkId, FrameSink>> sinks;
    {
        const std::lock_guard lock{m_sinksMutex};
        sinks.reserve(m_frameSinks.size());
        for (const Registration<FrameSink>& registration : m_frameSinks) {
            sinks.emplace_back(registration.id, registration.sink);
        }
    }

    {
        const std::lock_guard lock{m_channelsMutex};
        m_sourceNodes.reserve(m_channels.size());

        for (const std::unique_ptr<CanChannel>& channel : m_channels) {
            m_sourceNodes.push_back(
                m_graph.addNode(std::make_unique<ChannelSourceNode>(*channel)));
        }
    }

    // Each registered sink becomes one node fed by every source. This is the
    // fan-in the graph otherwise refuses - and it is refused for good reason,
    // because merging needs a policy. Here the policy is explicit and is the
    // one the old sink list had: one sink node per source, all calling the same
    // callback, in channel order.
    m_sinkNodes.clear();

    for (const auto& [id, callback] : sinks) {
        for (const NodeId source : m_sourceNodes) {
            const NodeId sinkNode = m_graph.addNode(
                std::make_unique<FrameSinkNode>(callback, "sink"));

            m_sinkNodes.emplace_back(id, sinkNode);

            if (Result result = m_graph.connect(PortRef{source, 0}, PortRef{sinkNode, 0});
                result.failed()) {
                return result;
            }
        }
    }

    // The implicit default graph: every channel feeds the trace, with no user
    // action at all. ARCHITECTURE.md section 3b - reading a bus must never cost
    // a visit to the canvas.
    //
    // One node per channel, all writing into the engine's single store. An
    // input takes exactly one edge, so this is what "every channel feeds the
    // trace" has to look like; the executor's deterministic order is what keeps
    // the interleaving reproducible.
    for (const NodeId source : m_sourceNodes) {
        const NodeId traceNode =
            m_graph.addNode(std::make_unique<TraceSinkNode>(m_traceStore));

        if (Result result = m_graph.connect(PortRef{source, 0}, PortRef{traceNode, 0});
            result.failed()) {
            return result;
        }
    }

    // The implicit transmit path, for the same reason as the trace above.
    //
    // ARCHITECTURE.md 3b says reading a bus must never cost a visit to the
    // canvas. Transmitting is the same promise seen from the other side: a user
    // who fills in a row in the Transmit panel and presses Send has said
    // everything they mean, and requiring them to also drop a block and draw a
    // wire before anything happens would make the panel look broken.
    //
    // One pair per channel. Each list node collects only the rows that name its
    // own channel, so a row goes out once, on the bus it was addressed to.
    if (m_transmitList != nullptr) {
        // Counted in size_t and narrowed, not counted in uint8_t: a loop whose
        // counter is the same width as its limit never terminates once the
        // limit passes 255.
        for (std::size_t index = 0; index < m_channels.size(); ++index) {
            const auto channel = static_cast<std::uint8_t>(index);

            const NodeId listNode =
                m_graph.addNode(std::make_unique<TransmitListNode>(*m_transmitList, channel));

            const NodeId sink =
                m_graph.addNode(std::make_unique<ChannelSinkNode>(*m_channels[index]));

            if (Result result = m_graph.connect(PortRef{listNode, 0}, PortRef{sink, 0});
                result.failed()) {
                return result;
            }
        }
    }

    // The project's own nodes go on last, so a builder can wire to any source
    // and to anything the default graph put there. A failure here fails the
    // start rather than starting a measurement that quietly lacks half the
    // pipeline the user drew.
    if (m_graphBuilder) {
        if (Result result = m_graphBuilder(m_graph, m_sourceNodes); result.failed()) {
            return result;
        }
    }

    return m_graph.compile(m_configuration.maximumBatchSize);
}

void CanEngine::setGraphBuilder(GraphBuilder builder)
{
    m_graphBuilder = std::move(builder);
}

void CanEngine::setGraphDescription(GraphDescription description,
                                    NodeCatalog catalog,
                                    std::string basePath)
{
    // Captured by value into the builder, which is what makes the description
    // safe to edit while a measurement runs.
    setGraphBuilder([this, description = std::move(description),
                     catalog = std::move(catalog),
                     basePath = std::move(basePath)](PipelineGraph& graph,
                                                     std::span<const NodeId>) -> Result {
        NodeBuildContext context;
        context.basePath = basePath;
        context.traceStore = &m_traceStore;
        context.transmitList = m_transmitList;
        context.channel = [this](std::uint8_t index) { return channel(index); };
        context.log = [this](const std::string& text, bool isError) {
            // Copied under the lock and called outside it, like every other
            // fan-out here: a sink that blocks must not be holding the mutex
            // that the next node needs in order to log.
            const auto sinks = copySinks(m_sinksMutex, m_logSinks);
            for (const LogSink& sink : sinks) {
                sink(text, isError);
            }
        };

        return description.build(catalog, context, graph);
    });
}

NodeId CanEngine::sourceNode(std::uint8_t applicationChannel) const
{
    return applicationChannel < m_sourceNodes.size() ? m_sourceNodes[applicationChannel]
                                                     : NodeId::Invalid;
}

void CanEngine::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        const std::lock_guard lock{m_channelsMutex};
        for (const std::unique_ptr<CanChannel>& channel : m_channels) {
            channel->stop();
        }
    }

    // The backends have stopped, but their queues may still hold frames that
    // were received microseconds before. Those frames are real measurement
    // data - drain them rather than dropping them on the floor.
    dispatchPass();
}

Result CanEngine::transmit(std::uint8_t applicationChannel, const CanFrame& frame)
{
    CanChannel* target = channel(applicationChannel);

    if (target == nullptr) {
        return Result::error(ErrorCode::DeviceNotFound,
                             std::format("CAN {} is not configured", applicationChannel + 1));
    }

    return target->transmit(frame);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void CanEngine::dispatchLoop()
{
    Clock::time_point nextDispatch = Clock::now();
    Clock::time_point lastStatistics = Clock::now();

    while (m_running.load(std::memory_order_acquire)) {
        nextDispatch += m_configuration.dispatchInterval;

        dispatchPass();

        const Clock::time_point now = Clock::now();
        if (now - lastStatistics >= m_configuration.statisticsInterval) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastStatistics);
            publishStatistics(static_cast<std::uint64_t>(elapsed.count()));
            lastStatistics = now;
        }

        // If a pass overran its slot - a burst, a slow sink, a stalled disk -
        // do not try to catch up by spinning. Skip ahead and keep the cadence.
        if (nextDispatch < now) {
            nextDispatch = now;
        } else {
            std::this_thread::sleep_until(nextDispatch);
        }
    }
}

std::size_t CanEngine::dispatchPass()
{
    // One pass: walk the compiled graph once. The source nodes pull from their
    // channel queues, everything downstream runs in topological order, and the
    // sinks are reached as nodes like anything else.
    //
    // Nothing is dispatched by name or looked up here - that work happened at
    // compile time, which is the entire reason the graph is compiled (rule #12).
    if (!m_graph.isCompiled()) {
        return 0;
    }

    const std::lock_guard lock{m_channelsMutex};

    // Drain every channel first, then run the graph once. Two steps and not
    // one, so that every source node reading a channel sees the same frames -
    // see the note in CanChannel on the current pass.
    for (const std::unique_ptr<CanChannel>& channel : m_channels) {
        channel->beginPass(m_configuration.maximumBatchSize);
    }

    const std::uint64_t before = totalSourceFrames();
    m_graph.execute();
    const std::uint64_t after = totalSourceFrames();

    const std::uint64_t dispatched = after - before;
    m_deliveredFrames.fetch_add(dispatched, std::memory_order_relaxed);

    return static_cast<std::size_t>(dispatched);
}

std::uint64_t CanEngine::totalSourceFrames() const
{
    // Counted at the sinks rather than at the sources: "delivered" has always
    // meant "reached a consumer", and with a filter node in the middle those
    // two numbers are legitimately different.
    std::uint64_t total = 0;
    for (const auto& [id, node] : m_sinkNodes) {
        (void)id;
        if (const auto* sink = dynamic_cast<const FrameSinkNode*>(m_graph.node(node))) {
            total += sink->deliveredFrames();
        }
    }
    return total;
}

std::size_t CanEngine::pumpOnce()
{
    return dispatchPass();
}

std::vector<CanEngine::NodeReport> CanEngine::nodeStatistics() const
{
    const std::lock_guard lock{m_nodeStatisticsMutex};
    return m_nodeStatistics;
}

void CanEngine::publishStatistics(std::uint64_t elapsedNs)
{
    const auto sinks = copySinks(m_sinksMutex, m_statisticsSinks);

    m_statisticsScratch.clear();

    {
        const std::lock_guard lock{m_channelsMutex};
        m_statisticsScratch.reserve(m_channels.size());

        for (const std::unique_ptr<CanChannel>& channel : m_channels) {
            channel->closeStatisticsWindow(elapsedNs);
            m_statisticsScratch.push_back(channel->statistics());
        }
    }

    // The pipeline's counters, taken on the thread that owns them.
    //
    // Built into a local and swapped in under the lock, so the UI never sees a
    // half-filled table and the dispatch thread never holds the lock while
    // asking a node for its numbers.
    {
        std::vector<NodeReport> reports;

        for (const NodeId id : m_graph.nodeIds()) {
            const IPipelineNode* node = m_graph.node(id);
            if (node == nullptr) {
                continue;
            }

            std::vector<NodeStatistic> counters = node->statistics();
            if (counters.empty()) {
                // A node with nothing to report stays out of the table
                // entirely. A row of a name and no numbers is a row that only
                // makes the ones that matter harder to find.
                continue;
            }

            reports.push_back(NodeReport{node->displayName(),
                                         std::string{node->typeName()},
                                         std::move(counters)});
        }

        const std::lock_guard lock{m_nodeStatisticsMutex};
        m_nodeStatistics.swap(reports);
    }

    if (sinks.empty()) {
        return;
    }

    const std::span<const CanStatisticsSnapshot> snapshot{m_statisticsScratch};

    for (const StatisticsSink& sink : sinks) {
        if (sink) {
            sink(snapshot);
        }
    }
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

SinkId CanEngine::addFrameSink(FrameSink sink)
{
    const std::lock_guard lock{m_sinksMutex};
    const SinkId id = m_nextSinkId++;
    m_frameSinks.push_back({id, std::move(sink)});
    return id;
}

void CanEngine::removeFrameSink(SinkId id)
{
    eraseSink(m_sinksMutex, m_frameSinks, id);

    // Removing it from the list is not enough while a measurement is running.
    // The graph was compiled at start(), so this sink already has a node in the
    // execution plan holding its own copy of the callback - and that node keeps
    // calling it. addFrameSink's header says removal is safe while running, and
    // until now that was not true.
    //
    // The node is deactivated rather than removed: the dispatch thread is
    // walking the plan, and recompiling underneath it would mean locking the
    // hot path to serve an operation that happens once.
    const std::lock_guard lock{m_channelsMutex};

    for (const auto& [sinkId, nodeId] : m_sinkNodes) {
        if (sinkId != id) {
            continue;
        }

        if (auto* sink = dynamic_cast<FrameSinkNode*>(m_graph.node(nodeId))) {
            sink->deactivate();
        }
    }
}

SinkId CanEngine::addStatisticsSink(StatisticsSink sink)
{
    const std::lock_guard lock{m_sinksMutex};
    const SinkId id = m_nextSinkId++;
    m_statisticsSinks.push_back({id, std::move(sink)});
    return id;
}

void CanEngine::removeStatisticsSink(SinkId id)
{
    eraseSink(m_sinksMutex, m_statisticsSinks, id);
}

SinkId CanEngine::addLogSink(LogSink sink)
{
    const std::lock_guard lock{m_sinksMutex};
    const SinkId id = m_nextSinkId++;
    m_logSinks.push_back({id, std::move(sink)});
    return id;
}

void CanEngine::removeLogSink(SinkId id)
{
    eraseSink(m_sinksMutex, m_logSinks, id);
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

std::vector<CanStatisticsSnapshot> CanEngine::statistics() const
{
    const std::lock_guard lock{m_channelsMutex};

    std::vector<CanStatisticsSnapshot> result;
    result.reserve(m_channels.size());

    for (const std::unique_ptr<CanChannel>& channel : m_channels) {
        result.push_back(channel->statistics());
    }

    return result;
}

} // namespace torquebus
