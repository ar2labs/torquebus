// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Two things a simulation needs once it is convincing: a script that can *ask*
// about the bus rather than only react to what is wired into it, and a script
// that can deliberately be wrong.
//
// The second is the one worth being careful about. A tool that only ever sends
// correct traffic tests half of a receiver: the half that works. What a stuck
// counter, a stale checksum or a DLC that lies do to the rest of the network is
// the question a bench exists to answer, and until now the only way to ask it
// was to unplug something.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/trace/TraceStore.h"

#include <gtest/gtest.h>

#include <chrono>
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

/// A scripted ECU with a trace it can ask about, and everything it sent kept.
struct Bench final {
    TraceStore trace{256};
    PipelineGraph graph;

    LuaEcuNode* ecu{nullptr};
    CollectNode* collector{nullptr};

    explicit Bench(const std::string& source, bool withTrace = true)
    {
        auto node = std::make_unique<LuaEcuNode>(source, "bus.lua");
        ecu = node.get();

        if (withTrace) {
            ecu->setTraceStore(&trace);
        }

        const NodeId ecuId = graph.addNode(std::move(node));

        auto sink = std::make_unique<CollectNode>();
        collector = sink.get();
        const NodeId sinkId = graph.addNode(std::move(sink));

        EXPECT_TRUE(graph.connect(PortRef{ecuId, 0}, PortRef{sinkId, 0}).succeeded());
        EXPECT_TRUE(graph.compile().succeeded());
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

    /// Puts a frame into the trace, as a measurement would.
    void seen(std::uint32_t identifier, std::vector<std::uint8_t> payload, std::uint64_t timeNs)
    {
        CanFrame frame;
        frame.identifier = identifier;
        frame.timestampNs = timeNs;
        frame.length = static_cast<std::uint8_t>(payload.size());
        frame.dlc = frame.length;

        std::copy(payload.begin(), payload.end(), frame.data.begin());

        trace.append(std::span<const CanFrame>{&frame, 1});
    }

    [[nodiscard]] std::vector<CanFrame> framesOf(std::uint32_t identifier) const
    {
        std::vector<CanFrame> found;

        for (const CanFrame& frame : collector->frames) {
            if (frame.identifier == identifier) {
                found.push_back(frame);
            }
        }

        return found;
    }
};

} // namespace

TEST(LuaBusAndFaultTests, AScriptCanAskWhatAMessageLastCarried)
{
    // The question a script could not ask before: not "what arrived on my
    // input this pass", but "what is the bus doing".
    Bench bench{R"(
        function on_enable()
            every(5, function()
                local data, info = bus_last(0x123)

                if data then
                    emit(0x200, data)
                    log_message("cycle " .. info.cycle_us .. " count " .. info.count)
                end
            end)
        end
    )"};

    bench.seen(0x123, {0x11, 0x22}, 0);
    bench.seen(0x123, {0x33, 0x44}, 10'000'000);

    bench.run(40);

    const std::vector<CanFrame> echoed = bench.framesOf(0x200);

    ASSERT_FALSE(echoed.empty());
    EXPECT_TRUE(echoed.front().length == 2);
    EXPECT_TRUE(echoed.front().data[0] == 0x33);
    EXPECT_TRUE(echoed.front().data[1] == 0x44);
}

TEST(LuaBusAndFaultTests, AMessageNobodyHasSentYetReadsAsNothing)
{
    // Nil rather than empty bytes: "no frame" and "a frame with no payload" are
    // different, and a script writing `if data then` has to be able to tell.
    Bench bench{R"(
        seen_nothing = false

        function on_enable()
            every(5, function()
                if bus_last(0x999) == nil then
                    seen_nothing = true
                    emit(0x201, "\1")
                end
            end)
        end
    )"};

    bench.run(30);

    EXPECT_FALSE(bench.framesOf(0x201).empty());
}

TEST(LuaBusAndFaultTests, AScriptCanReadTheMeasurementSOwnCounters)
{
    Bench bench{R"(
        function on_enable()
            every(5, function()
                local stats = bus_stats()
                emit(0x202, string.char(stats.identifiers, stats.frames))
            end)
        end
    )"};

    bench.seen(0x100, {0x01}, 0);
    bench.seen(0x101, {0x02}, 1'000'000);
    bench.seen(0x100, {0x03}, 2'000'000);

    bench.run(30);

    const std::vector<CanFrame> reported = bench.framesOf(0x202);

    ASSERT_FALSE(reported.empty());
    EXPECT_TRUE(reported.back().data[0] == 2); // two identifiers
    EXPECT_TRUE(reported.back().data[1] == 3); // three frames
}

TEST(LuaBusAndFaultTests, AskingAboutTheBusWithoutATraceSaysSo)
{
    // Rather than answering zero, which a script would believe. Built without
    // a graph, because a script whose on_enable fails makes compile() fail -
    // which is the rule, and not what this case is about.
    LuaEcuNode node{R"(
        function on_enable()
            bus_last(0x123)
        end
    )",
                    "bus.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("bus_last") != std::string::npos);
}

TEST(LuaBusAndFaultTests, AFrozenMessageRepeatsItselfAStuckECU)
{
    // The counter stops and the checksum goes stale, and nothing in the node
    // had to know which byte was which - which is the only way to do this
    // without a database describing the message.
    Bench bench{R"(
        local counter = tb.counter(4)

        function on_enable()
            cyclic(0x300, 5, function()
                return string.char(counter(), 0xAA)
            end)

            every(5, function()
                if tb.now() > 0.04 then
                    fault(0x300, { freeze = true })
                end
            end)
        end
    )"};

    bench.run(140);

    const std::vector<CanFrame> frames = bench.framesOf(0x300);
    ASSERT_TRUE(frames.size() >= 8);

    // The last few are all the same byte, where the first few were not.
    const std::uint8_t frozen = frames.back().data[0];

    std::size_t sameAsLast = 0;
    for (const CanFrame& frame : frames) {
        if (frame.data[0] == frozen) {
            ++sameAsLast;
        }
    }

    // More than a wrapping counter would ever produce on its own.
    EXPECT_TRUE(sameAsLast > frames.size() / 4);
}

TEST(LuaBusAndFaultTests, ADLCCanBeMadeToLieAboutThePayload)
{
    // A frame carrying three bytes and claiming eight. A receiver either
    // tolerates that or does not, and finding out is the point.
    Bench bench{R"(
        function on_enable()
            cyclic(0x310, 5, "\1\2\3")
            fault(0x310, { dlc = 8 })
        end
    )"};

    bench.run(40);

    const std::vector<CanFrame> frames = bench.framesOf(0x310);
    ASSERT_FALSE(frames.empty());

    EXPECT_TRUE(frames.front().length == 3);
    EXPECT_TRUE(frames.front().dlc == 8);
}

TEST(LuaBusAndFaultTests, BitsCanBeFlippedOnTheWayOut)
{
    // A checksum byte flipped here is a message that arrives looking valid and
    // checksums wrong - which is a different failure from a message that does
    // not arrive, and the receiver treats them differently or should.
    Bench bench{R"(
        function on_enable()
            cyclic(0x320, 5, "\x00\xF0")
            fault(0x320, { flip = { [1] = 0xFF, [2] = 0x0F } })
        end
    )"};

    bench.run(40);

    const std::vector<CanFrame> frames = bench.framesOf(0x320);
    ASSERT_FALSE(frames.empty());

    EXPECT_TRUE(frames.front().data[0] == 0xFF);
    EXPECT_TRUE(frames.front().data[1] == 0xFF);
}

TEST(LuaBusAndFaultTests, AFaultCanBeSwitchedOffAgain)
{
    // The likeliest reason a later measurement makes no sense is an injected
    // fault left switched on, so turning one off has to be as easy as turning
    // it on.
    Bench bench{R"(
        function on_enable()
            cyclic(0x330, 5, "\1")
            fault(0x330, { flip = { [1] = 0xFF } })

            every(5, function()
                if tb.now() > 0.04 then
                    fault(0x330)
                end
            end)
        end
    )"};

    bench.run(120);

    const std::vector<CanFrame> frames = bench.framesOf(0x330);
    ASSERT_TRUE(frames.size() >= 6);

    EXPECT_TRUE(frames.front().data[0] == 0xFE);
    EXPECT_TRUE(frames.back().data[0] == 0x01);
}

TEST(LuaBusAndFaultTests, TruncationSendsFewerBytesThanTheMessageHas)
{
    Bench bench{R"(
        function on_enable()
            cyclic(0x340, 5, "\1\2\3\4\5\6\7\8")
            fault(0x340, { truncate = 2 })
        end
    )"};

    bench.run(40);

    const std::vector<CanFrame> frames = bench.framesOf(0x340);
    ASSERT_FALSE(frames.empty());

    EXPECT_TRUE(frames.front().length == 2);
    EXPECT_TRUE(frames.front().data[0] == 0x01);
}

TEST(LuaBusAndFaultTests, TheCRCHelperComputesWhatAReceiverWillCheck)
{
    // SAE J1850: polynomial 0x1D, init 0xFF, final XOR 0xFF. Checked against
    // values computed independently rather than against itself - a checksum
    // that agrees only with its own implementation is not a checksum.
    Bench bench{R"(
        function on_enable()
            emit(0x350, string.char(
                tb.crc8("\x00"),
                tb.crc8("\x01\x02\x03\x04"),
                tb.crc8("\xFF\xFF\xFF\xFF\xFF\xFF\xFF")))
        end
    )"};

    bench.run(20);

    const std::vector<CanFrame> frames = bench.framesOf(0x350);
    ASSERT_FALSE(frames.empty());

    // Reference values computed independently - the algorithm's published
    // check value for "123456789" is 0x4B, and the implementation that
    // produced these agrees with it. Numbers taken from the code under test
    // would only prove it agrees with itself.
    EXPECT_TRUE(frames.front().data[0] == 0x3B);
    EXPECT_TRUE(frames.front().data[1] == 0x67);
    EXPECT_TRUE(frames.front().data[2] == 0x34);
}

TEST(LuaBusAndFaultTests, TheE2EHelperPutsTheChecksumAndCounterWhereTheyBelong)
{
    Bench bench{R"(
        function on_enable()
            emit(0x360, tb.e2e("\xAA\xBB", 3))
        end
    )"};

    bench.run(20);

    const std::vector<CanFrame> frames = bench.framesOf(0x360);
    ASSERT_FALSE(frames.empty());

    const CanFrame& frame = frames.front();

    ASSERT_TRUE(frame.length == 4);
    EXPECT_TRUE(frame.data[1] == 0x03); // counter in the low nibble of byte two
    EXPECT_TRUE(frame.data[2] == 0xAA);
    EXPECT_TRUE(frame.data[3] == 0xBB);

    // And the checksum is over everything after it.
    const std::vector<std::uint8_t> body{frame.data[1], frame.data[2], frame.data[3]};

    std::uint8_t crc = 0xFF;
    for (const std::uint8_t byte : body) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) != 0 ? static_cast<std::uint8_t>((crc << 1) ^ 0x1D)
                                    : static_cast<std::uint8_t>(crc << 1);
        }
    }

    EXPECT_TRUE(frame.data[0] == static_cast<std::uint8_t>(crc ^ 0xFF));
}
