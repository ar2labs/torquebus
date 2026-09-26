// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What makes simulated traffic look like a vehicle rather than a generator:
// several messages at their own cycle times, payloads that change between them,
// and a message that can be silenced.
//
// These cases do wait on a real clock, unlike the protocol suites - the node
// reads steady_clock inside process() and there is nowhere to hand it a
// different one. The waits are tens of milliseconds and every assertion is a
// range rather than an exact count, because a test that demands exactly six
// ticks in 60 ms is a test that fails on a loaded machine and teaches people to
// re-run the suite.

#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// Keeps every frame the ECU produced.
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

/// One scripted ECU, run for a while, with everything it sent collected.
struct Bench final {
    PipelineGraph graph;
    LuaEcuNode* ecu{nullptr};
    CollectNode* collector{nullptr};

    explicit Bench(const std::string& source)
    {
        auto node = std::make_unique<LuaEcuNode>(source, "timing.lua");
        ecu = node.get();
        const NodeId ecuId = graph.addNode(std::move(node));

        auto sink = std::make_unique<CollectNode>();
        collector = sink.get();
        const NodeId sinkId = graph.addNode(std::move(sink));

        EXPECT_TRUE(graph.connect(PortRef{ecuId, 0}, PortRef{sinkId, 0}).succeeded());
        EXPECT_TRUE(graph.compile().succeeded());
    }

    /// Runs for `milliseconds`, dispatching about as often as the executor does.
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

TEST(LuaTimingTests, SeveralTimersRunAtTheirOwnRates)
{
    // The thing set_timer could not do: an ECU sends a 10 ms message and a
    // 100 ms one, which is what every real one does and what one timer forces
    // a script to fake with a counter.
    Bench bench{R"(
        function on_enable()
            every(10, function() emit(0x100, "\1") end)
            every(50, function() emit(0x200, "\2") end)
        end
    )"};

    bench.run(220);

    const std::size_t fast = bench.countOf(0x100);
    const std::size_t slow = bench.countOf(0x200);

    // Ranges, not counts: the executor dispatches on its own rhythm and the
    // machine is shared.
    EXPECT_TRUE(fast >= 12);
    EXPECT_TRUE(fast <= 30);

    EXPECT_TRUE(slow >= 3);
    EXPECT_TRUE(slow <= 7);

    // And the relationship holds even when both bounds are generous.
    EXPECT_TRUE(fast > slow * 2);
}

TEST(LuaTimingTests, ACyclicMessageSendsItself)
{
    // Declared once, no timer body, no bookkeeping in the script - which is the
    // whole point: an ECU's periodic traffic is a list of facts, not a program.
    Bench bench{R"(
        function on_enable()
            cyclic(0x123, 20, "\xAA\xBB")
        end
    )"};

    bench.run(150);

    const std::size_t count = bench.countOf(0x123);
    EXPECT_TRUE(count >= 4);
    EXPECT_TRUE(count <= 12);

    ASSERT_FALSE(bench.collector->frames.empty());

    const CanFrame& frame = bench.collector->frames.front();
    EXPECT_TRUE(frame.length == 2);
    EXPECT_TRUE(frame.data[0] == 0xAA);
    EXPECT_TRUE(frame.data[1] == 0xBB);
}

TEST(LuaTimingTests, ACyclicMessageCanBuildItsPayloadEachTime)
{
    // A rolling counter, which nearly every real message carries so a receiver
    // can tell a repeated frame from a fresh one.
    Bench bench{R"(
        local next_counter = tb.counter(4)

        function on_enable()
            cyclic(0x321, 10, function()
                return string.char(next_counter())
            end)
        end
    )"};

    bench.run(140);

    ASSERT_TRUE(bench.collector->frames.size() >= 4);

    // Successive frames carry successive counters, wrapping at 16.
    for (std::size_t index = 1; index < bench.collector->frames.size(); ++index) {
        const std::uint8_t previous = bench.collector->frames[index - 1].data[0];
        const std::uint8_t current = bench.collector->frames[index].data[0];

        EXPECT_TRUE(current == static_cast<std::uint8_t>((previous + 1) % 16));
    }
}

TEST(LuaTimingTests, ACyclicMessageCanBeSilencedAndBroughtBack)
{
    // What does the rest of the network do when this ECU goes quiet? The
    // question fault injection exists to ask - and stopping is not a deletion:
    // the message comes back where it was, with the cycle time it had.
    //
    // Driven from the clock rather than from a count of calls, because `every`
    // fires *immediately* and then on its period - which the first version of
    // this test did not expect, and which cost twenty minutes of blaming the
    // scheduler for doing exactly what it says it does.
    Bench bench{R"(
        function on_enable()
            cyclic(0x400, 10, "\1")

            every(5, function()
                local seconds = tb.now()
                stop_cyclic(0x400, not (seconds > 0.06 and seconds < 0.14))
            end)
        end
    )"};

    bench.run(50);
    const std::size_t beforeStop = bench.countOf(0x400);
    EXPECT_TRUE(beforeStop >= 2);

    bench.run(50);
    const std::size_t whileStopped = bench.countOf(0x400);

    // Nothing, or the one frame that was already due when the stop landed.
    EXPECT_TRUE(whileStopped <= beforeStop + 2);

    bench.run(90);

    // And it came back.
    EXPECT_TRUE(bench.countOf(0x400) > whileStopped + 1);
}

TEST(LuaTimingTests, AProviderThatReturnsNothingSkipsACycle)
{
    // A legitimate way for a script to say "not this time" - a message that is
    // only sent while a condition holds - and not an error worth counting
    // against the script.
    Bench bench{R"(
        sending = false

        function on_enable()
            cyclic(0x500, 10, function()
                if sending then
                    return "\1"
                end
            end)

            every(50, function() sending = true end)
        end
    )"};

    bench.run(200);

    const std::size_t count = bench.countOf(0x500);

    // Some frames, but not one per cycle: the first fifty milliseconds are
    // silent.
    EXPECT_TRUE(count >= 4);
    EXPECT_TRUE(count <= 22);
}

TEST(LuaTimingTests, DeclaringTheSameCyclicMessageTwiceReplacesIt)
{
    // Two jobs sending 0x600 at different rates is never what anybody meant,
    // and it is what a script edited and re-run would otherwise produce.
    Bench bench{R"(
        function on_enable()
            cyclic(0x600, 10, "\1")
            cyclic(0x600, 10, "\2")
        end
    )"};

    bench.run(80);

    ASSERT_FALSE(bench.collector->frames.empty());

    for (const CanFrame& frame : bench.collector->frames) {
        EXPECT_TRUE(frame.data[0] == 0x02);
    }
}

TEST(LuaTimingTests, APeriodHasToBeAPositiveNumberOfMilliseconds)
{
    // A period of zero is a loop that never yields. Refused where it was
    // written rather than at the first tick - and refused at Start, like every
    // other error in on_enable, rather than leaving a half-configured ECU
    // running.
    LuaEcuNode node{R"(
        function on_enable()
            every(0, function() end)
        end
    )",
                    "timing.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("positive") != std::string::npos);
}

TEST(LuaTimingTests, EveryWantsAFunctionAndSaysSo)
{
    LuaEcuNode node{R"(
        function on_enable()
            every(100, "not a function")
        end
    )",
                    "timing.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("function") != std::string::npos);
}

TEST(LuaTimingTests, TheGeneratorsAreArithmeticOverTheMeasurementClock)
{
    // Checked through what they produce rather than by reading them back: a
    // ramp that never leaves its bounds and a sine that goes both above and
    // below the middle are the two things a wrong implementation gets wrong.
    Bench bench{R"(
        function on_enable()
            local ramp = tb.ramp(0, 100, 0.2)
            local sine = tb.sine(0, 200, 0.2)

            cyclic(0x700, 10, function()
                return string.char(math.floor(ramp()), math.floor(sine() / 2))
            end)
        end
    )"};

    bench.run(300);

    ASSERT_TRUE(bench.collector->frames.size() >= 8);

    bool sawLow = false;
    bool sawHigh = false;

    for (const CanFrame& frame : bench.collector->frames) {
        EXPECT_TRUE(frame.data[0] <= 100);

        if (frame.data[1] < 40) {
            sawLow = true;
        }
        if (frame.data[1] > 60) {
            sawHigh = true;
        }
    }

    EXPECT_TRUE(sawLow);
    EXPECT_TRUE(sawHigh);
}

TEST(LuaTimingTests, ResumingAStoppedMessageDoesNotProduceABurst)
{
    // Everything that was missed while it was stopped is *not* sent when it
    // comes back: the resumed job fires on its next cycle. A burst would be
    // worse than the silence it was meant to simulate, because it looks like a
    // fault in the tool rather than the one being injected.
    Bench bench{R"(
        function on_enable()
            cyclic(0x800, 5, "\1")

            every(5, function()
                local seconds = tb.now()
                stop_cyclic(0x800, not (seconds > 0.02 and seconds < 0.12))
            end)
        end
    )"};

    bench.run(140);

    // 140 ms at 5 ms would be 28 frames if nothing had stopped; a hundred
    // milliseconds of that was silent, so it has to be well under.
    const std::size_t count = bench.countOf(0x800);

    SCOPED_TRACE(::testing::Message() << count);
    EXPECT_TRUE(count >= 2);
    EXPECT_TRUE(count <= 16);
}
