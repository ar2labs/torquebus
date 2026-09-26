// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Remaining bus simulation: the traffic an ECU on a bench is waiting for.
//
// The cases divide into two halves. One is that the right messages go out at
// the right rate with the right values - the thing the feature is for. The
// other, and the longer half, is that the *wrong* ones do not: a node on the
// bench must never have its own messages sent for it, and a message the
// database gives no cycle time must not be invented into a periodic one.
//
// Both failures are silent on a trace that looks busy, which is why they are
// worth more cases than the happy path.

#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/simulation/RestBusNode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] CanSignal makeSignal(
    std::string name, std::uint16_t startBit, std::uint16_t bits, double factor, double offset)
{
    CanSignal signal;
    signal.name = std::move(name);
    signal.startBit = startBit;
    signal.bitLength = bits;
    signal.byteOrder = ByteOrder::Intel;
    signal.factor = factor;
    signal.offset = offset;

    return signal;
}

/// Two ECUs, three messages: one from the bench ECU, two from the rest of the
/// network, and one of those with no cycle time at all.
[[nodiscard]] std::shared_ptr<CanDatabase> makeDatabase()
{
    auto database = std::make_shared<CanDatabase>();
    database->nodes = {"Engine", "BodyController"};

    CanMessage engine;
    engine.identifier = 0x100;
    engine.name = "EngineData";
    engine.transmitter = "Engine";
    engine.length = 8;
    engine.cycleTimeMs = 10;
    engine.signalList.push_back(makeSignal("EngineSpeed", 0, 16, 0.125, 0.0));
    engine.signalList.push_back(makeSignal("CoolantTemp", 16, 8, 1.0, -40.0));
    database->addMessage(engine);

    CanMessage doors;
    doors.identifier = 0x200;
    doors.name = "DoorStatus";
    doors.transmitter = "BodyController";
    doors.length = 8;
    doors.cycleTimeMs = 20;
    doors.signalList.push_back(makeSignal("DriverDoor", 0, 1, 1.0, 0.0));
    database->addMessage(doors);

    CanMessage request;
    request.identifier = 0x300;
    request.name = "DiagnosticRequest";
    request.transmitter = "Engine";
    request.length = 8;
    // No cycle time: event-triggered, and a rest bus must not invent a period.
    database->addMessage(request);

    return database;
}

/// Collects whatever the rest bus sent.
class CollectNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.collect"; }
    [[nodiscard]] std::string displayName() const override { return "Collect"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            frames.push_back(frame);
        }
    }

    std::vector<CanFrame> frames;

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

struct Bench final {
    PipelineGraph graph;
    RestBusNode* bus{nullptr};
    CollectNode* collector{nullptr};

    Bench()
    {
        auto node = std::make_unique<RestBusNode>();
        bus = node.get();
        bus->setDatabase(makeDatabase());

        const NodeId busId = graph.addNode(std::move(node));

        auto sink = std::make_unique<CollectNode>();
        collector = sink.get();
        const NodeId sinkId = graph.addNode(std::move(sink));

        EXPECT_TRUE(graph.connect(PortRef{busId, 0}, PortRef{sinkId, 0}).succeeded());
    }

    void run(int milliseconds)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};

        while (std::chrono::steady_clock::now() < until) {
            graph.execute();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    [[nodiscard]] std::size_t countOf(std::uint32_t identifier) const
    {
        std::size_t count = 0;

        for (const CanFrame& frame : collector->frames) {
            if (frame.identifier == identifier) {
                ++count;
            }
        }

        return count;
    }
};

} // namespace

TEST(RestBusTests, TheRestBusSendsWhatTheNetworkWouldHaveSent)
{
    Bench bench;

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(200);

    // Both periodic messages, at roughly their own rates.
    const std::size_t engine = bench.countOf(0x100);
    const std::size_t doors = bench.countOf(0x200);

    SCOPED_TRACE(::testing::Message() << engine);
    SCOPED_TRACE(::testing::Message() << doors);

    EXPECT_TRUE(engine >= 8);
    EXPECT_TRUE(doors >= 4);
    EXPECT_TRUE(engine > doors);

    // And they are transmissions, not something that looks received.
    ASSERT_FALSE(bench.collector->frames.empty());
    EXPECT_TRUE(bench.collector->frames.front().direction == CanDirection::Tx);
}

TEST(RestBusTests, ANodeOnTheBenchDoesNotHaveItsMessagesSentForIt)
{
    // The case that matters most. Two of an ECU on one bus is a fault that
    // looks like a busy trace, and the engineer's own messages coming back at
    // them is the hardest kind to diagnose.
    Bench bench;
    bench.bus->setExcludedNodes({"Engine"});

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(150);

    EXPECT_TRUE(bench.countOf(0x100) == 0);
    EXPECT_TRUE(bench.countOf(0x200) >= 2);
}

TEST(RestBusTests, ExcludingBeatsIncluding)
{
    // A node in both lists is the one on the bench. The question "is this ECU
    // real?" has one answer, and the safe one is yes.
    Bench bench;
    bench.bus->setSimulatedNodes({"Engine", "BodyController"});
    bench.bus->setExcludedNodes({"Engine"});

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(120);

    EXPECT_TRUE(bench.countOf(0x100) == 0);
    EXPECT_TRUE(bench.countOf(0x200) >= 2);
}

TEST(RestBusTests, OnlyTheNamedNodesAreSimulated)
{
    Bench bench;
    bench.bus->setSimulatedNodes({"BodyController"});

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(120);

    EXPECT_TRUE(bench.countOf(0x100) == 0);
    EXPECT_TRUE(bench.countOf(0x200) >= 2);
}

TEST(RestBusTests, AMessageWithNoCycleTimeIsNotInventedIntoAPeriodicOne)
{
    // A database that declares no GenMsgCycleTime is not saying "every 100 ms".
    // Traffic the real network never carries is worse than missing traffic,
    // because it looks right.
    Bench bench;

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(200);

    EXPECT_TRUE(bench.countOf(0x300) == 0);

    // And it was counted, so a rest bus that sends nothing says why rather than
    // looking broken.
    std::uint64_t skipped = 0;

    for (const NodeStatistic& statistic : bench.bus->statistics()) {
        if (statistic.label == "Messages without a cycle time") {
            skipped = statistic.value;
        }
    }

    EXPECT_TRUE(skipped == 1);
}

TEST(RestBusTests, ADefaultCycleTimeIsHowYouOptIn)
{
    Bench bench;
    bench.bus->setDefaultCycleMs(20);

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(150);

    EXPECT_TRUE(bench.countOf(0x300) >= 2);
}

TEST(RestBusTests, SignalsHoldTheirDefaultUntilSomethingDrivesThem)
{
    // Raw zero, which is `offset` in physical terms. Definite rather than
    // uninitialised: a signal the caller did not set has to be something, and
    // zero is the one value anybody can predict.
    Bench bench;

    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(60);

    ASSERT_FALSE(bench.collector->frames.empty());

    for (const CanFrame& frame : bench.collector->frames) {
        if (frame.identifier != 0x100) {
            continue;
        }

        EXPECT_TRUE(frame.data[0] == 0);
        EXPECT_TRUE(frame.data[1] == 0);
        EXPECT_TRUE(frame.length == 8);
    }
}

TEST(RestBusTests, ADrivenSignalFollowsItsVariable)
{
    // The interactive half: a dashboard slider or a Lua script writes the
    // variable, and the rest bus carries it.
    SystemVariables variables;

    Bench bench;
    bench.bus->setSystemVariables(&variables);
    bench.bus->setDrivenSignals({{"EngineData", "EngineSpeed", "engine_speed"}});

    ASSERT_TRUE(bench.graph.compile().succeeded());

    // 2400 rpm at a factor of 0.125 is raw 19200 = 0x4B00, little-endian.
    variables.set("engine_speed", 2400.0);

    bench.run(80);

    ASSERT_FALSE(bench.collector->frames.empty());

    bool sawValue = false;

    for (const CanFrame& frame : bench.collector->frames) {
        if (frame.identifier != 0x100) {
            continue;
        }

        if (frame.data[0] == 0x00 && frame.data[1] == 0x4B) {
            sawValue = true;
        }
    }

    EXPECT_TRUE(sawValue);

    // And it keeps following: the value written now appears in the frames sent
    // after it, not only in the ones built at Start.
    variables.set("engine_speed", 800.0);
    bench.collector->frames.clear();

    bench.run(80);

    ASSERT_FALSE(bench.collector->frames.empty());

    // 800 / 0.125 = 6400 = 0x1900.
    bool sawNewValue = false;

    for (const CanFrame& frame : bench.collector->frames) {
        if (frame.identifier == 0x100 && frame.data[0] == 0x00 && frame.data[1] == 0x19) {
            sawNewValue = true;
        }
    }

    EXPECT_TRUE(sawNewValue);
}

TEST(RestBusTests, ADrivenSignalDefaultsToAVariableNamedAfterIt)
{
    SystemVariables variables;

    Bench bench;
    bench.bus->setSystemVariables(&variables);
    bench.bus->setDrivenSignals({{"EngineData", "CoolantTemp", ""}});

    ASSERT_TRUE(bench.graph.compile().succeeded());

    // Created by prepare(), which is what gives a dashboard a name to bind to.
    EXPECT_TRUE(variables.find("EngineData.CoolantTemp") != SystemVariables::kUnknown);

    variables.set("EngineData.CoolantTemp", 90.0);

    bench.run(60);

    bool sawValue = false;

    for (const CanFrame& frame : bench.collector->frames) {
        // Offset -40, factor 1: 90 degrees is raw 130.
        if (frame.identifier == 0x100 && frame.data[2] == 130) {
            sawValue = true;
        }
    }

    EXPECT_TRUE(sawValue);
}

TEST(RestBusTests, ASignalThatIsNotThereIsNamedNotIgnored)
{
    // A typo in a signal name is a control that does nothing, and finding that
    // out on a bench is expensive.
    SystemVariables variables;

    RestBusNode node;
    node.setDatabase(makeDatabase());
    node.setSystemVariables(&variables);
    node.setDrivenSignals({{"EngineData", "EngineSpeeed", ""}});

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("EngineSpeeed") != std::string::npos);
}

TEST(RestBusTests, ASignalOnAMessageThisBlockDoesNotSendIsNamed)
{
    // Including the case that is nobody's typo: the message is excluded, so the
    // control would silently do nothing.
    SystemVariables variables;

    RestBusNode node;
    node.setDatabase(makeDatabase());
    node.setSystemVariables(&variables);
    node.setExcludedNodes({"Engine"});
    node.setDrivenSignals({{"EngineData", "EngineSpeed", ""}});

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("EngineData") != std::string::npos);
}

TEST(RestBusTests, ARestBusWithNoDatabaseSaysSo)
{
    RestBusNode node;

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(std::string{result.message()}.find("database") != std::string::npos);
}

TEST(RestBusTests, MessagesDoNotAllFallDueInTheSamePass)
{
    // A real network does not start in lockstep, and a rest bus that put every
    // message in one pass every cycle would produce a trace with two hundred
    // frames sharing a timestamp - which reads as a fault in the tool.
    Bench bench;
    bench.bus->setDefaultCycleMs(20);

    ASSERT_TRUE(bench.graph.compile().succeeded());

    // One pass only: at t=0 nothing should be due for every message at once.
    bench.graph.execute();

    EXPECT_TRUE(bench.collector->frames.size() <= 2);
}
