// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The description is what a canvas edits and a project file stores, so almost
// every test here is about a message. A graph that will not build has to say
// which node, which port, and what was wrong in terms the user recognises -
// they are looking at blocks with names on them, not at C++ types.

#include <catch2/catch_test_macros.hpp>

#include "core/can/CanEngine.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/trace/TraceStore.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A build context with a trace but no channels, for graphs that need neither.
[[nodiscard]] NodeBuildContext contextWith(TraceStore& store)
{
    NodeBuildContext context;
    context.traceStore = &store;
    return context;
}

[[nodiscard]] NodeDescription node(std::string id, std::string type, NodeParameters parameters = {})
{
    return NodeDescription{.id = std::move(id),
                           .typeName = std::move(type),
                           .parameters = std::move(parameters),
                           .x = 0.0,
                           .y = 0.0};
}

} // namespace

TEST_CASE("The catalog declares ports before anything is instantiated", "[graph][catalog]")
{
    // This is what a node palette draws from: it must be able to show that a
    // CAN Channel has one Frames output without an engine, a channel, or a
    // measurement in sight.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* source = catalog.find("can.source");
    REQUIRE(source != nullptr);
    CHECK(source->displayName == "CAN Channel");
    CHECK(source->category == "Sources");
    CHECK(source->inputs.empty());
    REQUIRE(source->outputs.size() == 1);
    CHECK(source->outputs.front().type == PortType::Frames);

    const NodeTypeInfo* ecu = catalog.find("lua.ecu");
    REQUIRE(ecu != nullptr);
    CHECK(ecu->inputs.size() == 1);
    CHECK(ecu->outputs.size() == 1);

    // And the parameter list is what the properties panel builds its fields
    // from, so a new node type needs no UI change to become editable.
    CHECK_FALSE(ecu->parameters.empty());
}

TEST_CASE("An unknown type names what it was given and what exists", "[graph][catalog]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("trace_1", "can.trace")); // The type is trace.sink.

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    const std::string message{result.message()};
    CHECK(message.find("trace_1") != std::string::npos);
    CHECK(message.find("can.trace") != std::string::npos);
}

TEST_CASE("Mismatched port types name both ends", "[graph][validation]")
{
    // Both ends and both types: "cannot connect" alone leaves the user to work
    // out which of the two blocks is the wrong one.
    NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    catalog.registerType(
        NodeTypeInfo{.typeName = "test.signals",
                     .displayName = "Signals",
                     .category = "Test",
                     .description = {},
                     .inputs = {PortDescriptor{"signals", PortType::Signals}},
                     .outputs = {},
                     .parameters = {}},
        [](const NodeParameters&, const NodeBuildContext&, std::string_view,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            out = nullptr;
            return Result::error(ErrorCode::NotImplemented, "test node");
        });

    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addNode(node("plot", "test.signals"));
    description.addEdge(EdgeDescription{"can_1", 0, "plot", 0});

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    const std::string message{result.message()};
    CHECK(message.find("can_1") != std::string::npos);
    CHECK(message.find("plot") != std::string::npos);
    CHECK(message.find("Frames") != std::string::npos);
    CHECK(message.find("Signals") != std::string::npos);
}

TEST_CASE("A port that does not exist is caught before building", "[graph][validation]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"can_1", 3, "trace_1", 0});

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("port 3") != std::string::npos);
}

TEST_CASE("An edge to a node that is not there is caught", "[graph][validation]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addEdge(EdgeDescription{"can_1", 0, "trace_1", 0});

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("trace_1") != std::string::npos);
}

TEST_CASE("Two wires into one input are refused", "[graph][validation]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addNode(node("can_2", "can.source"));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"can_1", 0, "trace_1", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "trace_1", 0});

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    // The message says why, not just no: merging needs a policy, and the user
    // should learn that here rather than from a forum post.
    CHECK(std::string{result.message()}.find("merging") != std::string::npos);
}

TEST_CASE("Duplicate ids are refused", "[graph][validation]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("ecu", "trace.sink"));
    description.addNode(node("ecu", "trace.sink"));

    const Result result = description.validate(catalog);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("'ecu'") != std::string::npos);
}

TEST_CASE("Removing a node removes the wires that touched it", "[graph]")
{
    // The one inconsistency a description must never be able to hold: an edge
    // pointing at something that is gone fails at build time, long after the
    // deletion that caused it.
    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addNode(node("ecu", "lua.ecu"));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"can_1", 0, "ecu", 0});
    description.addEdge(EdgeDescription{"ecu", 0, "trace_1", 0});

    description.removeNode("ecu");

    CHECK(description.nodes().size() == 2);
    CHECK(description.edges().empty());
    CHECK(description.find("ecu") == nullptr);
}

TEST_CASE("uniqueId numbers from the second one", "[graph]")
{
    GraphDescription description;

    CHECK(description.uniqueId("ecu") == "ecu");

    description.addNode(node("ecu", "lua.ecu"));
    CHECK(description.uniqueId("ecu") == "ecu_2");

    description.addNode(node("ecu_2", "lua.ecu"));
    CHECK(description.uniqueId("ecu") == "ecu_3");
}

TEST_CASE("A described graph builds, compiles and runs", "[graph][build]")
{
    // The whole point, end to end: a graph made of data - the shape a project
    // file holds - becomes a running pipeline that moves frames.
    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(node("can_1", "can.source",
                             NodeParameters{{"channel", ParameterValue::fromInteger(0)}}));
    description.addNode(
        node("ecu", "lua.ecu",
             NodeParameters{
                 {"script",
                  ParameterValue::fromText(
                      R"(function on_message(id) if id == 0x100 then emit(0x101, "\1") end end)")},
             }));
    description.addNode(node("tx", "can.transmit",
                             NodeParameters{{"channel", ParameterValue::fromInteger(0)}}));

    description.addEdge(EdgeDescription{"can_1", 0, "ecu", 0});
    description.addEdge(EdgeDescription{"ecu", 0, "tx", 0});

    REQUIRE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());

    std::vector<CanFrame> received;
    std::mutex mutex;
    engine.addFrameSink([&](std::span<const CanFrame> batch) {
        const std::lock_guard lock{mutex};
        received.insert(received.end(), batch.begin(), batch.end());
    });

    engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());

    REQUIRE(engine.start().succeeded());

    CanFrame request;
    request.identifier = 0x100;
    request.length = 1;
    request.dlc = 1;
    REQUIRE(engine.transmit(0, request).succeeded());

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    engine.stop();

    const std::lock_guard lock{mutex};
    const auto answers = std::count_if(received.begin(), received.end(),
                                       [](const CanFrame& f) { return f.identifier == 0x101; });
    CHECK(answers >= 1);
}

TEST_CASE("Two source nodes on one channel both see every frame", "[graph][build]")
{
    // The regression this pins: draining consumes, so a described graph with
    // its own can.source for CAN 1 used to answer nothing - the engine's
    // default trace source had already taken the batch. Whichever node ran
    // first won, silently, and which one that was depended on the order the
    // user happened to add blocks in.
    //
    // The engine now drains once per pass and every source publishes the same
    // view. Two ECUs wired to two separate CAN 1 blocks must both answer.
    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    const NodeParameters channelZero{{"channel", ParameterValue::fromInteger(0)}};

    GraphDescription description;
    description.addNode(node("can_a", "can.source", channelZero));
    description.addNode(node("can_b", "can.source", channelZero));
    description.addNode(
        node("ecu_a", "lua.ecu",
             NodeParameters{{"script", ParameterValue::fromText(
                                           R"(function on_message(id)
                                                  if id == 0x100 then emit(0x201, "\1") end
                                              end)")}}));
    description.addNode(
        node("ecu_b", "lua.ecu",
             NodeParameters{{"script", ParameterValue::fromText(
                                           R"(function on_message(id)
                                                  if id == 0x100 then emit(0x202, "\1") end
                                              end)")}}));
    description.addNode(node("tx_a", "can.transmit", channelZero));
    description.addNode(node("tx_b", "can.transmit", channelZero));

    description.addEdge(EdgeDescription{"can_a", 0, "ecu_a", 0});
    description.addEdge(EdgeDescription{"can_b", 0, "ecu_b", 0});
    description.addEdge(EdgeDescription{"ecu_a", 0, "tx_a", 0});
    description.addEdge(EdgeDescription{"ecu_b", 0, "tx_b", 0});

    std::vector<CanFrame> received;
    std::mutex mutex;
    engine.addFrameSink([&](std::span<const CanFrame> batch) {
        const std::lock_guard lock{mutex};
        received.insert(received.end(), batch.begin(), batch.end());
    });

    engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());
    REQUIRE(engine.start().succeeded());

    CanFrame request;
    request.identifier = 0x100;
    request.length = 1;
    request.dlc = 1;
    REQUIRE(engine.transmit(0, request).succeeded());

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    engine.stop();

    const std::lock_guard lock{mutex};
    const auto count = [&received](std::uint32_t identifier) {
        return std::count_if(received.begin(), received.end(),
                             [identifier](const CanFrame& f) {
                                 return f.identifier == identifier;
                             });
    };

    CHECK(count(0x201) >= 1);
    CHECK(count(0x202) >= 1);

    // And the trace saw the request too, so the engine's own default path was
    // not starved by the user's blocks either.
    CHECK(engine.traceStore().size() >= 1);
}

TEST_CASE("One script becomes two ECUs through its parameters", "[graph][build][lua]")
{
    // The point of the parameters table, borrowed from cansim: without it every
    // number a script needs is a constant in the file, and running the same
    // behaviour twice with different numbers means copying the file.
    //
    // Here one script, two nodes, two identifiers.
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const std::string script = R"(
        function on_message()
            emit(parameters.answer_id, string.pack("<I1", parameters.payload))
        end
    )";

    GraphDescription description;
    description.addNode(node("ecu_a", "lua.ecu",
                             NodeParameters{
                                 {"script", ParameterValue::fromText(script)},
                                 {"answer_id", ParameterValue::fromInteger(0x201)},
                                 {"payload", ParameterValue::fromInteger(0xAA)},
                             }));
    description.addNode(node("ecu_b", "lua.ecu",
                             NodeParameters{
                                 {"script", ParameterValue::fromText(script)},
                                 {"answer_id", ParameterValue::fromInteger(0x202)},
                                 {"payload", ParameterValue::fromInteger(0xBB)},
                             }));

    PipelineGraph graph;
    REQUIRE(description.build(catalog, contextWith(store), graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();

    const auto ids = graph.nodeIds();
    REQUIRE(ids.size() == 2);
}

TEST_CASE("A script reads its settings from the parameters table", "[graph][build][lua]")
{
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("src", "can.filter")); // Stand-in producer of nothing.
    description.addNode(
        node("ecu", "lua.ecu",
             NodeParameters{
                 // Every type the parameter bag carries, so the conversion into
                 // Lua is checked rather than assumed for three of four.
                 {"script", ParameterValue::fromText(R"(
                     function on_enable()
                         if parameters.count ~= 7 then error("integer lost") end
                         if math.abs(parameters.factor - 0.125) > 1e-9 then
                             error("real lost")
                         end
                         if parameters.label ~= "engine" then error("text lost") end
                         if parameters.active ~= true then error("boolean lost") end
                         if parameters.script ~= nil then
                             error("the node's own settings leaked into the script")
                         end
                     end
                 )")},
                 {"count", ParameterValue::fromInteger(7)},
                 {"factor", ParameterValue::fromReal(0.125)},
                 {"label", ParameterValue::fromText("engine")},
                 {"active", ParameterValue::fromBoolean(true)},
             }));

    PipelineGraph graph;
    REQUIRE(description.build(catalog, contextWith(store), graph).succeeded());

    // on_enable runs during compile, and it raises if anything above is wrong.
    const Result result = graph.compile();
    INFO(std::string{result.message()});
    REQUIRE(result.succeeded());
}

TEST_CASE("A script with no parameters still gets an empty table", "[graph][build][lua]")
{
    // So that `parameters.can_id or 0x100` works without first testing that the
    // table exists. A nil global here would make every example script two lines
    // longer for no reason.
    LuaEcuNode node{R"(
        function on_enable()
            if parameters == nil then error("no parameters table") end
            if next(parameters) ~= nil then error("table should be empty") end
        end
    )",
                    "bare.lua"};

    REQUIRE(node.prepare(64).succeeded());
}

TEST_CASE("A disabled node is skipped, and so are its wires", "[graph][build]")
{
    // Switching a node off must not require rewiring around it: deleting it to
    // try the measurement without it, then drawing it again, loses its settings
    // and its position. This is the same experiment without the loss.
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("filter", "can.filter"));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"filter", 0, "trace_1", 0});

    description.nodes()[1].enabled = false;

    PipelineGraph graph;
    REQUIRE(description.build(catalog, contextWith(store), graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    // The filter is there; the trace is not, and the wire between them was not
    // an error on the way out.
    CHECK(graph.nodeIds().size() == 1);
}

TEST_CASE("Two wires into one input are allowed when one source is disabled",
          "[graph][validation]")
{
    // Otherwise switching a node off would leave the project refusing to build
    // over a wire that will not exist.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("can_1", "can.source"));
    description.addNode(node("can_2", "can.source"));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"can_1", 0, "trace_1", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "trace_1", 0});

    CHECK(description.validate(catalog).failed());

    description.nodes()[1].enabled = false;
    CHECK(description.validate(catalog).succeeded());
}

TEST_CASE("A node naming a channel that is not configured says so in the user's numbering",
          "[graph][build]")
{
    // "CAN 3", not "channel 2". A message using the internal index when the
    // panel shows a different number costs somebody ten minutes.
    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(node("can_3", "can.source",
                             NodeParameters{{"channel", ParameterValue::fromInteger(2)}}));

    engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());

    const Result result = engine.start();

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("CAN 3") != std::string::npos);
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("A Lua node with both a script and a path is refused", "[graph][build]")
{
    // A project holding one copy of a script and a stale path to another is a
    // bug that only appears on the machine where the path happens to resolve.
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("ecu", "lua.ecu",
                             NodeParameters{
                                 {"script", ParameterValue::fromText("function on_message() end")},
                                 {"scriptPath", ParameterValue::fromText("ecu.lua")},
                             }));

    PipelineGraph graph;
    const Result result = description.build(catalog, contextWith(store), graph);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("Keep one") != std::string::npos);
}

TEST_CASE("A Lua node loads its script from a file", "[graph][build]")
{
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(
        node("ecu", "lua.ecu",
             NodeParameters{{"scriptPath",
                             ParameterValue::fromText(TORQUEBUS_EXAMPLE_SCRIPT_DIR
                                                      "/ecu_vehicle.lua")}}));

    PipelineGraph graph;
    REQUIRE(description.build(catalog, contextWith(store), graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    // Named after the file and not the node id, because that is what the user
    // has open when they read the error.
    CHECK(graph.node(graph.executionOrder().front())->displayName() == "ecu_vehicle.lua");
}

TEST_CASE("A missing script file fails the build naming the node", "[graph][build]")
{
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("ecu_motor", "lua.ecu",
                             NodeParameters{{"scriptPath",
                                             ParameterValue::fromText("no/such/script.lua")}}));

    PipelineGraph graph;
    const Result result = description.build(catalog, contextWith(store), graph);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("ecu_motor") != std::string::npos);
}

TEST_CASE("A trace node without a store fails rather than dropping frames", "[graph][build]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("trace_1", "trace.sink"));

    NodeBuildContext context; // No trace store.
    PipelineGraph graph;

    const Result result = description.build(catalog, context, graph);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("trace_1") != std::string::npos);
}

TEST_CASE("A filter node built from parameters actually filters", "[graph][build]")
{
    TraceStore store{1024};
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(node("filter", "can.filter",
                             NodeParameters{
                                 {"from", ParameterValue::fromInteger(0x100)},
                                 {"to", ParameterValue::fromInteger(0x1FF)},
                             }));
    description.addNode(node("trace_1", "trace.sink"));
    description.addEdge(EdgeDescription{"filter", 0, "trace_1", 0});

    PipelineGraph graph;
    REQUIRE(description.build(catalog, contextWith(store), graph).succeeded());
    REQUIRE(graph.compile().succeeded());

    // The parameters reached the filter: two frames in, one inside the range.
    const NodeId filterNode = graph.executionOrder().front();
    auto* filter = graph.nodeAs<FrameFilterNode>(filterNode);
    REQUIRE(filter != nullptr);
    REQUIRE(filter->filters().size() == 1);
    CHECK(filter->filters().filters().front().identifierFrom == 0x100U);
    CHECK(filter->filters().filters().front().identifierTo == 0x1FFU);
}

TEST_CASE("A description survives being edited while a measurement runs", "[graph][build]")
{
    // The engine takes a copy, so the canvas can go on being edited during a
    // recording without changing what is currently running. What runs is what
    // was set, not whatever the canvas has become since.
    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    REQUIRE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(node("can_1", "can.source",
                             NodeParameters{{"channel", ParameterValue::fromInteger(0)}}));

    engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());
    REQUIRE(engine.start().succeeded());

    // An edit that would not build - a node with an unknown type - must not
    // affect the running measurement at all.
    description.addNode(node("nonsense", "no.such.type"));

    CHECK(engine.isRunning());
    engine.stop();
}

// ---------------------------------------------------------------------------
// Settings that are wrong before the graph is ever built
// ---------------------------------------------------------------------------
//
// These exist because of a real session: a Lua ECU block was dropped on the
// canvas, the Output panel said "Pipeline: 1 node(s), 0 connection(s)" - and
// then Start failed with "has no script". validate() is the function that
// promises to report a problem while the user is still looking at the block,
// and it was not looking at parameters at all.

TEST_CASE("A block missing a required setting is caught before Start",
          "[graph][validate]")
{
    // Registered here rather than borrowed from the built-ins, and that is the
    // finding rather than a convenience: **no built-in type has a required
    // parameter.** All three that used to claim one - can.source, can.transmit
    // and dbc.decoder - build perfectly well without it, on purpose, so that a
    // canvas can be assembled in any order. Their descriptors said otherwise
    // and were corrected.
    //
    // So this tests the mechanism, which is the part that has to work when a
    // type does have a setting it genuinely cannot be built without.
    NodeCatalog catalog;
    catalog.registerType(
        NodeTypeInfo{
            .typeName = "test.needsPath",
            .displayName = "Needs a path",
            .category = "Tests",
            .description = "A type with a setting that has no sensible default.",
            .parameters = {ParameterDescriptor{.name = "path",
                                               .displayName = "File",
                                               .type = ParameterValue::Type::Text,
                                               .required = true,
                                               .description = "No default is possible."}},
        },
        [](const NodeParameters&, const NodeBuildContext&, std::string_view,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            out = nullptr;
            return Result::ok();
        });

    GraphDescription graph;
    graph.addNode(NodeDescription{.id = "reader", .typeName = "test.needsPath"});

    const Result result = graph.validate(catalog);
    REQUIRE(result.failed());

    // Named by the label the properties editor shows, because the person
    // reading this is looking at a form with an empty field in it - and by the
    // block's id, because a graph with forty blocks has to say which one.
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("File") != std::string::npos);
    CHECK(std::string{result.message()}.find("reader") != std::string::npos);

    graph.nodes()[0].parameters.set("path", ParameterValue::fromText("somewhere.txt"));
    CHECK(graph.validate(catalog).succeeded());
}

TEST_CASE("A half-configured canvas is not an error", "[graph][validate]")
{
    // The decision this whole area turns on, written down in NodeCatalog.cpp
    // and worth a test of its own: a block dropped and not yet filled in must
    // still validate. A graph that refuses until every block is configured
    // cannot be built up in any order but one, which is not how anybody draws.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription graph;
    graph.addNode(NodeDescription{.id = "src", .typeName = "can.source"});
    graph.addNode(NodeDescription{.id = "dec", .typeName = "dbc.decoder"});
    graph.addNode(NodeDescription{.id = "tx", .typeName = "can.transmit"});

    const Result result = graph.validate(catalog);
    INFO(std::string{result.message()});
    CHECK(result.succeeded());
}

TEST_CASE("A Lua ECU with no script at all is caught before Start",
          "[graph][validate][lua]")
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription graph;
    graph.addNode(NodeDescription{.id = "lua_ecu", .typeName = "lua.ecu"});

    const Result result = graph.validate(catalog);

    REQUIRE(result.failed());

    // And it says what to do about it. "has no script" was true and useless.
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("Script") != std::string::npos);
}

TEST_CASE("A Lua ECU with both a script and a path is caught before Start",
          "[graph][validate][lua]")
{
    // Neither parameter is required on its own and exactly one is required
    // together - a sentence about two parameters, which no ParameterDescriptor
    // can express. That is what a type's own validator is for.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription graph;
    graph.addNode(NodeDescription{
        .id = "lua_ecu",
        .typeName = "lua.ecu",
        .parameters = {{"script", ParameterValue::fromText("function on_frame() end")},
                       {"scriptPath", ParameterValue::fromText("ecu.lua")}}});

    CHECK(graph.validate(catalog).failed());
}

TEST_CASE("A block whose settings are complete passes", "[graph][validate]")
{
    // The other half of the same claim: a check that only ever refuses would
    // pass the three cases above and be worthless.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription graph;
    graph.addNode(NodeDescription{
        .id = "lua_ecu",
        .typeName = "lua.ecu",
        .parameters = {{"script", ParameterValue::fromText("function on_frame() end")}}});
    graph.addNode(NodeDescription{
        .id = "tx",
        .typeName = "can.transmit",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}}});


    const Result result = graph.validate(catalog);
    INFO(std::string{result.message()});
    CHECK(result.succeeded());
}

TEST_CASE("Validating settings twice is what the creator still does anyway",
          "[graph][validate][lua]")
{
    // The creator does not assume anybody validated first. A caller can build
    // a description it never validated - the engine does, on a project file -
    // and a node built without a script would otherwise run as an ECU that
    // does nothing, which is the failure that hides.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> node;

    const Result result = catalog.create("lua.ecu", NodeParameters{}, context, "lua_ecu", node);

    CHECK(result.failed());
    CHECK(node == nullptr);
}
