// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Every block the palette offers, built and run.
//
// The rest of the suite tests each block where it lives, with the inputs its author
// thought of. What none of it can say is that the whole catalog works as the canvas uses
// it: a block built from the parameters the catalog declares, wired the way its ports
// say, started on an engine, fed traffic, stopped. A block that builds in a test and not
// from the catalog, one that faults on the second frame, one that was added without
// anything running it - those are found here, by a person with the window open, or not at
// all.
//
// The test has two halves. Each block type has a recipe: the smallest graph that
// exercises it, with something to feed it and somewhere for its output to go. Then a test
// that fails if a type has no recipe, which is how a block added tomorrow is held to the
// same standard without anybody remembering to.

#include "core/can/CanEngine.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

/// Files the recipes point blocks at.
struct Files final {
    std::filesystem::path folder;
    std::string database; ///< One message, 0x321, every 10 ms.
    std::string j1939Database;
    std::string log; ///< A short recording of 0x555.
};

[[nodiscard]] NodeDescription block(std::string id,
                                    std::string type,
                                    NodeParameters parameters = {},
                                    double x = 0.0,
                                    double y = 0.0)
{
    return NodeDescription{.id = std::move(id),
                           .typeName = std::move(type),
                           .parameters = std::move(parameters),
                           .x = x,
                           .y = y};
}

[[nodiscard]] NodeParameters text(const char* name, std::string value)
{
    return NodeParameters{{name, ParameterValue::fromText(std::move(value))}};
}

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = identifier;
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    result.dlc = length;
    result.length = length;
    return result;
}

/// What a recipe says about a block.
struct Recipe final {
    GraphDescription graph;

    /// Identifiers that must appear on the bus because of the block - the proof that it did
    /// something and not only that it did not fall over. Empty when its job is not to
    /// transmit.
    std::set<std::uint32_t> mustAppear;

    /// A recording is started before Start, for the block that only builds under Record.
    bool recording{false};

    /// A row in the Transmit list.
    bool transmitRow{false};

    /// Decoded signals must have reached the plot store.
    bool mustPlot{false};
};

using RecipeFactory = std::function<Recipe(const Files&)>;

/// The graph for one block: a channel to feed it, the block, and a place for what it makes.
[[nodiscard]] Recipe transform(const std::string& type,
                               NodeParameters parameters,
                               bool feedsTheBus,
                               std::set<std::uint32_t> mustAppear = {})
{
    Recipe recipe;
    recipe.mustAppear = std::move(mustAppear);

    recipe.graph.addNode(block("src", "can.source", {}, 0, 0));
    recipe.graph.addNode(block("under_test", type, std::move(parameters), 200, 0));
    recipe.graph.addNode(block("sink", feedsTheBus ? "can.transmit" : "trace.sink", {}, 400, 0));
    recipe.graph.addEdge(EdgeDescription{"src", 0, "under_test", 0});
    recipe.graph.addEdge(EdgeDescription{"under_test", 0, "sink", 0});

    return recipe;
}

/// A block with no input, whose frames go onto the bus.
[[nodiscard]] Recipe
producer(const std::string& type, NodeParameters parameters, std::set<std::uint32_t> mustAppear)
{
    Recipe recipe;
    recipe.mustAppear = std::move(mustAppear);

    recipe.graph.addNode(block("under_test", type, std::move(parameters), 0, 0));
    recipe.graph.addNode(block("sink", "can.transmit", {}, 200, 0));
    recipe.graph.addEdge(EdgeDescription{"under_test", 0, "sink", 0});

    return recipe;
}

[[nodiscard]] const std::map<std::string, RecipeFactory>& recipes()
{
    static const std::map<std::string, RecipeFactory> table = {
        {"can.source",
         [](const Files&) {
             Recipe recipe;
             recipe.graph.addNode(block("under_test", "can.source"));
             recipe.graph.addNode(block("sink", "trace.sink", {}, 200, 0));
             recipe.graph.addEdge(EdgeDescription{"under_test", 0, "sink", 0});
             return recipe;
         }},

        {"can.transmit",
         // What it is for is putting a block's frames on the channel, which is what
         // the rest bus does here. (Not a filter that re-sends what it hears: on a
         // bus that echoes a transmit back, that is a flood.)
         [](const Files& files) {
             return producer("sim.restbus", text("database", files.database), {0x321});
         }},

        {"trace.sink",
         [](const Files&) {
             Recipe recipe;
             recipe.graph.addNode(block("src", "can.source"));
             recipe.graph.addNode(block("under_test", "trace.sink", {}, 200, 0));
             recipe.graph.addEdge(EdgeDescription{"src", 0, "under_test", 0});
             return recipe;
         }},

        {"can.filter",
         [](const Files&) {
             return transform("can.filter",
                              NodeParameters{{"from", ParameterValue::fromInteger(0x100)},
                                             {"to", ParameterValue::fromInteger(0x1FF)}},
                              false);
         }},

        {"can.bus",
         [](const Files&) {
             Recipe recipe;
             recipe.mustAppear = {0x200};

             recipe.graph.addNode(
                 block("under_test", "can.bus", {{"channel", ParameterValue::fromInteger(0)}}));
             recipe.graph.addNode(block("ecu",
                                        "lua.ecu",
                                        text("script",
                                             "function on_message(id, data)\n"
                                             "  if id == 0x100 then emit(0x200, data) end\n"
                                             "end\n"),
                                        200,
                                        140));
             recipe.graph.addEdge(EdgeDescription{"under_test", 0, "ecu", 0});
             recipe.graph.addEdge(EdgeDescription{"ecu", 0, "under_test", 0});
             return recipe;
         }},

        {"lua.ecu",
         [](const Files&) {
             return transform("lua.ecu",
                              text("script",
                                   "function on_message(id, data)\n"
                                   "  if id == 0x100 then emit(0x200, data) end\n"
                                   "end\n"),
                              true,
                              {0x200});
         }},

        {"lua.test",
         [](const Files&) {
             return transform("lua.test",
                              text("script",
                                   "test('alive', function()\n"
                                   "  send(0x600, '\\x01')\n"
                                   "  assert_true(true)\n"
                                   "end)\n"),
                              true,
                              {0x600});
         }},

        {"sim.restbus",
         [](const Files& files) {
             return producer("sim.restbus", text("database", files.database), {0x321});
         }},

        {"dbc.decoder",
         [](const Files& files) {
             Recipe recipe;
             recipe.mustPlot = true;
             recipe.graph.addNode(block("src", "can.source"));
             recipe.graph.addNode(
                 block("under_test", "dbc.decoder", text("database", files.database), 200, 0));
             recipe.graph.addNode(block("plot", "signal.plot", {}, 400, 0));
             recipe.graph.addEdge(EdgeDescription{"src", 0, "under_test", 0});
             recipe.graph.addEdge(EdgeDescription{"under_test", 0, "plot", 0});
             return recipe;
         }},

        {"signal.plot",
         [](const Files& files) {
             Recipe recipe;
             recipe.mustPlot = true;
             recipe.graph.addNode(block("src", "can.source"));
             recipe.graph.addNode(
                 block("dec", "dbc.decoder", text("database", files.database), 200, 0));
             recipe.graph.addNode(block("under_test", "signal.plot", {}, 400, 0));
             recipe.graph.addEdge(EdgeDescription{"src", 0, "dec", 0});
             recipe.graph.addEdge(EdgeDescription{"dec", 0, "under_test", 0});
             return recipe;
         }},

        {"j1939.decoder",
         [](const Files& files) {
             Recipe recipe;
             recipe.graph.addNode(block("src", "can.source"));
             recipe.graph.addNode(block(
                 "under_test", "j1939.decoder", text("database", files.j1939Database), 200, 0));
             recipe.graph.addNode(block("plot", "signal.plot", {}, 400, 0));
             recipe.graph.addEdge(EdgeDescription{"src", 0, "under_test", 0});
             recipe.graph.addEdge(EdgeDescription{"under_test", 0, "plot", 0});
             return recipe;
         }},

        {"transmit.list",
         [](const Files&) {
             Recipe recipe = producer("transmit.list", {}, {0x400});
             recipe.transmitRow = true;
             return recipe;
         }},

        {"log.source",
         [](const Files& files) {
             return producer("log.source", text("path", files.log), {0x555});
         }},

        {"uds.client", [](const Files&) { return transform("uds.client", {}, true); }},

        {"isotp.transport",
         [](const Files&) {
             return transform("isotp.transport", text("request", "22 F1 90"), true, {0x7E0});
         }},

        {"tinyml.ecu", [](const Files&) { return transform("tinyml.ecu", {}, true, {0x105}); }},

        {"can.log",
         [](const Files&) {
             Recipe recipe;
             recipe.recording = true;
             recipe.graph.addNode(block("src", "can.source"));
             recipe.graph.addNode(block("under_test", "can.log", {}, 200, 0));
             recipe.graph.addEdge(EdgeDescription{"src", 0, "under_test", 0});
             return recipe;
         }},
    };

    return table;
}

class BlocksTest : public testing::Test {
protected:
    void SetUp() override
    {
        // Its own folder per process. ctest runs each test in a process of its own, several at
        // a time, and a folder they shared would be one test removing the files another is
        // about to open.
        std::random_device entropy;
        files.folder = std::filesystem::temp_directory_path()
                       / std::format("torquebus-blocks-{:08x}{:08x}", entropy(), entropy());
        std::filesystem::create_directories(files.folder);

        files.database = (files.folder / "sweep.dbc").string();
        {
            std::ofstream out{files.database};
            out << "VERSION \"sweep\"\nBU_: ECU\n"
                   "BO_ 801 Plain: 8 ECU\n"
                   " SG_ Value : 0|16@1+ (1,0) [0|65535] \"\" Vector__XXX\n"
                   "BA_DEF_ BO_ \"GenMsgCycleTime\" INT 0 65535;\n"
                   "BA_ \"GenMsgCycleTime\" BO_ 801 10;\n";
        }

        files.j1939Database = std::string{TORQUEBUS_EXAMPLE_DATABASE_DIR} + "/j1939.dbc";

        files.log = (files.folder / "sweep.tblog").string();
        {
            TraceLogWriter writer;
            EXPECT_TRUE(writer.open(files.log, 1).succeeded());

            std::vector<CanFrame> frames;
            for (std::uint64_t index = 0; index < 200; ++index) {
                CanFrame recorded = frame(0x555, 2);
                recorded.timestampNs = index * 1'000'000;
                frames.push_back(recorded);
            }

            EXPECT_TRUE(writer.append(frames).succeeded());
            writer.close();
        }
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(files.folder, ignored);
    }

    /// Builds the graph on an engine, runs it for a moment with traffic, and returns what
    /// went wrong, if anything.
    void run(const std::string& type)
    {
        SCOPED_TRACE(type);

        const auto recipe = recipes().find(type);
        ASSERT_NE(recipe, recipes().end())
            << "'" << type
            << "' has no recipe in PipelineBlocksTests.cpp. Add the smallest graph that "
               "exercises it - something to feed it, and somewhere for what it makes to go - "
               "so that a block added to the catalog is run by something before it ships.";

        Recipe built = recipe->second(files);

        const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
        ASSERT_TRUE(built.graph.validate(catalog).succeeded())
            << built.graph.validate(catalog).message();

        CanEngine engine;

        CanChannelConfig config;
        config.deviceHandle = "virtual:0";
        config.timing.bitrate = 500'000;
        ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

        TransmitList list;
        engine.setTransmitList(&list);

        if (built.transmitRow) {
            TransmitEntry row;
            row.name = "row";
            row.frame = frame(0x400, 2);
            row.trigger = TransmitTrigger::Periodic;
            row.cycleMs = 10;
            (void)list.add(row);
        }

        std::mutex mutex;
        std::vector<std::string> problems;
        engine.addLogSink([&](const std::string& message, bool isError) {
            if (isError) {
                const std::lock_guard lock{mutex};
                problems.push_back(message);
            }
        });

        engine.setGraphDescription(built.graph, catalog, files.folder.string());

        if (built.recording) {
            ASSERT_TRUE(
                engine.startRecording((files.folder / "recorded.tblog").string()).succeeded());
        }

        const Result started = engine.start();
        ASSERT_TRUE(started.succeeded()) << started.message();

        // Traffic: something for every kind of block to find. The speed and temperature
        // that the TinyML block watches, a message the database knows, a J1939 identifier, a
        // diagnostic request, and the identifier the scripts answer.
        for (int round = 0; round < 6; ++round) {
            for (const std::uint32_t identifier :
                 {0x100U, 0x101U, 0x102U, 0x321U, 0x18FEEE00U, 0x7E8U}) {
                CanFrame traffic = frame(identifier);
                traffic.data[0] = static_cast<std::uint8_t>(round * 7);
                (void)engine.transmit(0, traffic);
            }

            std::this_thread::sleep_for(15ms);
        }

        std::this_thread::sleep_for(60ms);

        EXPECT_TRUE(engine.isRunning()) << "the measurement ended on its own";
        EXPECT_EQ(engine.graph().faultedNodeCount(), 0U) << "a block threw";

        engine.stop();
        engine.stopRecording();

        {
            const std::lock_guard lock{mutex};
            EXPECT_TRUE(problems.empty())
                << "the Output panel would have shown: " << problems.front();
        }

        std::set<std::uint32_t> seen;
        for (const TraceIdentifierStats& stats : engine.traceStore().identifiers()) {
            seen.insert(stats.identifier);
        }

        for (const std::uint32_t identifier : built.mustAppear) {
            EXPECT_TRUE(seen.contains(identifier))
                << "nothing with identifier 0x" << std::hex << identifier
                << " ever reached the bus, so the block did not do what it is for";
        }

        if (built.mustPlot) {
            EXPECT_GT(engine.plotStore().seriesCount(), 0U) << "no signal was ever decoded";
        }
    }

    Files files;
};

} // namespace

TEST_F(BlocksTest, EveryBlockTheCatalogOffersHasARecipe)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    for (const NodeTypeInfo& info : catalog.types()) {
        EXPECT_TRUE(recipes().contains(info.typeName))
            << "'" << info.typeName
            << "' is in the catalog and has no recipe in PipelineBlocksTests.cpp";
    }

    // And no recipe for a block that is gone.
    for (const auto& [type, factory] : recipes()) {
        static_cast<void>(factory);
        EXPECT_TRUE(catalog.contains(type))
            << "'" << type << "' has a recipe and is not in the catalog";
    }
}

TEST_F(BlocksTest, CanBus)
{
    run("can.bus");
}

TEST_F(BlocksTest, CanSource)
{
    run("can.source");
}

TEST_F(BlocksTest, CanTransmit)
{
    run("can.transmit");
}

TEST_F(BlocksTest, TraceSink)
{
    run("trace.sink");
}

TEST_F(BlocksTest, CanFilter)
{
    run("can.filter");
}

TEST_F(BlocksTest, LuaEcu)
{
    run("lua.ecu");
}

TEST_F(BlocksTest, LuaTest)
{
    run("lua.test");
}

TEST_F(BlocksTest, RestBus)
{
    run("sim.restbus");
}

TEST_F(BlocksTest, DbcDecoder)
{
    run("dbc.decoder");
}

TEST_F(BlocksTest, SignalPlot)
{
    run("signal.plot");
}

TEST_F(BlocksTest, J1939Decoder)
{
    run("j1939.decoder");
}

TEST_F(BlocksTest, TransmitList)
{
    run("transmit.list");
}

TEST_F(BlocksTest, LogReplay)
{
    run("log.source");
}

TEST_F(BlocksTest, UdsClient)
{
    run("uds.client");
}

TEST_F(BlocksTest, IsoTpTransport)
{
    run("isotp.transport");
}

TEST_F(BlocksTest, TinyMlEcu)
{
    run("tinyml.ecu");
}

TEST_F(BlocksTest, CanLogger)
{
    run("can.log");
}
