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

#include <catch2/catch_test_macros.hpp>

#include "core/can/CanFrame.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A source that publishes a fixed list once, then nothing.
///
/// At namespace scope and not inside a TEST_CASE: MSVC rejects a static data
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

TEST_CASE("A script that will not compile fails the graph compile", "[lua][ecu]")
{
    LuaEcuNode node{"function on_message( end", "broken.lua"};

    const Result result = node.prepare(64);

    REQUIRE(result.failed());
    // The name and the line have to survive: "syntax error" alone sends the
    // user hunting through a file they just wrote.
    REQUIRE(std::string{result.message()}.find("broken.lua") != std::string::npos);
}

TEST_CASE("on_enable runs before any frame arrives", "[lua][ecu]")
{
    LuaEcuNode node{R"(
        started = false
        function on_enable() started = true end
        function on_message(id) if started then emit(0x100, "\1") end end
    )",
                    "enable.lua"};

    REQUIRE(node.prepare(64).succeeded());

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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();

    REQUIRE(graph.nodeAs<Collector>(sink)->frames.size() == 1);
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.front().identifier == 0x100U);
}

TEST_CASE("on_message receives identifier, payload and channel", "[lua][ecu]")
{
    PipelineGraph graph;

    const auto source = graph.addNode(std::make_unique<FixedSource>(
        std::vector<CanFrame>{frame(0x18FF50E5, {0xDE, 0xAD, 0xBE, 0xEF})}));

    // The script echoes what it was given back onto the bus, so what arrives at
    // the collector proves what the script saw. Reading the payload with
    // string.unpack is exactly how the cansim scripts do it.
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();

    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    REQUIRE(out.size() == 3);

    REQUIRE(out[0].identifier == 0x123U);
    REQUIRE(out[0].length == 4);
    REQUIRE(out[0].data[0] == 0xEF);
    REQUIRE(out[0].data[3] == 0xDE);

    REQUIRE(out[1].identifier == 0x124U); // the 29-bit identifier arrived as extended
    REQUIRE(out[2].identifier == 0x125U); // and four bytes arrived as four bytes
}

TEST_CASE("emit marks frames as transmitted on the node's channel", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    const CanFrame& out = graph.nodeAs<Collector>(sink)->frames.front();

    REQUIRE(out.channel == 3);
    REQUIRE(out.direction == CanDirection::Tx);
    REQUIRE(out.format == CanFrameFormat::Standard);
    REQUIRE(out.length == 2);
    // Nothing invented a bus timestamp: the frame has not been transmitted yet.
    REQUIRE(out.timestampNs == 0);
}

TEST_CASE("emit chooses the extended format when the identifier needs it", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].format == CanFrameFormat::Extended);
    REQUIRE(out[1].format == CanFrameFormat::Extended);
}

TEST_CASE("emit rejects a payload longer than the frame holds", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    // Nine bytes into a classic frame is a mistake, and it is reported as one
    // rather than truncated to eight - a silently shortened frame is a bug the
    // user then chases on the bus.
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.empty());
    REQUIRE(errors.size() == 1);
    REQUIRE(errors.front().find("at most 8") != std::string::npos);
}

TEST_CASE("a script that fails on every frame stops after the error limit", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    REQUIRE(ecuPointer->isFaulted());
    // Five failures reported, plus the one line that says it has stopped.
    REQUIRE(errors.size() == LuaEcuNode::kErrorLimit + 1);
    REQUIRE(errors.back().find("stopped") != std::string::npos);

    // A second pass changes nothing: the node is out, and the rest of the
    // measurement carries on around it.
    graph.execute();
    REQUIRE(errors.size() == LuaEcuNode::kErrorLimit + 1);
}

TEST_CASE("an occasional error does not stop the node", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    REQUIRE_FALSE(ecuPointer->isFaulted());
    REQUIRE(errors.size() == 1);
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.size() == 9);
}

TEST_CASE("log_message reaches the log handler and is not an error", "[lua][ecu]")
{
    std::vector<std::string> lines;
    bool sawError = false;

    auto ecuNode =
        std::make_unique<LuaEcuNode>(R"(function on_enable() log_message("ready") end)", "log.lua");
    ecuNode->setLogHandler([&](const std::string& text, bool isError) {
        lines.push_back(text);
        sawError = sawError || isError;
    });

    REQUIRE(ecuNode->prepare(64).succeeded());

    REQUIRE(lines.size() == 1);
    REQUIRE_FALSE(sawError);
    // Prefixed with the node name: an Output panel with ten ECUs in it is
    // useless if every line reads "ready".
    REQUIRE(lines.front() == "log.lua: ready");
}

TEST_CASE("the sandbox holds inside an ECU script", "[lua][ecu]")
{
    // Not a duplicate of the LuaRuntime test: that one proves the runtime does
    // not open the libraries, this one proves the ECU node uses that runtime
    // rather than opening its own.
    LuaEcuNode node{R"(
        escaped = (os ~= nil) or (io ~= nil) or (require ~= nil)
        function on_enable() if escaped then error("sandbox is open") end end
    )",
                    "sandbox.lua"};

    REQUIRE(node.prepare(64).succeeded());
}

TEST_CASE("get_time_us counts from the start of the measurement", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    const CanFrame& out = graph.nodeAs<Collector>(sink)->frames.front();
    REQUIRE(out.length == 4);

    // Small, because prepare() and execute() are microseconds apart. The point
    // is that it is elapsed time and not a Unix epoch, which would be enormous.
    const std::uint32_t microseconds = static_cast<std::uint32_t>(out.data[0])
                                       | (static_cast<std::uint32_t>(out.data[1]) << 8)
                                       | (static_cast<std::uint32_t>(out.data[2]) << 16)
                                       | (static_cast<std::uint32_t>(out.data[3]) << 24);

    REQUIRE(microseconds < 5'000'000U);
}

TEST_CASE("a script with no on_message is legal and silent", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    REQUIRE(graph.nodeAs<Collector>(sink)->frames.empty());
}

TEST_CASE("on_timer fires once the interval has elapsed", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());

    // Immediately: nothing yet. The timer starts at prepare(), so a 20 ms
    // interval must not fire on the first pass - an ECU that sends its first
    // cyclic message instantly would show a wrong gap in the trace.
    graph.execute();
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    graph.execute();
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.size() == 1);

    // And a pass right after it does not fire again: the node measures from the
    // last tick, not from the last pass.
    graph.execute();
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.size() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    graph.execute();
    REQUIRE(graph.nodeAs<Collector>(sink)->frames.size() == 2);
}

TEST_CASE("on_disable runs when the measurement ends", "[lua][ecu]")
{
    std::vector<std::string> lines;

    auto ecuNode = std::make_unique<LuaEcuNode>(
        R"(function on_disable() log_message("stopped cleanly") end)", "bye.lua");
    ecuNode->setLogHandler([&lines](const std::string& text, bool) { lines.push_back(text); });

    PipelineGraph graph;
    (void)graph.addNode(std::move(ecuNode));
    REQUIRE(graph.compile().succeeded());

    REQUIRE(lines.empty());
    graph.finish();
    REQUIRE(lines.size() == 1);
    REQUIRE(lines.front().find("stopped cleanly") != std::string::npos);
}

TEST_CASE("two ECUs on one bus each keep their own state", "[lua][ecu]")
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

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{first, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{second, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{first, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());
    graph.execute();

    // The first ECU counted 1 and 2 - not 1 and 3, which is what a shared
    // interpreter would have produced.
    const auto& out = graph.nodeAs<Collector>(sink)->frames;
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].identifier == 0x401U);
    REQUIRE(out[1].identifier == 0x402U);
}
