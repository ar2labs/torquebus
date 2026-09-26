// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The whole loop, with nothing simulated except the wire:
//
//     [CAN 1 source] -> [Lua ECU] -> [CAN 1 transmit] -> the bus
//
// The unit tests prove the script sees the right values and emits the right
// frames. This file proves the frames leave: that a script's emit() reaches a
// real channel through the engine's own graph, that the engine rebuilds those
// nodes on every start rather than losing them, and that a broken script does
// not take the measurement with it.

#include "core/can/CanEngine.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

CanChannelConfig quietConfig(const std::string& handle)
{
    CanChannelConfig config;
    config.deviceHandle = handle;
    config.timing.bitrate = 500'000;
    // The backend generates no traffic unless asked, which is what these tests
    // need: they count what the script produced, and a background generator
    // would make that a moving target.
    return config;
}

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

/// Thread-safe frame collector, since sinks are called on the engine thread.
class Recorder final {
public:
    [[nodiscard]] FrameSink sink()
    {
        return [this](std::span<const CanFrame> batch) {
            const std::lock_guard lock{m_mutex};
            m_frames.insert(m_frames.end(), batch.begin(), batch.end());
        };
    }

    [[nodiscard]] std::vector<CanFrame> frames() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames;
    }

    [[nodiscard]] std::size_t countWithIdentifier(std::uint32_t identifier) const
    {
        const std::lock_guard lock{m_mutex};
        std::size_t count = 0;
        for (const CanFrame& item : m_frames) {
            if (item.identifier == identifier) {
                ++count;
            }
        }
        return count;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<CanFrame> m_frames;
};

} // namespace

TEST(LuaEcuBusTests, ALuaECUAnswersARequestOnTheBus)
{
    // The classic diagnostic exchange, and the smallest thing a simulated ECU
    // has to be able to do: hear a request, answer it.
    CanEngine engine;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    Recorder recorder;
    engine.addFrameSink(recorder.sink());

    engine.setGraphBuilder(
        [&engine](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
            const NodeId ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
            function on_message(id, data)
                if id == 0x7DF then
                    emit(0x7E8, "\2\1\0")
                end
            end
        )",
                                                                          "diag.lua"));

            const NodeId transmit =
                graph.addNode(std::make_unique<ChannelSinkNode>(*engine.channel(0)));

            if (Result result = graph.connect(PortRef{sources[0], 0}, PortRef{ecu, 0});
                result.failed()) {
                return result;
            }
            return graph.connect(PortRef{ecu, 0}, PortRef{transmit, 0});
        });

    ASSERT_TRUE(engine.start().succeeded());

    ASSERT_TRUE(engine.transmit(0, frame(0x7DF, 3)).succeeded());

    // The virtual backend echoes what is transmitted, so the request comes back
    // as a received frame, reaches the ECU, and the answer goes out the same
    // way. Two dispatch intervals is enough for both legs.
    std::this_thread::sleep_for(100ms);
    engine.stop();

    EXPECT_TRUE(recorder.countWithIdentifier(0x7DFU) >= 1);
    EXPECT_TRUE(recorder.countWithIdentifier(0x7E8U) >= 1);
}

TEST(LuaEcuBusTests, TheECUSurvivesAStopAndASecondStart)
{
    // The bug this guards against: the engine rebuilds its graph from scratch
    // on every start, so a node added to the graph once is destroyed by the
    // next start. A user who presses Stop and Start would find their ECUs gone
    // and no error to explain it. The builder is re-run instead.
    CanEngine engine;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    std::atomic<int> builds{0};

    engine.setGraphBuilder(
        [&engine, &builds](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
            ++builds;

            const NodeId ecu = graph.addNode(std::make_unique<LuaEcuNode>(
                R"(function on_message(id) if id == 0x100 then emit(0x101, "\1") end end)",
                "echo.lua"));
            const NodeId transmit =
                graph.addNode(std::make_unique<ChannelSinkNode>(*engine.channel(0)));

            if (Result result = graph.connect(PortRef{sources[0], 0}, PortRef{ecu, 0});
                result.failed()) {
                return result;
            }
            return graph.connect(PortRef{ecu, 0}, PortRef{transmit, 0});
        });

    for (int run = 0; run < 2; ++run) {
        Recorder recorder;
        const SinkId sink = engine.addFrameSink(recorder.sink());

        ASSERT_TRUE(engine.start().succeeded());
        ASSERT_TRUE(engine.transmit(0, frame(0x100, 1)).succeeded());
        std::this_thread::sleep_for(100ms);
        engine.stop();

        SCOPED_TRACE(::testing::Message() << "run " << run);
        EXPECT_TRUE(recorder.countWithIdentifier(0x101U) >= 1);

        engine.removeFrameSink(sink);
    }

    EXPECT_TRUE(builds.load() == 2);
}

TEST(LuaEcuBusTests, AScriptThatWillNotCompileFailsTheStartWithItsOwnMessage)
{
    // Better here than three seconds into a recording: the start fails, the
    // channels are stopped again, and the message names the file and the line.
    CanEngine engine;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    engine.setGraphBuilder([](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
        const NodeId ecu =
            graph.addNode(std::make_unique<LuaEcuNode>("function on_message( end", "broken.lua"));
        return graph.connect(PortRef{sources[0], 0}, PortRef{ecu, 0});
    });

    const Result result = engine.start();

    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(std::string{result.message()}.find("broken.lua") != std::string::npos);
    EXPECT_FALSE(engine.isRunning());
    // And the channel it had already opened is not left running behind a failed
    // start - all or nothing, as start() promises.
    EXPECT_FALSE(engine.channel(0)->isRunning());
}

TEST(LuaEcuBusTests, AFaultedECUDoesNotStopTheMeasurement)
{
    // One broken simulated ECU out of ten should cost that ECU, not the
    // recording. The trace keeps filling, the other nodes keep running.
    CanEngine engine;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    Recorder recorder;
    engine.addFrameSink(recorder.sink());

    std::vector<std::string> errors;
    std::mutex errorMutex;

    engine.setGraphBuilder([&](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
        auto broken = std::make_unique<LuaEcuNode>(
            R"(function on_message() local t = nil; return t.x end)", "faulty.lua");
        broken->setLogHandler([&](const std::string& text, bool isError) {
            if (isError) {
                const std::lock_guard lock{errorMutex};
                errors.push_back(text);
            }
        });

        const NodeId ecu = graph.addNode(std::move(broken));
        return graph.connect(PortRef{sources[0], 0}, PortRef{ecu, 0});
    });

    ASSERT_TRUE(engine.start().succeeded());

    for (int index = 0; index < 20; ++index) {
        ASSERT_TRUE(
            engine.transmit(0, frame(0x200 + static_cast<std::uint32_t>(index), 2)).succeeded());
    }

    std::this_thread::sleep_for(150ms);

    EXPECT_TRUE(engine.isRunning());
    engine.stop();

    // The frames still reached the sink and the trace: a script that throws is
    // a script problem, not a bus problem.
    EXPECT_TRUE(recorder.frames().size() >= 20);
    EXPECT_TRUE(engine.traceStore().size() >= 20);

    const std::lock_guard lock{errorMutex};
    // Reported a bounded number of times and then silenced, rather than once
    // per frame for the rest of the recording.
    EXPECT_TRUE(errors.size() == LuaEcuNode::kErrorLimit + 1);
}

TEST(LuaEcuBusTests, ACyclicECUPutsFramesOnTheBusWithNoInputAtAll)
{
    // The other half of what a simulated network needs: a node that generates
    // traffic on a timer, with nothing feeding it. This is how a bus full of
    // simulated ECUs comes to life with no real hardware attached.
    CanEngine engine;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    Recorder recorder;
    engine.addFrameSink(recorder.sink());

    engine.setGraphBuilder([&engine](PipelineGraph& graph, std::span<const NodeId>) -> Result {
        const NodeId ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
            counter = 0
            function on_enable() set_timer(10) end
            function on_timer()
                counter = counter + 1
                emit(0x300, string.pack("<I2", counter & 0xFFFF))
            end
        )",
                                                                      "cyclic.lua"));

        const NodeId transmit =
            graph.addNode(std::make_unique<ChannelSinkNode>(*engine.channel(0)));

        return graph.connect(PortRef{ecu, 0}, PortRef{transmit, 0});
    });

    ASSERT_TRUE(engine.start().succeeded());
    std::this_thread::sleep_for(200ms);
    engine.stop();

    // 200 ms at 10 ms per tick is about twenty frames. The assertion is
    // deliberately loose - this is a timing test on a shared CI machine, and
    // what it is proving is that the timer runs at all and at roughly the rate
    // asked for, not that the scheduler is exact.
    const std::size_t sent = recorder.countWithIdentifier(0x300U);
    SCOPED_TRACE(::testing::Message() << "frames on the bus: " << sent);
    EXPECT_TRUE(sent >= 5);
    EXPECT_TRUE(sent <= 40);
}
