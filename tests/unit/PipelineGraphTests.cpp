// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The graph's own rules, tested without any hardware or engine in the way.

#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameNodes.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// A node with configurable ports, so a test can build any shape it needs.
class TestNode final : public IPipelineNode {
public:
    TestNode(std::string name,
             std::vector<PortDescriptor> inputs,
             std::vector<PortDescriptor> outputs)
        : m_name{std::move(name)}
        , m_inputs{std::move(inputs)}
        , m_outputs{std::move(outputs)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return m_name; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return m_inputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return m_outputs;
    }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override
    {
        prepared = true;
        preparedWith = maximumBatchSize;
        return prepareResult;
    }

    void process(NodeContext& context) override
    {
        ++passes;
        if (order != nullptr) {
            order->push_back(m_name);
        }

        // Forward whatever arrived, so a chain of these carries data end to end.
        const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
        if (!incoming.empty() && !m_outputs.empty()) {
            m_buffer.assign(incoming.begin(), incoming.end());
            context.publish<CanFrame>(0, m_buffer);
            lastSeen = incoming.size();
        }
    }

    void finish() override
    {
        finished = true;
        if (order != nullptr) {
            order->push_back(m_name + ":finish");
        }
    }

    int passes{0};
    std::size_t lastSeen{0};
    bool prepared{false};
    bool finished{false};
    std::size_t preparedWith{0};
    Result prepareResult{Result::ok()};
    std::vector<std::string>* order{nullptr};

private:
    std::string m_name;
    std::vector<PortDescriptor> m_inputs;
    std::vector<PortDescriptor> m_outputs;
    std::vector<CanFrame> m_buffer;
};

std::unique_ptr<TestNode>
makeNode(std::string name, std::vector<PortDescriptor> inputs, std::vector<PortDescriptor> outputs)
{
    return std::make_unique<TestNode>(std::move(name), std::move(inputs), std::move(outputs));
}

const PortDescriptor kFramesPort{"frames", PortType::Frames};
const PortDescriptor kSignalsPort{"signals", PortType::Signals};

/// Emits a fixed batch every pass.
///
/// At namespace scope rather than inside the test bodies that use it: a class
/// defined inside a function may not have static data members, and kPorts is
/// one. GCC accepts it anyway; MSVC is right to refuse.
class FixedSource final : public IPipelineNode {
public:
    explicit FixedSource(std::span<const CanFrame> frames)
        : m_frames{frames}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "fixed"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    void process(NodeContext& context) override { context.publish<CanFrame>(0, m_frames); }

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::span<const CanFrame> m_frames;
};

} // namespace

TEST(PipelineGraphTests, AnEmptyGraphRefusesToCompile)
{
    PipelineGraph graph;
    EXPECT_TRUE(graph.compile().failed());
    EXPECT_FALSE(graph.isCompiled());
}

TEST(PipelineGraphTests, ConnectingMismatchedPortTypesIsRefused)
{
    // The whole reason ports carry a type: the mistake is caught while drawing
    // the wire, not three layers downstream as an empty panel.
    PipelineGraph graph;

    const NodeId decoder = graph.addNode(makeNode("decoder", {kFramesPort}, {kSignalsPort}));
    const NodeId trace = graph.addNode(makeNode("trace", {kFramesPort}, {}));

    const Result result = graph.connect(PortRef{decoder, 0}, PortRef{trace, 0});

    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::InvalidArgument);

    // The message has to name both ends, or it is useless on a canvas.
    const std::string message{result.message()};
    EXPECT_TRUE(message.find("decoder") != std::string::npos);
    EXPECT_TRUE(message.find("trace") != std::string::npos);
    EXPECT_TRUE(message.find("Signals") != std::string::npos);
    EXPECT_TRUE(message.find("Frames") != std::string::npos);
}

TEST(PipelineGraphTests, MatchingPortTypesConnect)
{
    PipelineGraph graph;

    const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
    const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));

    EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
    EXPECT_TRUE(graph.edges().size() == 1);
    EXPECT_TRUE(graph.compile().succeeded());
}

TEST(PipelineGraphTests, OutOfRangePortsAreRefused)
{
    PipelineGraph graph;

    const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
    const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));

    EXPECT_TRUE(graph.connect(PortRef{source, 5}, PortRef{sink, 0}).failed());
    EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 5}).failed());
    EXPECT_TRUE(graph.edges().empty());
}

TEST(PipelineGraphTests, AnInputTakesExactlyOneEdge)
{
    // Fan-in would need a merge policy - interleave? concatenate? by timestamp?
    // Picking one silently is worse than making the user place a merge node.
    PipelineGraph graph;

    const NodeId first = graph.addNode(makeNode("first", {}, {kFramesPort}));
    const NodeId second = graph.addNode(makeNode("second", {}, {kFramesPort}));
    const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));

    EXPECT_TRUE(graph.connect(PortRef{first, 0}, PortRef{sink, 0}).succeeded());

    const Result second_attempt = graph.connect(PortRef{second, 0}, PortRef{sink, 0});
    EXPECT_TRUE(second_attempt.failed());
    EXPECT_TRUE(second_attempt.code() == ErrorCode::InvalidState);
    EXPECT_TRUE(std::string{second_attempt.message()}.find("merge") != std::string::npos);
}

TEST(PipelineGraphTests, FanOutIsUnrestricted)
{
    PipelineGraph graph;

    const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
    const NodeId trace = graph.addNode(makeNode("trace", {kFramesPort}, {}));
    const NodeId logger = graph.addNode(makeNode("logger", {kFramesPort}, {}));
    const NodeId plot = graph.addNode(makeNode("plot", {kFramesPort}, {}));

    EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{trace, 0}).succeeded());
    EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{logger, 0}).succeeded());
    EXPECT_TRUE(graph.connect(PortRef{source, 0}, PortRef{plot, 0}).succeeded());
    EXPECT_TRUE(graph.compile().succeeded());
}

TEST(PipelineGraphTests, ACycleIsDetectedAndNamed)
{
    // A dataflow graph with a loop has no valid evaluation order. Catching it
    // at compile time is what lets the canvas refuse the closing wire.
    PipelineGraph graph;

    const NodeId a = graph.addNode(makeNode("alpha", {kFramesPort}, {kFramesPort}));
    const NodeId b = graph.addNode(makeNode("beta", {kFramesPort}, {kFramesPort}));

    ASSERT_TRUE(graph.connect(PortRef{a, 0}, PortRef{b, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{b, 0}, PortRef{a, 0}).succeeded());

    const Result result = graph.compile();
    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::InvalidState);
    EXPECT_TRUE(std::string{result.message()}.find("cycle") != std::string::npos);
    EXPECT_FALSE(graph.isCompiled());
}

TEST(PipelineGraphTests, NodesRunInTopologicalOrder)
{
    PipelineGraph graph;

    auto sourceNode = makeNode("source", {}, {kFramesPort});
    auto middleNode = makeNode("middle", {kFramesPort}, {kFramesPort});
    auto sinkNode = makeNode("sink", {kFramesPort}, {});

    std::vector<std::string> order;
    sourceNode->order = &order;
    middleNode->order = &order;
    sinkNode->order = &order;

    // Added deliberately out of order, so passing this cannot be an accident of
    // insertion order.
    const NodeId sink = graph.addNode(std::move(sinkNode));
    const NodeId middle = graph.addNode(std::move(middleNode));
    const NodeId source = graph.addNode(std::move(sourceNode));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{middle, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{middle, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    ASSERT_TRUE(order.size() == 3);
    EXPECT_TRUE(order[0] == "source");
    EXPECT_TRUE(order[1] == "middle");
    EXPECT_TRUE(order[2] == "sink");
}

TEST(PipelineGraphTests, FinishRunsInReverseSoConsumersTearDownFirst)
{
    PipelineGraph graph;

    auto sourceNode = makeNode("source", {}, {kFramesPort});
    auto sinkNode = makeNode("sink", {kFramesPort}, {});

    std::vector<std::string> order;
    sourceNode->order = &order;
    sinkNode->order = &order;

    const NodeId source = graph.addNode(std::move(sourceNode));
    const NodeId sink = graph.addNode(std::move(sinkNode));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.finish();

    ASSERT_TRUE(order.size() == 2);
    EXPECT_TRUE(order[0] == "sink:finish");
    EXPECT_TRUE(order[1] == "source:finish");
}

TEST(PipelineGraphTests, EveryNodeIsPreparedWithTheBatchSizeItWillSee)
{
    PipelineGraph graph;

    auto sourceNode = makeNode("source", {}, {kFramesPort});
    auto sinkNode = makeNode("sink", {kFramesPort}, {});

    TestNode* sourceRaw = sourceNode.get();
    TestNode* sinkRaw = sinkNode.get();

    const NodeId source = graph.addNode(std::move(sourceNode));
    const NodeId sink = graph.addNode(std::move(sinkNode));
    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());

    ASSERT_TRUE(graph.compile(512).succeeded());

    EXPECT_TRUE(sourceRaw->prepared);
    EXPECT_TRUE(sinkRaw->prepared);
    EXPECT_TRUE(sourceRaw->preparedWith == 512);
    EXPECT_TRUE(sinkRaw->preparedWith == 512);
}

TEST(PipelineGraphTests, ANodeThatFailsToPrepareAbortsTheCompile)
{
    // Far better to hear about a missing DBC file or a broken Lua chunk here
    // than on the first frame of a measurement.
    PipelineGraph graph;

    auto badNode = makeNode("broken", {}, {kFramesPort});
    badNode->prepareResult = Result::error(ErrorCode::FileNotFound, "no such database");

    const NodeId bad = graph.addNode(std::move(badNode));
    (void)bad;

    const Result result = graph.compile();
    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::FileNotFound);

    const std::string message{result.message()};
    EXPECT_TRUE(message.find("broken") != std::string::npos);
    EXPECT_TRUE(message.find("no such database") != std::string::npos);
    EXPECT_FALSE(graph.isCompiled());
}

TEST(PipelineGraphTests, EditingTheGraphInvalidatesTheCompile)
{
    {
        PipelineGraph graph;
        const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
        const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));
        ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
        ASSERT_TRUE(graph.compile().succeeded());
        ASSERT_TRUE(graph.isCompiled());

        (void)graph.addNode(makeNode("extra", {kFramesPort}, {}));
        EXPECT_FALSE(graph.isCompiled());
    }

    {
        PipelineGraph graph;
        const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
        const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));
        ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
        ASSERT_TRUE(graph.compile().succeeded());
        ASSERT_TRUE(graph.isCompiled());

        graph.removeNode(sink);
        EXPECT_FALSE(graph.isCompiled());
        EXPECT_TRUE(graph.edges().empty()); // its edge went with it
    }

    {
        PipelineGraph graph;
        const NodeId source = graph.addNode(makeNode("source", {}, {kFramesPort}));
        const NodeId sink = graph.addNode(makeNode("sink", {kFramesPort}, {}));
        ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
        ASSERT_TRUE(graph.compile().succeeded());
        ASSERT_TRUE(graph.isCompiled());

        graph.disconnect(PortRef{sink, 0});
        EXPECT_FALSE(graph.isCompiled());
    }
}

TEST(PipelineGraphTests, AnUncompiledGraphDoesNothingRatherThanMisbehaving)
{
    PipelineGraph graph;

    auto node = makeNode("node", {}, {kFramesPort});
    TestNode* raw = node.get();
    (void)graph.addNode(std::move(node));

    graph.execute(); // not compiled

    EXPECT_TRUE(raw->passes == 0);
}

TEST(PipelineGraphTests, ADisconnectedInputSeesAnEmptyBatchNotStaleData)
{
    PipelineGraph graph;

    auto node = makeNode("lonely", {kFramesPort}, {});
    TestNode* raw = node.get();
    (void)graph.addNode(std::move(node));

    ASSERT_TRUE(graph.compile().succeeded());
    graph.execute();

    EXPECT_TRUE(raw->passes == 1);
    EXPECT_TRUE(raw->lastSeen == 0);
}

TEST(PipelineGraphTests, BatchesFlowAlongTheEdges)
{
    // The executor's actual job: what one node emits is what the next one sees.
    PipelineGraph graph;

    std::vector<CanFrame> produced(4);
    for (std::uint32_t index = 0; index < produced.size(); ++index) {
        produced[index].identifier = 0x100 + index;
        produced[index].length = 8;
        produced[index].dlc = 8;
    }

    std::vector<CanFrame> received;

    const NodeId source = graph.addNode(std::make_unique<FixedSource>(produced));
    const NodeId sink = graph.addNode(std::make_unique<FrameSinkNode>(
        [&received](std::span<const CanFrame> batch) {
            received.assign(batch.begin(), batch.end());
        },
        "collector"));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    ASSERT_TRUE(received.size() == 4);
    EXPECT_TRUE(received[0].identifier == 0x100);
    EXPECT_TRUE(received[3].identifier == 0x103);
}

TEST(PipelineGraphTests, AFilterNodeShapesOneBranchWithoutAffectingTheOthers)
{
    // This is what a graph buys over a fixed sink list: "trace everything, but
    // only plot these identifiers" in one picture.
    PipelineGraph graph;

    std::vector<CanFrame> produced(4);
    for (std::uint32_t index = 0; index < produced.size(); ++index) {
        produced[index].identifier = 0x100 + index;
        produced[index].length = 1;
        produced[index].dlc = 1;
    }

    std::size_t traced = 0;
    std::size_t plotted = 0;

    CanFilterSet onlyOne;
    onlyOne.add(CanFilter::acceptIdentifier(0x102));

    const NodeId source = graph.addNode(std::make_unique<FixedSource>(produced));
    const NodeId trace = graph.addNode(std::make_unique<FrameSinkNode>(
        [&traced](std::span<const CanFrame> batch) { traced += batch.size(); }, "trace"));
    const NodeId filter = graph.addNode(std::make_unique<FrameFilterNode>(onlyOne));
    const NodeId plot = graph.addNode(std::make_unique<FrameSinkNode>(
        [&plotted](std::span<const CanFrame> batch) { plotted += batch.size(); }, "plot"));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{trace, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{filter, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{filter, 0}, PortRef{plot, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    EXPECT_TRUE(traced == 4); // the trace branch is untouched
    EXPECT_TRUE(plotted == 1); // the plot branch sees only 0x102
}

TEST(PipelineGraphTests, AMisTypedBatchReadsAsEmptyRatherThanAsReinterpretedBytes)
{
    // Second line of defence. The graph should have refused the connection long
    // before this, but if a payload type ever reaches the wrong port, starving
    // the node is the only safe failure.
    std::vector<CanFrame> frames(2);
    const PortBatch batch{std::span<const CanFrame>{frames}};

    EXPECT_TRUE(batch.type() == PortType::Frames);
    EXPECT_TRUE(batch.size() == 2);
    EXPECT_TRUE(batch.as<CanFrame>().size() == 2);

    const PortBatch empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(empty.as<CanFrame>().empty());
}
