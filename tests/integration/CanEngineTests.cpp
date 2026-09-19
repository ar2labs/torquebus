// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The v0.2 pipeline, end to end, over the virtual bus:
//
//     backend thread -> queue -> engine thread -> filter -> statistics -> sinks
//
// Every assertion here is about a property PLAN.md sections 16-17 promise, not
// about an implementation detail. If the queue, the filter or the dispatch
// strategy is rewritten, these tests should survive unchanged.

#include "core/can/CanEngine.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

CanChannelConfig configFor(const std::string& handle)
{
    CanChannelConfig config;
    config.deviceHandle = handle;
    config.timing.bitrate = 500'000;
    return config;
}

/// Builds a frame, choosing the format the identifier actually needs.
///
/// The engine refuses a 29-bit identifier declared as a standard frame - and
/// rightly so - which makes this helper the honest way to write a J1939
/// identifier in a test without restating the format every time.
CanFrame frame(std::uint32_t identifier, std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = identifier;
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    result.dlc = length;
    result.length = length;
    return result;
}

/// Thread-safe collector standing in for the trace store.
class SinkRecorder final {
public:
    [[nodiscard]] FrameSink sink()
    {
        return [this](std::span<const CanFrame> batch) {
            const std::lock_guard lock{m_mutex};
            m_batchSizes.push_back(batch.size());
            m_frames.insert(m_frames.end(), batch.begin(), batch.end());
        };
    }

    [[nodiscard]] std::size_t count() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames.size();
    }

    [[nodiscard]] std::vector<CanFrame> frames() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames;
    }

    [[nodiscard]] std::size_t batchCount() const
    {
        const std::lock_guard lock{m_mutex};
        return m_batchSizes.size();
    }

    /// Waits until at least `target` frames have arrived, or the timeout.
    bool waitFor(std::size_t target, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (count() >= target) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return count() >= target;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<CanFrame> m_frames;
    std::vector<std::size_t> m_batchSizes;
};

} // namespace

TEST_CASE("An engine with no channels refuses to start", "[engine]")
{
    // Better a clear error than a measurement that runs and shows nothing.
    CanEngine engine;

    const Result result = engine.start();
    CHECK(result.failed());
    CHECK(result.code() == ErrorCode::InvalidState);
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Channels are assigned sequential application indices", "[engine]")
{
    CanEngine engine;

    std::uint8_t first = 99;
    std::uint8_t second = 99;

    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"), &first)
                .succeeded());
    REQUIRE(
        engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:1"), &second)
            .succeeded());

    CHECK(first == 0);
    CHECK(second == 1);
    CHECK(engine.channelCount() == 2);

    REQUIRE(engine.channel(0) != nullptr);
    CHECK(engine.channel(0)->displayName() == "CAN 1");
    CHECK(engine.channel(1)->displayName() == "CAN 2");
    CHECK(engine.channel(2) == nullptr);
}

TEST_CASE("Channels cannot be rebound while a measurement runs", "[engine]")
{
    // Swapping hardware underneath a live trace is never what anyone means.
    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    REQUIRE(engine.start().succeeded());

    const Result result =
        engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:1"));
    CHECK(result.code() == ErrorCode::InvalidState);
    CHECK(engine.channelCount() == 1);

    engine.stop();
}

TEST_CASE("A channel that cannot start aborts the whole measurement", "[engine]")
{
    // All or nothing: a measurement silently missing one of three buses is the
    // worst possible outcome, because the trace looks plausible and is wrong.
    CanEngine engine;

    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("nonexistent:7"))
                .succeeded());

    const Result result = engine.start();

    CHECK(result.failed());
    CHECK_FALSE(engine.isRunning());
    CHECK_FALSE(engine.channel(0)->isRunning());
}

TEST_CASE("Frames travel from one channel to the other through the engine", "[engine][pipeline]")
{
    SinkRecorder recorder;

    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());
    REQUIRE(engine.transmit(0, frame(0x18FF50E5)).succeeded());

    // One transmission, two frames delivered: the Tx echo on CAN 1 and the Rx
    // copy on CAN 2 - the same shape a physical two-adapter setup produces.
    REQUIRE(recorder.waitFor(2, 2s));

    engine.stop();

    const std::vector<CanFrame> frames = recorder.frames();
    REQUIRE(frames.size() >= 2);

    bool sawTxOnChannel0 = false;
    bool sawRxOnChannel1 = false;

    for (const CanFrame& received : frames) {
        CHECK(received.identifier == 0x18FF50E5);
        if (received.channel == 0 && received.direction == CanDirection::Tx) {
            sawTxOnChannel0 = true;
        }
        if (received.channel == 1 && received.direction == CanDirection::Rx) {
            sawRxOnChannel1 = true;
        }
    }

    CHECK(sawTxOnChannel0);
    CHECK(sawRxOnChannel1);
}

TEST_CASE("Sinks receive batches, not one call per frame", "[engine][pipeline]")
{
    // Rule #5 in observable form. If this ratio ever approaches 1:1, the
    // pipeline has regressed into per-frame signalling.
    SinkRecorder recorder;

    VirtualTrafficPattern traffic;
    traffic.framesPerSecond = 20'000;
    traffic.totalFrames = 5'000;

    auto backend = std::make_unique<VirtualCanBackend>();
    backend->setTrafficPattern(traffic);

    CanEngine engine;
    REQUIRE(engine.addChannel(std::move(backend), configFor("virtual:0")).succeeded());
    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());
    REQUIRE(recorder.waitFor(5'000, 10s));
    engine.stop();

    CHECK(recorder.count() >= 5'000);
    CHECK(recorder.batchCount() < recorder.count() / 4);
}

TEST_CASE("Filters are applied before frames reach any sink", "[engine][filter]")
{
    SinkRecorder recorder;

    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    engine.channel(0)->filters().add(CanFilter::acceptIdentifier(0x200));
    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());

    REQUIRE(engine.transmit(0, frame(0x100)).succeeded());
    REQUIRE(engine.transmit(0, frame(0x200)).succeeded());
    REQUIRE(engine.transmit(0, frame(0x300)).succeeded());

    REQUIRE(recorder.waitFor(1, 2s));
    std::this_thread::sleep_for(100ms); // give the others a chance to arrive
    engine.stop();

    const std::vector<CanFrame> frames = recorder.frames();
    REQUIRE_FALSE(frames.empty());

    for (const CanFrame& received : frames) {
        CHECK(received.identifier == 0x200);
    }

    CHECK(engine.channel(0)->statistics().filteredFrames >= 2);
}

TEST_CASE("Statistics count what actually reached the sinks", "[engine][statistics]")
{
    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    SinkRecorder recorder;
    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());

    for (int index = 0; index < 10; ++index) {
        REQUIRE(engine.transmit(0, frame(0x100 + static_cast<std::uint32_t>(index))).succeeded());
    }

    REQUIRE(recorder.waitFor(10, 2s));
    engine.stop();

    const CanStatisticsSnapshot snapshot = engine.channel(0)->statistics();
    CHECK(snapshot.txFrames == 10);
    CHECK(snapshot.rxFrames == 0);
    CHECK(snapshot.bitrate == 500'000);
    CHECK(engine.deliveredFrames() >= 10);
}

TEST_CASE("Statistics sinks are called while the measurement runs", "[engine][statistics]")
{
    std::atomic<int> updates{0};
    std::atomic<std::size_t> channelsReported{0};

    CanEngine::Configuration configuration;
    configuration.statisticsInterval = 20ms;

    CanEngine engine{configuration};
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    engine.addStatisticsSink([&](std::span<const CanStatisticsSnapshot> snapshot) {
        channelsReported.store(snapshot.size());
        updates.fetch_add(1);
    });

    REQUIRE(engine.start().succeeded());
    std::this_thread::sleep_for(200ms);
    engine.stop();

    CHECK(updates.load() >= 3);
    CHECK(channelsReported.load() == 1);
}

TEST_CASE("A measurement starts from zero every time", "[engine][statistics]")
{
    // Counters bleeding across runs turn a five-minute test into a
    // misdiagnosis. Starting resets everything.
    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    SinkRecorder first;
    const SinkId firstId = engine.addFrameSink(first.sink());

    REQUIRE(engine.start().succeeded());
    REQUIRE(engine.transmit(0, frame(0x100)).succeeded());
    REQUIRE(first.waitFor(1, 2s));
    engine.stop();

    CHECK(engine.channel(0)->statistics().totalFrames() == 1);

    engine.removeFrameSink(firstId);

    SinkRecorder second;
    engine.addFrameSink(second.sink());

    REQUIRE(engine.start().succeeded());
    REQUIRE(engine.transmit(0, frame(0x200)).succeeded());
    REQUIRE(second.waitFor(1, 2s));
    engine.stop();

    CHECK(engine.channel(0)->statistics().totalFrames() == 1);
    CHECK(second.count() == 1);
    CHECK(second.frames().front().identifier == 0x200);
}

TEST_CASE("A removed sink stops receiving frames", "[engine][sinks]")
{
    SinkRecorder kept;
    SinkRecorder removed;

    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());

    engine.addFrameSink(kept.sink());
    const SinkId id = engine.addFrameSink(removed.sink());

    REQUIRE(engine.start().succeeded());
    REQUIRE(engine.transmit(0, frame(0x100)).succeeded());
    REQUIRE(kept.waitFor(1, 2s));

    engine.removeFrameSink(id);
    const std::size_t frozen = removed.count();

    for (int index = 0; index < 20; ++index) {
        REQUIRE(engine.transmit(0, frame(0x200)).succeeded());
    }
    REQUIRE(kept.waitFor(21, 2s));

    engine.stop();

    CHECK(removed.count() == frozen);
    CHECK(kept.count() >= 21);
}

TEST_CASE("Stopping delivers frames still in flight", "[engine][lifecycle]")
{
    // Frames received microseconds before the stop are measurement data like
    // any other. Dropping them would make the end of every recording a lie.
    SinkRecorder recorder;

    CanEngine::Configuration configuration;
    configuration.dispatchInterval = 200ms; // deliberately sluggish

    CanEngine engine{configuration};
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());

    for (int index = 0; index < 50; ++index) {
        REQUIRE(engine.transmit(0, frame(0x100)).succeeded());
    }

    // Let the backend thread deliver into the queue, then stop before the
    // engine's slow dispatch timer has necessarily fired.
    std::this_thread::sleep_for(50ms);
    engine.stop();

    CHECK(recorder.count() == 50);
}

TEST_CASE("Transmitting on an unconfigured channel is an error", "[engine]")
{
    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    REQUIRE(engine.start().succeeded());

    const Result result = engine.transmit(4, frame(0x100));
    CHECK(result.code() == ErrorCode::DeviceNotFound);
    CHECK_FALSE(result.message().empty());

    engine.stop();
}

TEST_CASE("The engine sustains 100k frames per second without loss", "[engine][throughput]")
{
    // PLAN.md section 17: 100k+ frames/s internally with no loss in the
    // pipeline. This is a requirement, not an aspiration, so it runs on every
    // pull request rather than living behind an opt-in tag - a regression here
    // is exactly the kind that is cheap to fix today and structural in a year.
    constexpr std::uint64_t kTargetFrames = 300'000;

    SinkRecorder recorder;

    VirtualTrafficPattern traffic;
    traffic.framesPerSecond = 150'000;
    traffic.totalFrames = kTargetFrames;
    traffic.messageCount = 64;

    auto backend = std::make_unique<VirtualCanBackend>();
    backend->setTrafficPattern(traffic);
    VirtualCanBackend* backendPtr = backend.get();

    CanEngine::Configuration configuration;
    configuration.dispatchInterval = 2ms;
    configuration.maximumBatchSize = 8192;

    CanEngine engine{configuration};

    // A generous queue: the point of the test is the pipeline's throughput,
    // not how it degrades when deliberately starved.
    REQUIRE(engine.addChannel(std::move(backend), configFor("virtual:0")).succeeded());
    engine.addFrameSink(recorder.sink());

    const auto started = std::chrono::steady_clock::now();

    REQUIRE(engine.start().succeeded());
    REQUIRE(backendPtr->waitForTrafficCompletion(20s));
    REQUIRE(recorder.waitFor(kTargetFrames, 10s));

    engine.stop();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    const CanStatisticsSnapshot snapshot = engine.channel(0)->statistics();

    INFO("elapsed = " << elapsed.count() << " ms");
    INFO("delivered = " << recorder.count());
    INFO("dropped = " << snapshot.droppedFrames);

    CHECK(recorder.count() >= kTargetFrames);
    CHECK(snapshot.droppedFrames == 0);

    const double achievedRate =
        static_cast<double>(recorder.count()) / (static_cast<double>(elapsed.count()) / 1000.0);
    INFO("achieved = " << achievedRate << " frames/s");
    CHECK(achievedRate > 100'000.0);
}

TEST_CASE("The engine has headroom above the rate it is required to sustain",
          "[engine][throughput]")
{
    // The test above answers "does it meet the requirement". This one answers
    // "by how much", and the two are different questions.
    //
    // It exists because the number in the README had no source. The required
    // rate is throttled by the generator, so the figure that test reports is
    // the generator's setting and not a ceiling - reading it as one is how a
    // ceiling gets claimed that nobody measured. Here the generator is told to
    // go as fast as it can and the pipeline is what limits the result.
    //
    // The floor is deliberately no higher than the requirement: this is a
    // measurement, and a measurement that fails the build on a loaded CI
    // runner is a measurement nobody keeps. The number to read is the printed
    // one, compared against the same line from another build.
    //
    // **Frames are expected to be dropped here, and that is not a fault.** The
    // first version of this test asserted zero loss and waited for every frame
    // to arrive, with a comment calling zero loss "the contract at whatever
    // rate the machine reaches". That was wrong, and the architecture says so:
    // the queues are bounded, and `softwareOverruns` exists precisely because a
    // producer can outrun a consumer (ARCHITECTURE.md, "Who counts a dropped
    // frame"). Telling the generator to go as fast as it can and then demanding
    // nothing be lost is asking a bounded queue to be unbounded.
    //
    // It passed in Debug and failed in RelWithDebInfo, which is the giveaway: a
    // test whose outcome depends on how the optimiser happens to balance
    // producer against consumer is measuring the compiler, not the pipeline.
    // Zero loss *at the required rate* is the other test's job, and it is
    // throttled to that rate for exactly this reason.
    constexpr std::uint64_t kTargetFrames = 2'000'000;

    SinkRecorder recorder;

    VirtualTrafficPattern traffic;
    traffic.framesPerSecond = 100'000'000; // unreachable on purpose: no throttle
    traffic.totalFrames = kTargetFrames;
    traffic.messageCount = 64;

    auto backend = std::make_unique<VirtualCanBackend>();
    backend->setTrafficPattern(traffic);
    VirtualCanBackend* backendPtr = backend.get();

    CanEngine::Configuration configuration;
    configuration.dispatchInterval = 2ms;
    configuration.maximumBatchSize = 8192;

    CanEngine engine{configuration};

    REQUIRE(engine.addChannel(std::move(backend), configFor("virtual:0")).succeeded());
    engine.addFrameSink(recorder.sink());

    const auto started = std::chrono::steady_clock::now();

    REQUIRE(engine.start().succeeded());
    REQUIRE(backendPtr->waitForTrafficCompletion(60s));

    // Drained, not completed: what arrives is what the pipeline could carry,
    // and the difference between that and kTargetFrames is the headroom
    // question's actual answer.
    engine.stop();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    const CanStatisticsSnapshot snapshot = engine.channel(0)->statistics();

    const double achievedRate =
        static_cast<double>(recorder.count()) / (static_cast<double>(elapsed.count()) / 1000.0);

    WARN("engine throughput: " << static_cast<std::uint64_t>(achievedRate)
                               << " frames/s delivered  (" << recorder.count() << " of "
                               << kTargetFrames << " frames in " << elapsed.count() << " ms, "
                               << snapshot.droppedFrames << " dropped by a bounded queue)");

    // Something got through, and fast. Both loose on purpose - see above.
    CHECK(recorder.count() > 0);
    CHECK(achievedRate > 100'000.0);
}

TEST_CASE("The engine runs its work through the graph, not around it", "[engine][pipeline]")
{
    // Rule #11 made observable: there is no second data path. Starting the
    // engine compiles a graph, and the sinks the caller registered are nodes
    // in it.
    SinkRecorder recorder;

    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    engine.addFrameSink(recorder.sink());

    CHECK_FALSE(engine.graph().isCompiled());

    REQUIRE(engine.start().succeeded());

    CHECK(engine.graph().isCompiled());

    // Three nodes and two edges, not two and one: the engine also builds the
    // implicit trace path, so every channel feeds the trace store whether or
    // not anyone registered a sink (ARCHITECTURE.md, "The default graph is
    // implicit"). This test predates that and was counting the graph the
    // engine used to build.
    CHECK(engine.graph().nodeCount() == 3); // source, frame sink, trace sink
    CHECK(engine.graph().edges().size() == 2);
    CHECK(engine.sourceNode(0) != NodeId::Invalid);
    CHECK(engine.sourceNode(9) == NodeId::Invalid);

    REQUIRE(engine.transmit(0, frame(0x123)).succeeded());
    REQUIRE(recorder.waitFor(1, 2s));

    engine.stop();
}

TEST_CASE("Starting rebuilds the graph from the registered sinks", "[engine][pipeline]")
{
    // The honest statement of what v0.4 does, and what it does not.
    //
    // The graph is authoritative at *runtime* - every frame goes through it -
    // but its topology is still derived from the sink list on each start,
    // rather than being something the user composed and we persisted. Nodes
    // added to graph() by hand are therefore discarded by the next start().
    //
    // That is not a bug to work around in a test; it is the boundary of this
    // milestone. Composed topology becomes editable with the canvas (v0.6) and
    // durable with the project file (v0.13). Until then, this test pins the
    // behaviour so the change is deliberate when it comes.
    SinkRecorder recorder;

    CanEngine engine;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), configFor("virtual:0"))
                .succeeded());
    engine.addFrameSink(recorder.sink());

    REQUIRE(engine.start().succeeded());
    const std::size_t nodesFromSinks = engine.graph().nodeCount();
    engine.stop();

    // Add a node by hand while stopped.
    (void)engine.graph().addNode(std::make_unique<FrameFilterNode>());
    CHECK(engine.graph().nodeCount() == nodesFromSinks + 1);

    // ...and watch the next start rebuild without it.
    REQUIRE(engine.start().succeeded());
    CHECK(engine.graph().nodeCount() == nodesFromSinks);

    engine.stop();
}
