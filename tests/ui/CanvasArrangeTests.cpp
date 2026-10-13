// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where a tidy diagram puts every block.
//
// The layout is a function of the graph, so it is tested as one: a graph in,
// positions out, no window. What is asserted is what a person would notice - the
// modules that speak are over the bus and the ones that listen are under it,
// nothing is on top of anything else, and arranging a diagram that is already
// arranged leaves it alone - and not the particular numbers, which are the
// layout's to change.

#include "core/pipeline/GraphDescription.h"
#include "ui/canvas/CanvasArrange.h"
#include "ui/canvas/CanvasLayout.h"

#include <gtest/gtest.h>

#include <QRectF>

#include <algorithm>
#include <map>
#include <string>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void addBlock(GraphDescription& graph, const char* id, const char* type, std::int64_t channel = -1)
{
    NodeDescription node;
    node.id = id;
    node.typeName = type;
    if (channel >= 0) {
        node.parameters.set("channel", ParameterValue::fromInteger(channel));
    }
    graph.addNode(std::move(node));
}

/// A module wired to a bus the way a drop is: listening, speaking, or both.
void drop(GraphDescription& graph, const char* bus, const char* module, bool listens, bool talks)
{
    if (listens) {
        graph.addEdge(EdgeDescription{bus, 0, module, 0});
    }
    if (talks) {
        graph.addEdge(EdgeDescription{module, 0, bus, 0});
    }
}

using Positions = std::map<std::string, QPointF>;

[[nodiscard]] Positions place(const GraphDescription& graph, const ArrangeOptions& options = {})
{
    Positions positions;
    for (const Placement& placement : layOutBlocks(graph, options)) {
        positions[placement.id] = QPointF{placement.x, placement.y};
    }
    return positions;
}

/// Writes the placements back, the way the canvas does.
void apply(GraphDescription& graph, const Positions& positions)
{
    for (NodeDescription& node : graph.nodes()) {
        node.x = positions.at(node.id).x();
        node.y = positions.at(node.id).y();
    }
}

[[nodiscard]] QRectF
rectOf(const NodeDescription& node, const Positions& positions, const ArrangeOptions& options = {})
{
    const QPointF at = positions.at(node.id);

    if (node.typeName == kCanBusType) {
        return QRectF{at.x(), at.y(), layout::kRailMinWidth, layout::kRailHeight};
    }
    if (options.expanded.contains(node.id)) {
        const double network = node.typeName == "tinyml.ecu"
                                   ? layout::kTinyMlCardGap + layout::kTinyMlCardHeight
                                   : 0.0;
        return QRectF{at.x(), at.y(), layout::kCardWidth, layout::kCardHeight + network};
    }
    return QRectF{at.x(), at.y(), layout::kChipWidth, layout::kChipHeight};
}

/// A bus with six ECUs on it, two listeners, and a plot under one of them - the shape of the J1939
/// example.
[[nodiscard]] GraphDescription truck()
{
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    for (const char* ecu : {"engine", "aftertreatment", "body", "brakes", "switches", "cluster"}) {
        addBlock(graph, ecu, "lua.ecu", 0);
        drop(graph, "can_1", ecu, true, true);
    }
    addBlock(graph, "tinyml", "tinyml.ecu", 0);
    drop(graph, "can_1", "tinyml", true, true);

    addBlock(graph, "decoder", "j1939.decoder");
    addBlock(graph, "filter", "can.filter");
    addBlock(graph, "plot", "signal.plot");
    drop(graph, "can_1", "decoder", true, false);
    drop(graph, "can_1", "filter", true, false);
    graph.addEdge(EdgeDescription{"decoder", 0, "plot", 0});
    return graph;
}

} // namespace

TEST(CanvasArrangeTests, ModulesThatSpeakHangOverTheBusAndOnesThatOnlyListenHangUnderIt)
{
    const GraphDescription graph = truck();
    const Positions at = place(graph);

    const double railY = at.at("can_1").y();

    for (const char* speaks : {"engine", "body", "cluster"}) {
        EXPECT_LT(at.at(speaks).y() + layout::kChipHeight, railY) << speaks;
    }
    for (const char* listens : {"decoder", "filter"}) {
        EXPECT_GT(at.at(listens).y(), railY + layout::kRailHeight) << listens;
    }
}

TEST(CanvasArrangeTests, ABlockThatSpeaksOnTheBusButFeedsSomethingHangsUnderIt)
{
    // Over the bus, what it feeds would be across the wires of the bus from it.
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    addBlock(graph, "tinyml", "tinyml.ecu", 0);
    addBlock(graph, "plot", "signal.plot");
    drop(graph, "can_1", "tinyml", true, true);
    graph.addEdge(EdgeDescription{"tinyml", 1, "plot", 0});

    const Positions at = place(graph);

    EXPECT_GT(at.at("tinyml").y(), at.at("can_1").y() + layout::kRailHeight);
    EXPECT_GT(at.at("plot").y(), at.at("tinyml").y() + layout::kChipHeight);
}

TEST(CanvasArrangeTests, TheBusIsAtTheLeftWithItsModulesBeginningAfterItsPlate)
{
    const Positions at = place(truck());

    EXPECT_EQ(at.at("can_1").x(), 0.0);

    // A rail may not be to the right of a module it carries: where the canvas holds it.
    for (const char* module : {"engine", "decoder", "filter", "tinyml"}) {
        EXPECT_GE(at.at(module).x(), layout::kRailPlateWidth + layout::kRailInset) << module;
    }
}

TEST(CanvasArrangeTests, ModulesInARowAreOneChipAndAGapApart)
{
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    for (const char* ecu : {"a", "b", "c", "d"}) {
        addBlock(graph, ecu, "lua.ecu", 0);
        drop(graph, "can_1", ecu, true, true);
    }

    const Positions at = place(graph);

    EXPECT_EQ(at.at("a").y(), at.at("d").y());
    EXPECT_NEAR(at.at("b").x() - at.at("a").x(), layout::kChipPitch, 0.01);
    EXPECT_NEAR(at.at("d").x() - at.at("c").x(), layout::kChipPitch, 0.01);
}

TEST(CanvasArrangeTests, ARowThatIsTooWideWrapsAndTheNextRowIsHalfAPitchIn)
{
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    for (const char* ecu : {"a", "b", "c", "d", "e"}) {
        addBlock(graph, ecu, "lua.ecu", 0);
        drop(graph, "can_1", ecu, true, true);
    }

    ArrangeOptions options;
    options.maxPerRow = 3;
    const Positions at = place(graph, options);

    // Three on the row by the bus, two on the next one out - over the first.
    EXPECT_EQ(at.at("a").y(), at.at("c").y());
    EXPECT_LT(at.at("d").y(), at.at("a").y());
    EXPECT_EQ(at.at("d").y(), at.at("e").y());

    // Half a pitch in, so that a drop from the outer row goes between two blocks of the inner one.
    EXPECT_NEAR(at.at("d").x() - at.at("a").x(), layout::kChipPitch * 0.5, 0.01);
}

TEST(CanvasArrangeTests, WhatAModuleFeedsHangsUnderItWhereItIs)
{
    const Positions at = place(truck());

    EXPECT_GT(at.at("plot").y(), at.at("decoder").y() + layout::kChipHeight);
    EXPECT_NEAR(at.at("plot").x(), at.at("decoder").x(), 0.01);
}

TEST(CanvasArrangeTests, ArrangingADiagramThatIsAlreadyArrangedLeavesItAlone)
{
    // Wrapping included: a second row half a pitch in must not be shuffled into the first by the
    // order the blocks are in now.
    for (const int perRow : {8, 3}) {
        GraphDescription graph = truck();
        ArrangeOptions options;
        options.maxPerRow = perRow;

        const Positions first = place(graph, options);
        apply(graph, first);
        const Positions second = place(graph, options);

        EXPECT_EQ(first, second) << "with " << perRow << " to a row";
    }
}

TEST(CanvasArrangeTests, TheOrderTheModulesAreInIsTheOrderTheyAreLaidOutIn)
{
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    for (const char* ecu : {"a", "b", "c"}) {
        addBlock(graph, ecu, "lua.ecu", 0);
        drop(graph, "can_1", ecu, true, true);
    }

    // The person has them c, a, b, left to right.
    graph.nodes()[3].x = 0.0; // c is the last one added, put first
    graph.nodes()[1].x = 300.0;
    graph.nodes()[2].x = 600.0;

    const Positions at = place(graph);

    EXPECT_LT(at.at("c").x(), at.at("a").x());
    EXPECT_LT(at.at("a").x(), at.at("b").x());
}

TEST(CanvasArrangeTests, BlocksOnNoBusAreLaidOutBySourceAndDepth)
{
    // A replay into a trace is not on any bus: the source over what it feeds.
    GraphDescription graph;
    addBlock(graph, "replay", "log.source");
    addBlock(graph, "trace", "trace.sink");
    graph.addEdge(EdgeDescription{"replay", 0, "trace", 0});

    const Positions at = place(graph);

    EXPECT_GT(at.at("trace").y(), at.at("replay").y() + layout::kChipHeight);
    EXPECT_NEAR(at.at("trace").x(), at.at("replay").x(), 0.01);
}

TEST(CanvasArrangeTests, TwoBusesAreStackedByChannelAndNothingOfOneIsOnTheOther)
{
    GraphDescription graph;
    // CAN 2 listed first: the order on the page is the channel's, not the graph's.
    addBlock(graph, "can_2", "can.bus", 1);
    addBlock(graph, "can_1", "can.bus", 0);
    addBlock(graph, "second", "lua.ecu", 1);
    addBlock(graph, "first", "lua.ecu", 0);
    drop(graph, "can_2", "second", true, true);
    drop(graph, "can_1", "first", true, true);

    const Positions at = place(graph);

    EXPECT_LT(at.at("can_1").y(), at.at("can_2").y());
    EXPECT_LT(at.at("first").y(), at.at("can_1").y());
    EXPECT_LT(at.at("second").y(), at.at("can_2").y());
    // The second band starts below the whole of the first.
    EXPECT_GT(at.at("second").y(), at.at("can_1").y() + layout::kRailHeight);
}

TEST(CanvasArrangeTests, ACardTakesItsOwnWidthAndTheNextBlockInTheRowAllowsForIt)
{
    GraphDescription graph;
    addBlock(graph, "can_1", "can.bus", 0);
    for (const char* ecu : {"a", "b"}) {
        addBlock(graph, ecu, "lua.ecu", 0);
        drop(graph, "can_1", ecu, true, true);
    }

    ArrangeOptions options;
    options.expanded.insert("a");

    const Positions at = place(graph, options);

    EXPECT_NEAR(at.at("b").x() - at.at("a").x(), layout::kCardWidth + layout::kChipGap, 0.01);
}

TEST(CanvasArrangeTests, NoBlockIsLaidOnAnother)
{
    // Every block of a mixed graph, with and without cards: no two rectangles meet.
    for (const bool cards : {false, true}) {
        GraphDescription graph = truck();
        addBlock(graph, "replay", "log.source");
        addBlock(graph, "trace", "trace.sink");
        graph.addEdge(EdgeDescription{"replay", 0, "trace", 0});

        ArrangeOptions options;
        options.maxPerRow = 4;
        if (cards) {
            for (const NodeDescription& node : graph.nodes()) {
                if (node.typeName != kCanBusType) {
                    options.expanded.insert(node.id);
                }
            }
        }

        const Positions at = place(graph, options);
        ASSERT_EQ(at.size(), graph.nodes().size());

        const auto& nodes = graph.nodes();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (std::size_t j = i + 1; j < nodes.size(); ++j) {
                const QRectF a = rectOf(nodes[i], at, options);
                const QRectF b = rectOf(nodes[j], at, options);

                // A bus is as long as what is on it; what is checked is that no module is on its
                // plate or its wires, which is the height.
                EXPECT_FALSE(a.intersects(b))
                    << nodes[i].id << " and " << nodes[j].id << (cards ? " (cards)" : "");
            }
        }
    }
}

TEST(CanvasArrangeTests, EveryBlockOfTheGraphGetsAPlaceExactlyOnce)
{
    const GraphDescription graph = truck();
    const std::vector<Placement> placements = layOutBlocks(graph);

    ASSERT_EQ(placements.size(), graph.nodes().size());
    for (std::size_t i = 0; i < placements.size(); ++i) {
        EXPECT_EQ(placements[i].id, graph.nodes()[i].id);
    }
}

TEST(CanvasArrangeTests, AGraphThatLoopsStillGetsEveryBlockPlaced)
{
    // Not a pipeline - it could not be compiled - but a hand-edited file can hold one, and
    // arranging it must not hang.
    GraphDescription graph;
    addBlock(graph, "a", "can.filter");
    addBlock(graph, "b", "can.filter");
    graph.addEdge(EdgeDescription{"a", 0, "b", 0});
    graph.addEdge(EdgeDescription{"b", 0, "a", 0});

    EXPECT_EQ(layOutBlocks(graph).size(), 2U);
}

TEST(CanvasArrangeTests, AnEmptyGraphHasNothingToPlace)
{
    EXPECT_TRUE(layOutBlocks(GraphDescription{}).empty());
}
