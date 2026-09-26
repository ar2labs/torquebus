// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The block, rather than the protocol: that a request typed into a block's
// settings reaches the bus, that a bad one is refused while the block is on
// screen, and that the Events payload travels an edge like every other payload.

#include "core/diagnostics/DiagnosticEvent.h"
#include "core/isotp/IsoTpNode.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/trace/TraceStore.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] NodeDescription isotp(NodeParameters parameters)
{
    return NodeDescription{
        .id = "tp", .typeName = "isotp.transport", .parameters = std::move(parameters)};
}

} // namespace

TEST(IsoTpNodeTests, HexIsReadTheWayPeopleWriteIt)
{
    std::vector<std::uint8_t> bytes;

    ASSERT_TRUE(parseHexBytes("22 F1 90", bytes));
    EXPECT_TRUE((bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90}));

    ASSERT_TRUE(parseHexBytes("22f190", bytes));
    EXPECT_TRUE((bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90}));

    ASSERT_TRUE(parseHexBytes("0x22, 0xF1, 0x90", bytes));
    EXPECT_TRUE((bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90}));

    ASSERT_TRUE(parseHexBytes("22-F1-90", bytes));
    EXPECT_TRUE((bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90}));

    // An odd number of digits is the one mistake guessing cannot resolve:
    // "2 2F 19 0" and "22 F1 90" are different requests.
    EXPECT_FALSE(parseHexBytes("22 F1 9", bytes));
    EXPECT_FALSE(parseHexBytes("", bytes));
    EXPECT_FALSE(parseHexBytes("zz", bytes));
}

TEST(IsoTpNodeTests, AMessageIsShownAsBytesAndSaysWhenItWasCut)
{
    EXPECT_TRUE(toHexBytes({0x62, 0xF1, 0x90}) == "62 F1 90");

    const std::vector<std::uint8_t> long_(40, 0xAB);
    const std::string text = toHexBytes(long_, 4);

    EXPECT_TRUE(text.starts_with("AB AB AB AB"));

    // Said rather than silently truncated - a log line that stops mid-message
    // without saying so is one somebody will quote as the whole response.
    EXPECT_TRUE(text.find("40") != std::string::npos);
}

TEST(IsoTpNodeTests, ARequestTypedIntoTheBlockReachesTheBus)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore store{256};
    NodeBuildContext context;
    context.traceStore = &store;

    GraphDescription description;
    description.addNode(isotp({{"request", ParameterValue::fromText("22 F1 90")}}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"tp", 0, "trace", 0});

    ASSERT_TRUE(description.validate(catalog).succeeded());

    PipelineGraph graph;
    ASSERT_TRUE(description.build(catalog, context, graph).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    ASSERT_TRUE(store.size() == 1);

    const CanFrame& frame = store.row(0).frame;

    EXPECT_TRUE(frame.identifier == 0x7E0);
    EXPECT_TRUE(frame.data[0] == 0x03);
    EXPECT_TRUE(frame.data[1] == 0x22);
    EXPECT_TRUE(frame.data[3] == 0x90);
    EXPECT_TRUE(frame.length == 8); // Padded, which is what most ECUs need.
}

TEST(IsoTpNodeTests, AskedOnceMeansOnce)
{
    // An interval of zero is a question asked when the measurement begins and
    // not again - a tester that repeats a request nobody asked it to repeat is
    // one that fills a trace.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore store{256};
    NodeBuildContext context;
    context.traceStore = &store;

    GraphDescription description;
    description.addNode(isotp({{"request", ParameterValue::fromText("22 F1 90")}}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"tp", 0, "trace", 0});

    PipelineGraph graph;
    ASSERT_TRUE(description.build(catalog, context, graph).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    for (int pass = 0; pass < 10; ++pass) {
        graph.execute();
    }

    EXPECT_TRUE(store.size() == 1);
}

TEST(IsoTpNodeTests, ABlockWithNoRequestCarriesOnlyWhatItIsAskedTo)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore store{256};
    NodeBuildContext context;
    context.traceStore = &store;

    GraphDescription description;
    description.addNode(isotp({}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"tp", 0, "trace", 0});

    PipelineGraph graph;
    ASSERT_TRUE(description.build(catalog, context, graph).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();
    graph.execute();

    EXPECT_TRUE(store.empty());
}

TEST(IsoTpNodeTests, HalfAHexByteIsRefusedWhileTheBlockIsOnScreen)
{
    // At validate time, not at Start: the user is looking at the block they
    // just typed into.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(isotp({{"request", ParameterValue::fromText("22 F1 9")}}));

    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("hex") != std::string::npos);
}

TEST(IsoTpNodeTests, ASeparationTimeOutsideTheProtocolSRangeIsRefused)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(isotp({{"separationTimeMs", ParameterValue::fromInteger(200)}}));

    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("127") != std::string::npos);
}

TEST(IsoTpNodeTests, TheTransportSPortsAreTypedSoAWrongWireCannotBeDrawn)
{
    // Frames in and out, Events in and out. The graph refuses a signal wire
    // into a transport for the same reason it refuses one into a logger: the
    // type system is what makes "a decoder does not emit frames" enforceable
    // rather than aspirational.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* info = catalog.find("isotp.transport");
    ASSERT_TRUE(info != nullptr);

    ASSERT_TRUE(info->inputs.size() == 2);
    EXPECT_TRUE(info->inputs[0].type == PortType::Frames);
    EXPECT_TRUE(info->inputs[1].type == PortType::Events);

    ASSERT_TRUE(info->outputs.size() == 2);
    EXPECT_TRUE(info->outputs[0].type == PortType::Frames);
    EXPECT_TRUE(info->outputs[1].type == PortType::Events);

    GraphDescription description;
    description.addNode(isotp({}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});

    // Messages into a trace: the trace takes frames.
    description.addEdge(EdgeDescription{"tp", 1, "trace", 0});

    EXPECT_TRUE(description.validate(catalog).failed());
}
