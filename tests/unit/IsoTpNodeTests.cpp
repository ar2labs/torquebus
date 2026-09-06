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

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] NodeDescription isotp(NodeParameters parameters)
{
    return NodeDescription{.id = "tp", .typeName = "isotp.transport",
                           .parameters = std::move(parameters)};
}

} // namespace

TEST_CASE("Hex is read the way people write it", "[isotp][hex]")
{
    std::vector<std::uint8_t> bytes;

    REQUIRE(parseHexBytes("22 F1 90", bytes));
    CHECK(bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});

    REQUIRE(parseHexBytes("22f190", bytes));
    CHECK(bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});

    REQUIRE(parseHexBytes("0x22, 0xF1, 0x90", bytes));
    CHECK(bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});

    REQUIRE(parseHexBytes("22-F1-90", bytes));
    CHECK(bytes == std::vector<std::uint8_t>{0x22, 0xF1, 0x90});

    // An odd number of digits is the one mistake guessing cannot resolve:
    // "2 2F 19 0" and "22 F1 90" are different requests.
    CHECK_FALSE(parseHexBytes("22 F1 9", bytes));
    CHECK_FALSE(parseHexBytes("", bytes));
    CHECK_FALSE(parseHexBytes("zz", bytes));
}

TEST_CASE("A message is shown as bytes, and says when it was cut", "[isotp][hex]")
{
    CHECK(toHexBytes({0x62, 0xF1, 0x90}) == "62 F1 90");

    const std::vector<std::uint8_t> long_(40, 0xAB);
    const std::string text = toHexBytes(long_, 4);

    CHECK(text.starts_with("AB AB AB AB"));

    // Said rather than silently truncated - a log line that stops mid-message
    // without saying so is one somebody will quote as the whole response.
    CHECK(text.find("40") != std::string::npos);
}

TEST_CASE("A request typed into the block reaches the bus", "[isotp][graph]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore store{256};
    NodeBuildContext context;
    context.traceStore = &store;

    GraphDescription description;
    description.addNode(isotp({{"request", ParameterValue::fromText("22 F1 90")}}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"tp", 0, "trace", 0});

    REQUIRE(description.validate(catalog).succeeded());

    PipelineGraph graph;
    REQUIRE(description.build(catalog, context, graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();

    REQUIRE(store.size() == 1);

    const CanFrame& frame = store.row(0).frame;

    CHECK(frame.identifier == 0x7E0);
    CHECK(frame.data[0] == 0x03);
    CHECK(frame.data[1] == 0x22);
    CHECK(frame.data[3] == 0x90);
    CHECK(frame.length == 8); // Padded, which is what most ECUs need.
}

TEST_CASE("Asked once means once", "[isotp][graph]")
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
    REQUIRE(description.build(catalog, context, graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    for (int pass = 0; pass < 10; ++pass) {
        graph.execute();
    }

    CHECK(store.size() == 1);
}

TEST_CASE("A block with no request carries only what it is asked to",
          "[isotp][graph]")
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
    REQUIRE(description.build(catalog, context, graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();
    graph.execute();

    CHECK(store.empty());
}

TEST_CASE("Half a hex byte is refused while the block is on screen",
          "[isotp][validate]")
{
    // At validate time, not at Start: the user is looking at the block they
    // just typed into.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(isotp({{"request", ParameterValue::fromText("22 F1 9")}}));

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("hex") != std::string::npos);
}

TEST_CASE("A separation time outside the protocol's range is refused",
          "[isotp][validate]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(isotp({{"separationTimeMs", ParameterValue::fromInteger(200)}}));

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("127") != std::string::npos);
}

TEST_CASE("The transport's ports are typed, so a wrong wire cannot be drawn",
          "[isotp][graph]")
{
    // Frames in and out, Events in and out. The graph refuses a signal wire
    // into a transport for the same reason it refuses one into a logger: the
    // type system is what makes "a decoder does not emit frames" enforceable
    // rather than aspirational.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* info = catalog.find("isotp.transport");
    REQUIRE(info != nullptr);

    REQUIRE(info->inputs.size() == 2);
    CHECK(info->inputs[0].type == PortType::Frames);
    CHECK(info->inputs[1].type == PortType::Events);

    REQUIRE(info->outputs.size() == 2);
    CHECK(info->outputs[0].type == PortType::Frames);
    CHECK(info->outputs[1].type == PortType::Events);

    GraphDescription description;
    description.addNode(isotp({}));
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});

    // Messages into a trace: the trace takes frames.
    description.addEdge(EdgeDescription{"tp", 1, "trace", 0});

    CHECK(description.validate(catalog).failed());
}
