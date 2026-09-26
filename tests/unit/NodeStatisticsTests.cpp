// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Ten counters were being maintained on the frame path and shown nowhere. A
// decoder reporting that 100% of the traffic is outside its database is the
// single clearest sign that the wrong .dbc is loaded, and until this existed
// there was no way for anybody to see it.
//
// The reporting is a virtual on the node rather than a panel reaching in with a
// dynamic_cast per concrete type. These tests are mostly about that shape: a
// node says what it counted, in its own words, and nothing above it needs to
// know which type it was.

#include <gtest/gtest.h>

#include "core/database/DbcParser.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/DbcDecoderNode.h"
#include "core/pipeline/nodes/FrameNodes.h"

#include <memory>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// A source that publishes a fixed list of frames.
class StaticSource final : public IPipelineNode {
public:
    explicit StaticSource(std::vector<CanFrame> frames)
        : m_frames{std::move(frames)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.source"; }
    [[nodiscard]] std::string displayName() const override { return "static source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    void process(NodeContext& context) override
    {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_frames});
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<CanFrame> m_frames;
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier)
{
    CanFrame result;
    result.identifier = identifier;
    result.length = 2;
    result.dlc = 2;
    result.data[0] = 0x52;
    result.data[1] = 0x03;
    return result;
}

/// The value reported under `label`, or nothing.
[[nodiscard]] bool
statisticNamed(const IPipelineNode& node, const std::string& label, std::uint64_t& out)
{
    for (const NodeStatistic& statistic : node.statistics()) {
        if (statistic.label == label) {
            out = statistic.value;
            return true;
        }
    }
    return false;
}

} // namespace

TEST(NodeStatisticsTests, ANodeReportsItsCountersWithoutAnythingKnowingItsType)
{
    auto database = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parse(R"(
BO_ 257 VehicleSpeed: 8 ECU
 SG_ SpeedKmh : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECM
)",
                                 *database)
                    .succeeded());

    PipelineGraph graph;

    const NodeId source = graph.addNode(std::make_unique<StaticSource>(
        std::vector<CanFrame>{frame(0x101), frame(0x7FF), frame(0x300)}));

    auto decoderNode = std::make_unique<DbcDecoderNode>(std::move(database), "decoder");
    const NodeId decoder = graph.addNode(std::move(decoderNode));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{decoder, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    // Reached through the base interface, which is the point: a Statistics
    // panel never needs to have heard of DbcDecoderNode.
    const IPipelineNode* node = graph.node(decoder);
    ASSERT_TRUE(node != nullptr);

    std::uint64_t decoded = 0;
    std::uint64_t unknown = 0;
    ASSERT_TRUE(statisticNamed(*node, "Frames decoded", decoded));
    ASSERT_TRUE(statisticNamed(*node, "Frames not in the database", unknown));

    EXPECT_TRUE(decoded == 1);
    EXPECT_TRUE(unknown == 2);
}

TEST(NodeStatisticsTests, ANodeWithNothingToCountReportsNothing)
{
    // The default is an empty list, so a node type that has no numbers does not
    // have to say so - and the table above it can leave it out rather than
    // showing a name with no figures beside it.
    const StaticSource source{{}};

    EXPECT_TRUE(source.statistics().empty());
}

TEST(NodeStatisticsTests, EveryNodeInAGraphCanBeWalkedById)
{
    // What lets the engine take a snapshot without the graph handing out
    // pointers that a recompile would invalidate.
    PipelineGraph graph;

    const NodeId source =
        graph.addNode(std::make_unique<StaticSource>(std::vector<CanFrame>{frame(0x100)}));
    const NodeId sink =
        graph.addNode(std::make_unique<FrameSinkNode>([](std::span<const CanFrame>) { }));

    const std::vector<NodeId> ids = graph.nodeIds();

    ASSERT_TRUE(ids.size() == 2);
    EXPECT_TRUE(ids[0] == source);
    EXPECT_TRUE(ids[1] == sink);

    for (const NodeId id : ids) {
        EXPECT_TRUE(graph.node(id) != nullptr);
    }
}

TEST(NodeStatisticsTests, ASinkCountsWhatItDeliveredAndSaysSoInWords)
{
    // The label is part of the contract. Ten node types each abbreviating
    // differently is how a table of numbers becomes a table nobody reads.
    PipelineGraph graph;

    const NodeId source = graph.addNode(
        std::make_unique<StaticSource>(std::vector<CanFrame>{frame(0x100), frame(0x101)}));
    const NodeId sink =
        graph.addNode(std::make_unique<FrameSinkNode>([](std::span<const CanFrame>) { }));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{sink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    std::uint64_t delivered = 0;
    ASSERT_TRUE(statisticNamed(*graph.node(sink), "Frames delivered", delivered));
    EXPECT_TRUE(delivered == 2);
}
