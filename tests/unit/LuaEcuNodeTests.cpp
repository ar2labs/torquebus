// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What these tests are for: a Lua ECU is the one node whose behaviour is
// written by the user, at runtime, in a language that cannot be type-checked
// ahead of time. Everything that can go wrong here goes wrong in the field, in
// somebody else's script, so the boundary between C++ and Lua is worth pinning
// down exactly: what a script receives, what it can send back, what happens
// when it is wrong, and what happens when it is wrong on every single frame.

#include <gtest/gtest.h>

#include "core/can/CanFrame.h"
#include "core/j1939/J1939Id.h"
#include "core/j1939/J1939Name.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/LuaRuntime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A source that publishes a fixed list once, then nothing.
///
/// At namespace scope and not inside a TEST: MSVC rejects a static data
/// member in a local class (C2246), and a PortDescriptor array has to be static
/// for the span to outlive the call.
class FixedSource final : public IPipelineNode {
public:
    explicit FixedSource(std::vector<CanFrame> frames)
        : m_frames{std::move(frames)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.source"; }
    [[nodiscard]] std::string displayName() const override { return "Fixed source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    void process(NodeContext& context) override
    {
        if (m_done) {
            return;
        }
        m_done = true;
        context.publish<CanFrame>(0, m_frames);
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<CanFrame> m_frames;
    bool m_done{false};
};

/// A source that publishes one list of frames per pass, in order, and nothing after them: a bus as
/// it is over time, for a test that needs something to arrive on the second pass and not the first.
class SequencedSource final : public IPipelineNode {
public:
    explicit SequencedSource(std::vector<std::vector<CanFrame>> passes)
        : m_passes{std::move(passes)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.sequence"; }
    [[nodiscard]] std::string displayName() const override { return "Sequenced source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    void process(NodeContext& context) override
    {
        if (m_pass < m_passes.size() && !m_passes[m_pass].empty()) {
            context.publish<CanFrame>(0, m_passes[m_pass]);
        }

        ++m_pass;
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<std::vector<CanFrame>> m_passes;
    std::size_t m_pass{0};
};

/// Collects everything that reaches it, for inspection after the fact.
class Collector final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.collect"; }
    [[nodiscard]] std::string displayName() const override { return "Collector"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
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
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::initializer_list<std::uint8_t> bytes)
{
    CanFrame result;
    result.identifier = identifier;
    // Format follows the identifier rather than defaulting to Standard: a test
    // helper that silently builds an invalid frame wastes an afternoon, as one
    // already did in this project.
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    result.length = static_cast<std::uint8_t>(bytes.size());
    result.dlc = dlcFromPayloadLength(result.length, false);

    std::size_t index = 0;
    for (std::uint8_t byte : bytes) {
        result.data[index++] = byte;
    }

    return result;
}

} // namespace

TEST(LuaEcuNodeTests, AScriptThatWillNotCompileFailsTheGraphCompile)
{
    LuaEcuNode node{"function on_message( end", "broken.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    // The name and the line have to survive: "syntax error" alone sends the
    // user hunting through a file they just wrote.
    ASSERT_TRUE(std::string{result.message()}.find("broken.lua") != std::string::npos);
}

TEST(LuaEcuNodeTests, OnEnableRunsBeforeAnyFrameArrives)
{
    LuaEcuNode node{R"(
        started = false
        function on_enable() started = true end
        function on_message(id) if started then emit(0x100, "\1") end end
    )",
                    "enable.lua"};

    ASSERT_TRUE(node.prepare(64).succeeded());

    PipelineGraph graph;
    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x200, {1})}));
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        started = false
        function on_enable() started = true end
        function on_message(id) if started then emit(0x100, "\1") end end
    )",
                                                                "enable.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.size() == 1);
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.front().identifier == 0x100U);
}

TEST(LuaEcuNodeTests, OnMessageReceivesIdentifierPayloadAndChannel)
{
    PipelineGraph graph;

    const auto source = graph.addNode(std::make_unique<FixedSource>(
        std::vector<CanFrame>{frame(0x18FF50E5, {0xDE, 0xAD, 0xBE, 0xEF})}));

    // The script echoes what it was given back onto the bus, so what arrives at
    // the collector proves what the script saw. Reading the payload with
    // string.unpack is how the example ECU scripts decode binary payloads.
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_message(id, data, channel, extended)
            local a, b, c, d = string.byte(data, 1, 4)
            emit(0x123, string.char(d, c, b, a))
            if extended == 1 then emit(0x124, "E") end
            if #data == 4 then emit(0x125, "4") end
        end
    )",
                                                                "echo.lua"));

    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    ASSERT_TRUE(out.size() == 3);

    ASSERT_TRUE(out[0].identifier == 0x123U);
    ASSERT_TRUE(out[0].length == 4);
    ASSERT_TRUE(out[0].data[0] == 0xEF);
    ASSERT_TRUE(out[0].data[3] == 0xDE);

    ASSERT_TRUE(out[1].identifier == 0x124U); // the 29-bit identifier arrived as extended
    ASSERT_TRUE(out[2].identifier == 0x125U); // and four bytes arrived as four bytes
}

TEST(LuaEcuNodeTests, EmitMarksFramesAsTransmittedOnTheNodeSChannel)
{
    PipelineGraph graph;

    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0})}));
    // std::uint8_t{3} and not a bare 3: make_unique forwards the argument as
    // int&&, so the narrowing to the channel's uint8_t happens inside <memory>
    // where the compiler can no longer see that the value fits. MSVC reports
    // C4242 at a line in <memory>, with this call site only in the
    // instantiation trace - noise in a warnings-as-errors CI run, and pointing
    // at the wrong file. Naming the type at the call site is also just honest
    // about what the parameter is.
    const auto ecu = graph.addNode(
        std::make_unique<LuaEcuNode>(R"(function on_message() emit(0x7FF, "\1\2") end)",
                                     "tx.lua",
                                     /*channel=*/std::uint8_t{3}));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const CanFrame& out = graph.nodeAs<Collector>(sink)->frames.front();

    ASSERT_TRUE(out.channel == 3);
    ASSERT_TRUE(out.direction == CanDirection::Tx);
    ASSERT_TRUE(out.format == CanFrameFormat::Standard);
    ASSERT_TRUE(out.length == 2);
    // Nothing invented a bus timestamp: the frame has not been transmitted yet.
    ASSERT_TRUE(out.timestampNs == 0);
}

TEST(LuaEcuNodeTests, EmitChoosesTheExtendedFormatWhenTheIdentifierNeedsIt)
{
    PipelineGraph graph;

    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0})}));
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_message()
            emit(0x18FEE500, "\1")             -- 29 bits: extended by itself
            emit(0x100, "\2", {extended=true}) -- 11 bits, asked for extended
        end
    )",
                                                                "fmt.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    ASSERT_TRUE(out.size() == 2);
    ASSERT_TRUE(out[0].format == CanFrameFormat::Extended);
    ASSERT_TRUE(out[1].format == CanFrameFormat::Extended);
}

TEST(LuaEcuNodeTests, EmitRejectsAPayloadLongerThanTheFrameHolds)
{
    std::vector<std::string> errors;

    auto ecuNode = std::make_unique<LuaEcuNode>(
        R"(function on_message() emit(0x100, "123456789") end)", "toolong.lua");
    ecuNode->setLogHandler([&errors](const std::string& text, bool isError) {
        if (isError) {
            errors.push_back(text);
        }
    });

    PipelineGraph graph;
    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0})}));
    const auto ecu = graph.addNode(std::move(ecuNode));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    // Nine bytes into a classic frame is a mistake, and it is reported as one
    // rather than truncated to eight - a silently shortened frame is a bug the
    // user then chases on the bus.
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.empty());
    ASSERT_TRUE(errors.size() == 1);
    ASSERT_TRUE(errors.front().find("at most 8") != std::string::npos);
}

TEST(LuaEcuNodeTests, AScriptThatFailsOnEveryFrameStopsAfterTheErrorLimit)
{
    std::vector<std::string> errors;

    auto ecuNode = std::make_unique<LuaEcuNode>(
        R"(function on_message() local t = nil; return t.x end)", "faulty.lua");
    ecuNode->setLogHandler([&errors](const std::string& text, bool isError) {
        if (isError) {
            errors.push_back(text);
        }
    });
    LuaEcuNode* ecuPointer = ecuNode.get();

    // Twenty frames, five allowed errors: without the limit this is twenty log
    // lines, and at bus speed it is a hundred and fifty thousand a second.
    std::vector<CanFrame> input;
    for (int index = 0; index < 20; ++index) {
        input.push_back(frame(0x100 + static_cast<std::uint32_t>(index), {1}));
    }

    PipelineGraph graph;
    const auto source = graph.addNode(std::make_unique<FixedSource>(std::move(input)));
    const auto ecu = graph.addNode(std::move(ecuNode));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    ASSERT_TRUE(ecuPointer->isFaulted());
    // Five failures reported, plus the one line that says it has stopped.
    ASSERT_TRUE(errors.size() == LuaEcuNode::kErrorLimit + 1);
    ASSERT_TRUE(errors.back().find("stopped") != std::string::npos);

    // A second pass changes nothing: the node is out, and the rest of the
    // measurement carries on around it.
    graph.execute();
    ASSERT_TRUE(errors.size() == LuaEcuNode::kErrorLimit + 1);
}

TEST(LuaEcuNodeTests, AnOccasionalErrorDoesNotStopTheNode)
{
    std::vector<std::string> errors;

    // Fails on the first frame only. A transient fault - a nil field in one
    // unusual message - should not take an ECU out of a two-hour recording.
    auto ecuNode = std::make_unique<LuaEcuNode>(R"(
        count = 0
        function on_message()
            count = count + 1
            if count == 1 then local t = nil; return t.x end
            emit(0x200, "\1")
        end
    )",
                                                "flaky.lua");
    ecuNode->setLogHandler([&errors](const std::string& text, bool isError) {
        if (isError) {
            errors.push_back(text);
        }
    });
    LuaEcuNode* ecuPointer = ecuNode.get();

    std::vector<CanFrame> input;
    for (int index = 0; index < 10; ++index) {
        input.push_back(frame(0x100, {1}));
    }

    PipelineGraph graph;
    const auto source = graph.addNode(std::make_unique<FixedSource>(std::move(input)));
    const auto ecu = graph.addNode(std::move(ecuNode));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    ASSERT_FALSE(ecuPointer->isFaulted());
    ASSERT_TRUE(errors.size() == 1);
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.size() == 9);
}

TEST(LuaEcuNodeTests, LogMessageReachesTheLogHandlerAndIsNotAnError)
{
    std::vector<std::string> lines;
    bool sawError = false;

    auto ecuNode =
        std::make_unique<LuaEcuNode>(R"(function on_enable() log_message("ready") end)", "log.lua");
    ecuNode->setLogHandler([&](const std::string& text, bool isError) {
        lines.push_back(text);
        sawError = sawError || isError;
    });

    ASSERT_TRUE(ecuNode->prepare(64).succeeded());

    ASSERT_TRUE(lines.size() == 1);
    ASSERT_FALSE(sawError);
    // Prefixed with the node name: an Output panel with ten ECUs in it is
    // useless if every line reads "ready".
    ASSERT_TRUE(lines.front() == "log.lua: ready");
}

TEST(LuaEcuNodeTests, TheSandboxHoldsInsideAnECUScript)
{
    // Not a duplicate of the LuaRuntime test: that one proves the runtime does
    // not open the libraries, this one proves the ECU node uses that runtime
    // rather than opening its own.
    LuaEcuNode node{R"(
        escaped = (os ~= nil) or (io ~= nil) or (require ~= nil)
        function on_enable() if escaped then error("sandbox is open") end end
    )",
                    "sandbox.lua"};

    ASSERT_TRUE(node.prepare(64).succeeded());
}

TEST(LuaEcuNodeTests, GetTimeUsCountsFromTheStartOfTheMeasurement)
{
    PipelineGraph graph;

    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0})}));
    // A timestamp on the same clock as the frames: emitted as a payload so the
    // test can read it back without a binding just for tests.
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_message()
            emit(0x300, string.pack("<I4", get_time_us() & 0xFFFFFFFF))
        end
    )",
                                                                "clock.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const CanFrame& out = graph.nodeAs<Collector>(sink)->frames.front();
    ASSERT_TRUE(out.length == 4);

    // Small, because prepare() and execute() are microseconds apart. The point
    // is that it is elapsed time and not a Unix epoch, which would be enormous.
    const std::uint32_t microseconds = static_cast<std::uint32_t>(out.data[0])
                                       | (static_cast<std::uint32_t>(out.data[1]) << 8)
                                       | (static_cast<std::uint32_t>(out.data[2]) << 16)
                                       | (static_cast<std::uint32_t>(out.data[3]) << 24);

    ASSERT_TRUE(microseconds < 5'000'000U);
}

TEST(LuaEcuNodeTests, AScriptWithNoOnMessageIsLegalAndSilent)
{
    // The pure producer: an ECU that only sends on a timer never looks at the
    // bus. Calling a function it does not have would be an error on every
    // frame, which is why prepare() caches what exists.
    PipelineGraph graph;

    const auto source =
        graph.addNode(std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0})}));
    const auto ecu = graph.addNode(
        std::make_unique<LuaEcuNode>(R"(function on_enable() set_timer(10) end)", "quiet.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.empty());
}

TEST(LuaEcuNodeTests, OnTimerFiresOnceTheIntervalHasElapsed)
{
    // The periodic ECU, which is most of them: a script that sends a cyclic
    // message every N milliseconds and never looks at the bus.
    PipelineGraph graph;

    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        ticks = 0
        function on_enable() set_timer(20) end
        function on_timer()
            ticks = ticks + 1
            emit(0x500, string.pack("<I2", ticks))
        end
    )",
                                                                "cyclic.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    // Immediately: nothing yet. The timer starts at prepare(), so a 20 ms
    // interval must not fire on the first pass - an ECU that sends its first
    // cyclic message instantly would show a wrong gap in the trace.
    graph.execute();
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    graph.execute();
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.size() == 1);

    // And a pass right after it does not fire again: the node measures from the
    // last tick, not from the last pass.
    graph.execute();
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.size() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    graph.execute();
    ASSERT_TRUE(graph.nodeAs<Collector>(sink)->frames.size() == 2);
}

TEST(LuaEcuNodeTests, OnDisableRunsWhenTheMeasurementEnds)
{
    std::vector<std::string> lines;

    auto ecuNode = std::make_unique<LuaEcuNode>(
        R"(function on_disable() log_message("stopped cleanly") end)", "bye.lua");
    ecuNode->setLogHandler([&lines](const std::string& text, bool) { lines.push_back(text); });

    PipelineGraph graph;
    (void)graph.addNode(std::move(ecuNode));
    ASSERT_TRUE(graph.compile().succeeded());

    ASSERT_TRUE(lines.empty());
    graph.finish();
    ASSERT_TRUE(lines.size() == 1);
    ASSERT_TRUE(lines.front().find("stopped cleanly") != std::string::npos);
}

TEST(LuaEcuNodeTests, TwoECUsOnOneBusEachKeepTheirOwnState)
{
    // One VM per ECU, and this is what that buys: both scripts use a global
    // named `count`, and neither can see the other's. Sharing one interpreter
    // would make this test fail, and would make two copies of the same script
    // impossible to run at once.
    PipelineGraph graph;

    const std::string script = R"(
        count = 0
        function on_message()
            count = count + 1
            emit(0x400 + count, "\1")
        end
    )";

    const auto source = graph.addNode(
        std::make_unique<FixedSource>(std::vector<CanFrame>{frame(0x001, {0}), frame(0x002, {0})}));
    const auto first = graph.addNode(std::make_unique<LuaEcuNode>(script, "a.lua"));
    const auto second = graph.addNode(std::make_unique<LuaEcuNode>(script, "b.lua"));
    const auto sink = graph.addNode(std::make_unique<Collector>());

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{first, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{second, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{first, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    // The first ECU counted 1 and 2 - not 1 and 3, which is what a shared
    // interpreter would have produced.
    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    ASSERT_TRUE(out.size() == 2);
    ASSERT_TRUE(out[0].identifier == 0x401U);
    ASSERT_TRUE(out[1].identifier == 0x402U);
}

TEST(LuaEcuNodeTests, CheckSyntaxAcceptsValidScriptAndDetectsSyntaxErrors)
{
    LuaRuntime runtime;
    (void)runtime.openLibraries();

    const char* validScript = R"(
        local x = 42
        function test()
            return x * 2
        end
    )";
    const Result validResult = runtime.checkSyntax(validScript, "valid.lua");
    EXPECT_TRUE(validResult.succeeded());

    const char* badScript = "local x = 42\nfunction test()\n    return x *\nend\n";
    const Result badResult = runtime.checkSyntax(badScript, "bad.lua");
    EXPECT_TRUE(badResult.failed());
    EXPECT_NE(badResult.message().find("bad.lua:4"), std::string::npos);
}

TEST(LuaEcuNodeTests, J1939HelpersAreAvailableAndEmitExtendedFrames)
{
    PipelineGraph graph;
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_enable()
            local speed_val = 100.0
            local speed_raw = raw(speed_val, 1 / 256, 0, 2)
            send(6, 0xFEF1, 0x00, string.pack("<I1I2", 0xF3, speed_raw))

            local diag_payload = dm1(1, 0, 0, 0, 110, 15, 1)
            send(6, 0xFECA, 0x00, diag_payload)
        end
    )",
                                                                "j1939_test.lua"));

    const auto collector = graph.addNode(std::make_unique<Collector>());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{collector, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const auto& out = graph.nodeAs<Collector>(collector)->frames;
    ASSERT_EQ(out.size(), 2U);

    // First frame: CCVS1 on 0x18FEF100, extended, 8 bytes
    EXPECT_EQ(out[0].identifier, 0x18FEF100U);
    EXPECT_TRUE(out[0].isExtended());
    EXPECT_EQ(out[0].length, 8U);
    EXPECT_EQ(out[0].data[0], 0xF3U);
    // 100 / (1/256) = 25600 = 0x6400 -> low byte 0x00, high byte 0x64
    EXPECT_EQ(out[0].data[1], 0x00U);
    EXPECT_EQ(out[0].data[2], 0x64U);
    // Trailing bytes padded with 0xFF
    EXPECT_EQ(out[0].data[3], 0xFFU);
    EXPECT_EQ(out[0].data[7], 0xFFU);

    // Second frame: DM1 on 0x18FECA00, extended, 8 bytes
    EXPECT_EQ(out[1].identifier, 0x18FECA00U);
    EXPECT_TRUE(out[1].isExtended());
    EXPECT_EQ(out[1].length, 8U);
}

TEST(LuaEcuNodeTests, RawSaturatesAtTheBiggestValidValueAndSaysNotAvailableForNothing)
{
    PipelineGraph graph;
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_enable()
            -- A missing reading, a reading that is not a number, one too big for its byte and one
            -- below what the byte can hold.
            emit(0x100, string.pack("<I1I1I1I1", raw(nil, 1, 0, 1), raw(0 / 0, 1, 0, 1),
                                    raw(1e9, 1, 0, 1), raw(-5, 1, 0, 1)))
            -- Three bytes, which had no entry in the table and saturated at a single byte's limit.
            emit(0x101, string.pack("<I3I2", raw(1e12, 1, 0, 3), raw(nil, 1, 0, 2)))
        end
    )",
                                                                "raw_test.lua"));

    const auto collector = graph.addNode(std::make_unique<Collector>());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{collector, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const auto& out = graph.nodeAs<Collector>(collector)->frames;
    ASSERT_EQ(out.size(), 2U);

    EXPECT_EQ(out[0].data[0], 0xFFU); // nil: not available
    EXPECT_EQ(out[0].data[1], 0xFFU); // NaN: the same, and not an error from string.pack
    EXPECT_EQ(out[0].data[2], 0xFAU); // too big: the biggest valid value, not a wrapped one
    EXPECT_EQ(out[0].data[3], 0x00U); // below: zero

    EXPECT_EQ(out[1].data[0], 0xFFU);
    EXPECT_EQ(out[1].data[1], 0xFFU);
    EXPECT_EQ(out[1].data[2], 0xFAU); // 0xFAFFFF
    EXPECT_EQ(out[1].data[3], 0xFFU); // not available, two bytes
    EXPECT_EQ(out[1].data[4], 0xFFU);
}

TEST(LuaEcuNodeTests, SendIsStillEmitInAnEcuAndTheJ1939FormIsAlwaysExtended)
{
    PipelineGraph graph;
    const auto ecu = graph.addNode(std::make_unique<LuaEcuNode>(R"(
        function on_enable()
            send(0x123, "\1\2")                       -- the old call
            send(0x123, "\1", { extended = true })    -- and with options
            j1939_send(0, 0, 5, "")                   -- an identifier of 5, which fits 11 bits
        end
    )",
                                                                "send_test.lua"));

    const auto collector = graph.addNode(std::make_unique<Collector>());
    ASSERT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{collector, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    const auto& out = graph.nodeAs<Collector>(collector)->frames;
    ASSERT_EQ(out.size(), 3U);

    EXPECT_EQ(out[0].identifier, 0x123U);
    EXPECT_FALSE(out[0].isExtended());
    EXPECT_EQ(out[0].length, 2U);

    EXPECT_EQ(out[1].identifier, 0x123U);
    EXPECT_TRUE(out[1].isExtended());

    // A J1939 message is a 29-bit message whatever its number: priority 0 from address 5 with PGN 0
    // is 0x00000005, and as a standard frame it would be a different message on a different bus.
    EXPECT_EQ(out[2].identifier, 5U);
    EXPECT_TRUE(out[2].isExtended());
    EXPECT_EQ(out[2].length, 8U);
    EXPECT_EQ(out[2].data[0], 0xFFU);
}

// ---------------------------------------------------------------------------
// What an ECU answers when the bus asks
// ---------------------------------------------------------------------------
//
// A request (PGN 59904) asks for a parameter group nobody is sending. The node answers it for the
// script, the way the network-management layer of a real ECU does: the latest copy of what the
// script sent for that group - an Address Claimed among the rest - for a request to everyone or to
// the address it was sent from, and for one to an address the ECU speaks from, for something it
// never sends, a negative acknowledgement. The claims of the bus, asked of everyone, come after the
// pseudo-random delay J1939-81 gives them. A script that wants something else defines on_request.

namespace {

/// Runs passes of `graph` until `done`, or two seconds have passed. An answer that waits for its
/// time arrives on a later pass, and a test that wants it has to be willing to run them.
[[nodiscard]] bool executeUntil(PipelineGraph& graph, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        graph.execute();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return true;
}

/// The frames among `frames` that are of the parameter group `pgn`, and from `source` when one is
/// named.
[[nodiscard]] std::vector<CanFrame> ofPgn(const std::vector<CanFrame>& frames,
                                          std::uint32_t pgn,
                                          std::optional<std::uint8_t> source = std::nullopt)
{
    std::vector<CanFrame> found;

    for (const CanFrame& frame : frames) {
        const std::optional<J1939Id> id = j1939Decompose(frame);

        if (id.has_value() && id->pgn() == pgn
            && (!source.has_value() || id->sourceAddress == *source)) {
            found.push_back(frame);
        }
    }

    return found;
}

/// An ECU on a bus: a script, what arrives on its input pass by pass, and what it put on its
/// output.
struct EcuOnABus final {
    EcuOnABus(const std::string& script,
              std::vector<std::vector<CanFrame>> arriving,
              std::uint8_t channel = 0U)
    {
        const NodeId source = graph.addNode(std::make_unique<SequencedSource>(std::move(arriving)));
        ecu = graph.addNode(std::make_unique<LuaEcuNode>(script, "ecu.lua", channel));
        sink = graph.addNode(std::make_unique<Collector>());

        EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
        EXPECT_TRUE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
        EXPECT_TRUE(graph.compile().succeeded());
    }

    /// Passes, one after the other, as fast as they come.
    void pass(int count = 1)
    {
        for (int index = 0; index < count; ++index) {
            graph.execute();
        }
    }

    /// Passes for as long as it takes an answer that waits to be due, and a little longer: for a
    /// test that says nothing more is coming.
    void passForAWhile()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};

        while (std::chrono::steady_clock::now() < until) {
            graph.execute();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    [[nodiscard]] const std::vector<CanFrame>& out()
    {
        return graph.nodeAs<Collector>(sink)->frames;
    }
    [[nodiscard]] LuaEcuNode& node() { return *graph.nodeAs<LuaEcuNode>(ecu); }

    [[nodiscard]] std::uint64_t statistic(std::string_view label)
    {
        for (const NodeStatistic& entry : node().statistics()) {
            if (entry.label == label) {
                return entry.value;
            }
        }

        return ~std::uint64_t{0};
    }

    PipelineGraph graph;
    NodeId ecu{NodeId::Invalid};
    NodeId sink{NodeId::Invalid};
};

/// An ECU at 0x21 that claims its address at power-up and says nothing else.
constexpr const char* kClaimer = R"(
    function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
)";

} // namespace

TEST(LuaEcuNodeTests, AnEcuThatClaimedAnAddressClaimsItAgainWhenTheBusAsksAndAfterTheDelayOfJ1939)
{
    EcuOnABus bus{kClaimer, {{}, {j1939RequestFrame(kPgnAddressClaimed)}}};

    // The claim at power-up, on a bus nobody has asked anything of yet.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 1U);
    const CanFrame atPowerUp = bus.out().front();
    EXPECT_EQ(atPowerUp.identifier, 0x18EEFF21U);

    const std::optional<J1939Name> name = j1939NameFromClaim(atPowerUp);
    ASSERT_TRUE(name.has_value());
    const auto delay = std::chrono::nanoseconds{j1939ClaimDelayNs(*name)};
    ASSERT_GT(delay.count(), 0);

    // The request is heard in the pass that has it, and its answer is not in that pass: asked of
    // everyone, ECUs do not all answer at once.
    const auto asked = std::chrono::steady_clock::now();
    bus.pass();
    EXPECT_EQ(bus.out().size(), 1U);

    // It comes, and not before it is due - and it is the same claim, with nothing in the script
    // asked to give it.
    ASSERT_TRUE(executeUntil(bus.graph, [&bus] { return bus.out().size() == 2U; }));
    EXPECT_GE(std::chrono::steady_clock::now() - asked, delay);

    EXPECT_EQ(bus.out()[1].identifier, atPowerUp.identifier);
    EXPECT_EQ(bus.out()[1].length, 8U);
    EXPECT_EQ(bus.out()[1].direction, CanDirection::Tx);
    EXPECT_EQ(std::memcmp(bus.out()[1].data.data(), atPowerUp.data.data(), 8U), 0);
}

TEST(LuaEcuNodeTests, AFloodOfRequestsForTheClaimsOfTheBusIsNotAQueueWithNoEnd)
{
    // Asked of everyone, a claim waits for its time. Asked a thousand times in one pass it must not
    // wait a thousand times over: the room there is to wait in is limited, and what is in it is
    // answered - and counted as answered, which what was left out is not.
    const std::vector<CanFrame> flood(1000U, j1939RequestFrame(kPgnAddressClaimed));
    EcuOnABus bus{kClaimer, {{}, flood}};

    bus.pass(2);
    bus.passForAWhile();

    const std::size_t claims = ofPgn(bus.out(), kPgnAddressClaimed).size();

    // The one at power-up, and answers for some of the requests - a few hundred, not none and not
    // all a thousand.
    EXPECT_GT(claims, 100U);
    EXPECT_LT(claims, 400U);
    EXPECT_EQ(bus.statistic("Requests answered"), claims - 1U);
}

TEST(LuaEcuNodeTests, AClaimRequestToTheAddressOfTheEcuIsAnsweredInTheSamePass)
{
    // Asked of this ECU and nobody else there is no crowd to spread out.
    EcuOnABus bus{kClaimer, {{}, {j1939RequestFrame(kPgnAddressClaimed, 0x21U)}}};

    bus.pass(2);

    ASSERT_EQ(bus.out().size(), 2U);
    EXPECT_EQ(bus.out()[1].identifier, 0x18EEFF21U);
}

TEST(LuaEcuNodeTests, ARequestToAnotherAddressIsLeftToTheEcuThatHoldsIt)
{
    EcuOnABus bus{kClaimer,
                  {{},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x55U)},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x21U)}}};

    bus.pass(); // power-up
    ASSERT_EQ(bus.out().size(), 1U);

    bus.pass(); // asked of 0x55, which is somebody else: nothing, not even a refusal
    EXPECT_EQ(bus.out().size(), 1U);

    bus.pass(); // asked of 0x21, which is this ECU
    EXPECT_EQ(bus.out().size(), 2U);
}

TEST(LuaEcuNodeTests, ARequestThatIsNotOneForSomethingItHasIsNotAnsweredWithAClaim)
{
    CanFrame shortRequest = j1939RequestFrame(kPgnAddressClaimed);
    shortRequest.length = 2U;
    shortRequest.dlc = 2U;

    // The three bytes of a request for the claim, in a frame that is not J1939.
    CanFrame standard = j1939RequestFrame(kPgnAddressClaimed);
    standard.format = CanFrameFormat::Standard;
    standard.identifier = 0x7EAU;

    // Another ECU's claim, which is the bus saying who is on it and not a question.
    CanFrame otherClaim{};
    otherClaim.identifier = 0x18EEFF22U;
    otherClaim.format = CanFrameFormat::Extended;
    otherClaim.length = 8U;
    otherClaim.dlc = 8U;

    EcuOnABus bus{kClaimer,
                  {{}, {j1939RequestFrame(kPgnDm1)}, {shortRequest}, {standard}, {otherClaim}}};

    bus.pass(5);
    bus.passForAWhile();

    // The claim at power-up and nothing after it: a request to everyone for something it does not
    // send is not worth a refusal from every ECU of the bus.
    EXPECT_EQ(bus.out().size(), 1U);
}

TEST(LuaEcuNodeTests, AScriptThatClaimedTwoAddressesAnswersForEachOfThemAfterItsOwnDelay)
{
    EcuOnABus bus{R"(
        function on_enable()
            j1939_claim(0x10, 0, 0, nil, 1, false)
            j1939_claim(0x11, 1, 0, nil, 1, false)
        end
    )",
                  {{}, {j1939RequestFrame(kPgnAddressClaimed)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 2U);

    // Asked of everyone: both, each when its own NAME says. Which comes first is the NAMEs'
    // business.
    bus.pass();
    ASSERT_TRUE(executeUntil(bus.graph, [&bus] { return bus.out().size() == 4U; }));

    EXPECT_EQ(ofPgn(bus.out(), kPgnAddressClaimed, std::uint8_t{0x10U}).size(), 2U);
    EXPECT_EQ(ofPgn(bus.out(), kPgnAddressClaimed, std::uint8_t{0x11U}).size(), 2U);
}

TEST(LuaEcuNodeTests, AClaimRequestToOneOfTwoAddressesIsAnsweredForThatOneOnly)
{
    EcuOnABus bus{R"(
        function on_enable()
            j1939_claim(0x10, 0, 0, nil, 1, false)
            j1939_claim(0x11, 1, 0, nil, 1, false)
        end
    )",
                  {{}, {j1939RequestFrame(kPgnAddressClaimed, 0x11U)}}};

    bus.pass(2);

    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[2].identifier, 0x18EEFF11U);
}

TEST(LuaEcuNodeTests, AnEcuThatNeverClaimedHasNothingToSayWhenAsked)
{
    // Talks on the bus, and has never said who it is - and a short "claim" is not one.
    EcuOnABus bus{R"(
        function on_enable()
            emit(0x123, "\1")
            emit(0x18EEFF21, "\1\2\3", { extended = true })
        end
    )",
                  {{}, {j1939RequestFrame(kPgnAddressClaimed)}}};

    bus.pass(2);
    bus.passForAWhile();

    EXPECT_EQ(bus.out().size(), 2U);
}

TEST(LuaEcuNodeTests, WhatAnEcuSaysWhenAskedIsTheLastClaimItMadeForTheAddress)
{
    // The function is the sixth byte of the NAME: 26 the first time, 41 the second.
    EcuOnABus bus{R"(
        function on_enable()
            j1939_claim(0x21, 26, 0, nil, 1, false)
            j1939_claim(0x21, 41, 0, nil, 1, false)
        end
    )",
                  {{}, {j1939RequestFrame(kPgnAddressClaimed)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 2U);
    EXPECT_EQ(bus.out()[0].data[5], 26U);
    EXPECT_EQ(bus.out()[1].data[5], 41U);

    // One answer, not two: the address has one claim, and it is the second.
    bus.pass();
    ASSERT_TRUE(executeUntil(bus.graph, [&bus] { return bus.out().size() >= 3U; }));
    bus.passForAWhile();

    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[2].data[5], 41U);
}

TEST(LuaEcuNodeTests, AReloadedScriptAnswersForItsOwnAddressesAndARefusedOneChangesNothing)
{
    EcuOnABus bus{kClaimer,
                  {{},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x22U)},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x21U)},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x22U)}}};

    bus.pass(); // power-up: 0x21
    ASSERT_EQ(bus.out().size(), 1U);

    // The new script claims another address, and is the one that answers for it: the old address is
    // nobody's any more, and a request to it is not this ECU's to answer.
    ASSERT_TRUE(bus.node()
                    .reload(R"(
        function on_enable() j1939_claim(0x22, 26, 0, nil, 1, false) end
    )")
                    .succeeded());
    bus.pass();
    ASSERT_EQ(bus.out().size(), 2U);
    EXPECT_EQ(bus.out()[1].identifier, 0x18EEFF22U);

    bus.pass();
    EXPECT_EQ(bus.out().size(), 2U);

    // A script that will not load leaves the running one alone - and what it answers for.
    ASSERT_TRUE(bus.node().reload("function on_enable( end").failed());
    bus.pass();
    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[2].identifier, 0x18EEFF22U);
}

TEST(LuaEcuNodeTests, AReloadThatIsRefusedLeavesTheHookOfTheScriptThatIsRunning)
{
    // A script that decides for itself - here, that nothing is answered - and a reload that will
    // not load: the one that is running is still the one that decides.
    EcuOnABus bus{R"(
        function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
        function on_request() return false end
    )",
                  {{},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x21U)},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x21U)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 1U);

    ASSERT_TRUE(bus.node().reload("function on_enable( end").failed());

    bus.pass(2);
    EXPECT_EQ(bus.out().size(), 1U);
}

TEST(LuaEcuNodeTests, AnEcuAnswersARequestForAnyGroupItSendsWithTheLatestCopy)
{
    // ET1 at 0x21, once at power-up and again whenever the bus says so: what a request is answered
    // with is what was sent last, which for a message that cycles is its present value.
    EcuOnABus bus{R"(
        function on_enable() j1939_send(6, 0xFEEE, 0x21, "\1") end
        function on_message(id, data)
            if id == 0x100 then j1939_send(6, 0xFEEE, 0x21, data) end
        end
    )",
                  {{},
                   {frame(0x100, {2})},
                   {j1939RequestFrame(0xFEEEU)},
                   {j1939RequestFrame(0xFEEEU, 0x21U)},
                   {j1939RequestFrame(0xFEEEU, 0x55U)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 1U);
    EXPECT_EQ(bus.out()[0].identifier, 0x18FEEE21U);
    EXPECT_EQ(bus.out()[0].data[0], 1U);

    bus.pass(); // the script changes what it sends
    ASSERT_EQ(bus.out().size(), 2U);
    EXPECT_EQ(bus.out()[1].data[0], 2U);

    bus.pass(); // asked of everyone: at once, and the present value
    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[2].identifier, 0x18FEEE21U);
    EXPECT_EQ(bus.out()[2].data[0], 2U);

    bus.pass(); // asked of this ECU
    ASSERT_EQ(bus.out().size(), 4U);
    EXPECT_EQ(bus.out()[3].data[0], 2U);

    bus.pass(); // asked of somebody else
    EXPECT_EQ(bus.out().size(), 4U);

    EXPECT_EQ(bus.statistic("Requests answered"), 2U);
    EXPECT_EQ(bus.statistic("Requests refused"), 0U);
}

TEST(LuaEcuNodeTests, AnEcuRemembersAFewHundredGroupsAndNotMoreThanThat)
{
    // A script that sends a new group every pass for ever must not make the node's memory grow for
    // ever. It keeps the groups it sent first, and answers a request for one of them; for one it
    // sent after the room was used it has nothing to say - which, asked of everyone, is silence.
    EcuOnABus bus{R"(
        function on_enable()
            for pgn = 0xFD00, 0xFD00 + 299 do j1939_send(6, pgn, 0x21, "\1") end
        end
    )",
                  {{}, {j1939RequestFrame(0xFD00U)}, {j1939RequestFrame(0xFD00U + 299U)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 300U);

    bus.pass();
    ASSERT_EQ(bus.out().size(), 301U);
    EXPECT_EQ(bus.out()[300].identifier, 0x18FD0021U);

    bus.pass();
    EXPECT_EQ(bus.out().size(), 301U);
    EXPECT_EQ(bus.statistic("Requests answered"), 1U);
}

TEST(LuaEcuNodeTests, ARequestToAnEcuForSomethingItNeverSendsIsRefusedWithANegativeAcknowledgement)
{
    EcuOnABus bus{kClaimer,
                  {{},
                   {j1939RequestFrame(kPgnDm2, 0x21U, 0xF9U)},
                   {j1939RequestFrame(kPgnDm2, 0x21U, kJ1939NullAddress)},
                   {j1939RequestFrame(kPgnDm2)},
                   {j1939RequestFrame(kPgnDm2, 0x55U, 0xF9U)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 1U);

    // The tool at 0xF9 asked this ECU for its DM2, and it has none: J1939-21 has it say so, to the
    // tool, with the address and the PGN it is answering at the end.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 2U);
    const CanFrame& refusal = bus.out()[1];
    EXPECT_EQ(refusal.identifier, 0x18E8F921U);
    EXPECT_EQ(refusal.length, 8U);

    const std::array<std::uint8_t, 8> expected{
        0x01U, 0xFFU, 0xFFU, 0xFFU, 0xF9U, 0xCBU, 0xFEU, 0x00U};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), refusal.data.begin()));

    EXPECT_EQ(j1939Acknowledgement(refusal),
              (std::optional<J1939Acknowledgement>{
                  J1939Acknowledgement{kJ1939AckNegative, 0xFFU, 0xF9U, kPgnDm2}}));

    // From the null address nobody is there to be told, so the refusal goes to everyone.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[2].identifier, 0x18E8FF21U);
    EXPECT_EQ(bus.out()[2].data[4], kJ1939NullAddress);

    // Asked of everyone, or of somebody else: not this ECU's to refuse.
    bus.pass(2);
    EXPECT_EQ(bus.out().size(), 3U);

    EXPECT_EQ(bus.statistic("Requests refused"), 2U);
    EXPECT_EQ(bus.statistic("Requests answered"), 0U);
}

TEST(LuaEcuNodeTests, AScriptThatDefinesOnRequestIsToldOfEveryRequestAndAnswersWhatItWants)
{
    // The script reports what it was told on 0x300, answers a request for its DM2 itself and says
    // that the node has nothing to add; anything else it leaves to the node.
    EcuOnABus bus{R"(
        function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
        function on_request(pgn, destination, source, channel)
            emit(0x300, string.pack("<I4BBB", pgn, destination, source, channel))
            if pgn == 0xFECB then
                j1939_send(6, 0xFECB, 0x21, j1939_dm1(0, 0, 0, 0, 0, 0, 0))
                return false
            end
        end
    )",
                  {{},
                   {j1939RequestFrame(kPgnDm2, 0x21U, 0xF9U, 3U)},
                   {j1939RequestFrame(0xFEDAU, 0x21U, 0xF9U)},
                   {j1939RequestFrame(0xFEDAU)}}};

    bus.pass();
    ASSERT_EQ(bus.out().size(), 1U);

    // Told: the group, to whom, from whom and on which channel. Answered: by the script, and so not
    // refused by the node.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 3U);
    EXPECT_EQ(bus.out()[1].identifier, 0x300U);
    EXPECT_EQ(bus.out()[1].data[0], 0xCBU);
    EXPECT_EQ(bus.out()[1].data[1], 0xFEU);
    EXPECT_EQ(bus.out()[1].data[4], 0x21U);
    EXPECT_EQ(bus.out()[1].data[5], 0xF9U);
    EXPECT_EQ(bus.out()[1].data[6], 3U);
    EXPECT_EQ(bus.out()[2].identifier, 0x18FECB21U);

    // Told, and the script returns nothing: the node refuses what it does not send.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 5U);
    EXPECT_EQ(bus.out()[3].identifier, 0x300U);
    EXPECT_EQ(bus.out()[4].identifier, 0x18E8F921U);

    // Told of one to everyone too, and nothing comes of it.
    bus.pass();
    ASSERT_EQ(bus.out().size(), 6U);
    EXPECT_EQ(bus.out()[5].identifier, 0x300U);
    EXPECT_EQ(bus.out()[5].data[4], kJ1939GlobalAddress);
}

TEST(LuaEcuNodeTests, AnEcuThatIgnoresRequestsIsOneWhoseOnRequestReturnsFalse)
{
    EcuOnABus bus{R"(
        function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
        function on_request() return false end
    )",
                  {{},
                   {j1939RequestFrame(kPgnAddressClaimed)},
                   {j1939RequestFrame(kPgnAddressClaimed, 0x21U)},
                   {j1939RequestFrame(kPgnDm2, 0x21U, 0xF9U)}}};

    bus.pass(4);
    bus.passForAWhile();

    // The claim at power-up: not an answer to anything, no refusal either.
    EXPECT_EQ(bus.out().size(), 1U);
    EXPECT_EQ(bus.statistic("Requests answered"), 0U);
}

TEST(LuaEcuNodeTests, AnOnRequestThatFailsLeavesTheNodeAnsweringAsItWould)
{
    EcuOnABus bus{R"(
        function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
        function on_request() error("boom") end
    )",
                  {{}, {j1939RequestFrame(kPgnAddressClaimed, 0x21U)}}};

    bus.pass(2);

    // Whoever asked meets the ECU that was configured, and not silence.
    ASSERT_EQ(bus.out().size(), 2U);
    EXPECT_EQ(bus.out()[1].identifier, 0x18EEFF21U);
    EXPECT_FALSE(bus.node().isFaulted());
}

TEST(LuaEcuNodeTests, AnOnRequestThatKeepsFailingStopsTheNodeLikeAnyCallbackThatDoes)
{
    const std::vector<CanFrame> asked{j1939RequestFrame(kPgnAddressClaimed, 0x21U)};
    EcuOnABus bus{R"(
        function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end
        function on_request() error("boom") end
    )",
                  {{}, asked, asked, asked, asked, asked}};

    // The claim at power-up and four requests, each answered as the node would have answered it
    // and each a failure of the hook: the script is still running.
    bus.pass(5);
    EXPECT_EQ(bus.out().size(), 5U);
    EXPECT_FALSE(bus.node().isFaulted());

    // The fifth in a row is the limit every callback has: the script is stopped, and a node that
    // has stopped does not answer the request that stopped it.
    bus.pass();
    EXPECT_TRUE(bus.node().isFaulted());
    EXPECT_EQ(bus.out().size(), 5U);
}

TEST(LuaEcuNodeTests, TheMessagesOfAConversationAreNotWhatARequestForThemIsAnsweredWith)
{
    // A request, an acknowledgement and the two halves of a transfer are what an ECU says to
    // somebody in the course of a conversation, and not parameter groups it has. Asked for one -
    // and it has sent one - the ECU does not send it again: it says that it has no such group.
    for (const std::uint32_t pgn :
         {kPgnRequest, kPgnAcknowledgement, kPgnTransportConnection, kPgnTransportData}) {
        EcuOnABus bus{std::string{R"(
            function on_enable()
                j1939_claim(0x21, 26, 0, nil, 1, false)
                j1939_send(6, )"}
                          + std::to_string(pgn) + std::string{R"(, 0x21, "\0\0\0\0\0\0\0\0")
            end
        )"},
                      {{}, {j1939RequestFrame(pgn, 0x21U, 0xF9U)}}};

        bus.pass(2);

        // The claim, the message of the conversation, and the refusal.
        ASSERT_EQ(bus.out().size(), 3U) << std::hex << pgn;
        EXPECT_EQ(bus.out()[2].identifier, 0x18E8F921U) << std::hex << pgn;

        const std::optional<J1939Acknowledgement> refusal = j1939Acknowledgement(bus.out()[2]);
        ASSERT_TRUE(refusal.has_value());
        EXPECT_EQ(refusal->control, kJ1939AckNegative);
        EXPECT_EQ(refusal->pgn, pgn);
    }
}

TEST(LuaEcuNodeTests, WhatAnEcuAnswersLeavesOnTheChannelItTransmitsOn)
{
    // An ECU on CAN 4 speaks on CAN 4 - what it said at power-up, its refusal, and what it says
    // again when it is asked.
    EcuOnABus bus{R"(
        function on_enable() j1939_send(6, 0xFEEE, 0x21, "\1") end
    )",
                  {{},
                   {j1939RequestFrame(kPgnDm2, 0x21U, 0xF9U)},
                   {j1939RequestFrame(0xFEEEU, 0x21U, 0xF9U)}},
                  3U};

    bus.pass(3);
    ASSERT_EQ(bus.out().size(), 3U);

    for (const CanFrame& frame : bus.out()) {
        EXPECT_EQ(frame.channel, 3U) << std::hex << frame.identifier;
    }

    EXPECT_EQ(bus.out()[1].identifier, 0x18E8F921U); // the refusal
    EXPECT_EQ(bus.out()[2].identifier, 0x18FEEE21U); // the answer
}

TEST(LuaEcuNodeTests, AnEcuDoesNotAnswerItsOwnRequestComingBackOffTheBus)
{
    // Asked for the claims of the bus by the ECU at 0x21 itself, which hears it come back.
    EcuOnABus bus{kClaimer,
                  {{}, {j1939RequestFrame(kPgnAddressClaimed, kJ1939GlobalAddress, 0x21U)}}};

    bus.pass(2);
    bus.passForAWhile();

    EXPECT_EQ(bus.out().size(), 1U);
}
