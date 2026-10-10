// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The example scripts, run the way a person following the README runs them.
//
// Five scripts ship in `examples/scripts/`. The README points at them, the
// scripting guide walks through them, and they are the first thing somebody
// evaluating this tool will open. Until this file, four of the five were never
// executed by anything: one test loaded `ecu_vehicle.lua` to check a node takes
// its name from the filename, and stopped there.
//
// That gap has a specific shape. The Lua API is ours and it moves - `emit`
// changed signature once already. A change to it breaks these scripts
// silently: they are data, the compiler never sees them, and the failure
// surfaces as a line in the Output panel on the machine of somebody trying the
// tool for the first time.
//
// So each one is loaded and *run*, and each is asked for the thing it exists to
// demonstrate. Not deeply - this is not a second suite for the Lua engine,
// which has its own - but enough that a broken example cannot ship.

#include "core/can/CanEngine.h"
#include "core/database/DbcParser.h"
#include "core/isotp/IsoTpTypes.h"
#include "core/j1939/J1939Id.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/LuaTestNode.h"
#include "core/testing/TestReport.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::filesystem::path scriptPath(const char* name)
{
    return std::filesystem::path{TORQUEBUS_EXAMPLE_SCRIPT_DIR} / name;
}

[[nodiscard]] std::string readScript(const char* name)
{
    const std::filesystem::path path = scriptPath(name);
    EXPECT_TRUE(std::filesystem::exists(path));

    std::ifstream file{path, std::ios::binary};
    EXPECT_TRUE(file.is_open());

    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

CanChannelConfig quietConfig(const std::string& handle)
{
    CanChannelConfig config;
    config.deviceHandle = handle;
    config.timing.bitrate = 500'000;
    return config;
}

/// The error lines as one string, for a failure message that names them.
[[nodiscard]] std::string joined(const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += " | ";
        }
        text += line;
    }
    return text;
}

/// Collects what reached the bus, from the engine thread.
class Recorder final {
public:
    [[nodiscard]] FrameSink sink()
    {
        return [this](std::span<const CanFrame> batch) {
            const std::lock_guard lock{m_mutex};
            m_frames.insert(m_frames.end(), batch.begin(), batch.end());
        };
    }

    [[nodiscard]] std::size_t countWithIdentifier(std::uint32_t identifier) const
    {
        const std::lock_guard lock{m_mutex};
        std::size_t count = 0;
        for (const CanFrame& item : m_frames) {
            if (item.identifier == identifier) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t total() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames.size();
    }

    [[nodiscard]] std::vector<CanFrame> frames() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<CanFrame> m_frames;
};

/// What one example ECU did when it was run for a moment.
struct Outcome final {
    std::uint64_t emitted{};
    bool faulted{true};
    Recorder recorder;

    /// Error lines the script produced, which is the half isFaulted() misses.
    ///
    /// A node only faults after an error limit, deliberately: an occasional
    /// error must not kill an ECU. So a script erroring on every single tick
    /// can still come back not faulted from a short run - which is exactly what
    /// ecu_vehicle_dbc.lua did here before it was given its database, and what
    /// this test would have called a pass.
    std::vector<std::string> errors;
};

/// Runs an ECU script on a virtual bus for `duration`.
///
/// The shape is the one from the scripting guide - the script's emit() reaches a
/// real channel through the engine's own graph - because running these any other
/// way would be testing a path no user takes.
void runEcu(const char* name,
            std::chrono::milliseconds duration,
            Outcome& outcome,
            const char* databaseName = nullptr)
{
    const std::string source = readScript(name);

    // Some examples speak signal names rather than bytes, and emit_signal
    // refuses to guess without a database - the same rule uds_did follows. The
    // script's own header says which file to point the block at; this points it
    // at the same one.
    std::shared_ptr<const CanDatabase> database;
    if (databaseName != nullptr) {
        auto parsed = std::make_shared<CanDatabase>();
        const std::filesystem::path path =
            std::filesystem::path{TORQUEBUS_EXAMPLE_DATABASE_DIR} / databaseName;

        const Result read = DbcParser::parseFile(path.string(), *parsed);
        SCOPED_TRACE(::testing::Message() << std::string{read.message()});
        EXPECT_TRUE(read.succeeded());

        database = std::move(parsed);
    }

    CanEngine engine;
    EXPECT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), quietConfig("virtual:0"))
                    .succeeded());

    engine.addFrameSink(outcome.recorder.sink());

    // Collected from the node directly, and not only through the engine's log
    // sinks. A node gets its log handler from NodeCatalog when it is built from
    // a GraphDescription, which is how the application does it - a graph
    // assembled by hand here would silently have none, and every one of these
    // assertions would then be checking an empty vector. Finding that out was
    // this test failing with "script errors:" and nothing after the colon.
    auto collect = [&outcome, errorMutex = std::make_shared<std::mutex>()](const std::string& text,
                                                                           bool isError) {
        if (!isError) {
            return;
        }
        const std::lock_guard lock{*errorMutex};
        outcome.errors.push_back(text);
    };

    engine.addLogSink(collect);

    LuaEcuNode* ecu = nullptr;

    engine.setGraphBuilder([&engine, &source, name, &ecu, &database, collect](
                               PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
        auto node = std::make_unique<LuaEcuNode>(source, name);
        node->setLogHandler(collect);
        if (database) {
            node->setDatabase(database);
        }
        ecu = node.get();

        const NodeId ecuId = graph.addNode(std::move(node));
        const NodeId transmit =
            graph.addNode(std::make_unique<ChannelSinkNode>(*engine.channel(0)));

        if (Result result = graph.connect(PortRef{sources[0], 0}, PortRef{ecuId, 0});
            result.failed()) {
            return result;
        }
        return graph.connect(PortRef{ecuId, 0}, PortRef{transmit, 0});
    });

    // A script that does not compile, or whose on_enable throws, fails here -
    // which is the single most useful thing this file checks, because it is the
    // failure a stale example actually has.
    EXPECT_TRUE(engine.start().succeeded());

    std::this_thread::sleep_for(duration);

    EXPECT_TRUE(ecu != nullptr);
    outcome.emitted = ecu->emittedFrames();
    outcome.faulted = ecu->isFaulted();

    engine.stop();
}

} // namespace

TEST(ExampleScriptTests, EcuVehicleLuaPutsSpeedAndTemperatureOnTheBus)
{
    Outcome outcome;
    runEcu("ecu_vehicle.lua", 250ms, outcome);

    EXPECT_FALSE(outcome.faulted);
    EXPECT_TRUE(outcome.emitted > 0);

    SCOPED_TRACE(::testing::Message() << "script errors: " << joined(outcome.errors));
    EXPECT_TRUE(outcome.errors.empty());

    // The two identifiers the script's own comments name, and the ones
    // examples/databases/vehicle.dbc decodes - so this also guards the pairing
    // the README demonstrates.
    EXPECT_TRUE(outcome.recorder.countWithIdentifier(0x101U) > 0);
    EXPECT_TRUE(outcome.recorder.countWithIdentifier(0x102U) > 0);
}

TEST(ExampleScriptTests, EcuVehicleDbcLuaSpeaksSignalNamesAgainstTheShippedDatabase)
{
    // The pairing the script exists to demonstrate: it names signals and the
    // database supplies the identifier, the byte order and the scaling. Run
    // without one it emits nothing - emit_signal refuses rather than guessing -
    // which is how this test found out it had to supply the database the
    // script's header names.
    Outcome outcome;
    runEcu("ecu_vehicle_dbc.lua", 250ms, outcome, "vehicle.dbc");

    EXPECT_FALSE(outcome.faulted);
    EXPECT_TRUE(outcome.emitted > 0);

    SCOPED_TRACE(::testing::Message() << "script errors: " << joined(outcome.errors));
    EXPECT_TRUE(outcome.errors.empty());

    // Same identifiers as the hand-packed version, which is the whole claim:
    // the two scripts put the same thing on the wire.
    EXPECT_TRUE(outcome.recorder.countWithIdentifier(0x101U) > 0);
}

TEST(ExampleScriptTests, EcuMotorLuaRunsAndTransmits)
{
    // Exercises the reactive powertrain actuator ECU script (`ecu_motor.lua`),
    // which uses `string.pack` / `string.unpack` and periodic status frames.
    Outcome outcome;
    runEcu("ecu_motor.lua", 250ms, outcome);

    EXPECT_FALSE(outcome.faulted);
    EXPECT_TRUE(outcome.emitted > 0);

    SCOPED_TRACE(::testing::Message() << "script errors: " << joined(outcome.errors));
    EXPECT_TRUE(outcome.errors.empty());
}

TEST(ExampleScriptTests, EcuUdsLuaComesUpAsADiagnosticServer)
{
    // This one is not cyclic in the same way - it exists to answer, and a bus
    // with no tester on it gives it nothing to answer. So the assertion is that
    // it loaded, enabled, and registered itself as something that answers
    // diagnostics; the UDS behaviour itself has its own tests.
    const std::string source = readScript("ecu_uds.lua");

    LuaEcuNode ecu{source, "ecu_uds.lua"};

    // The addresses the script's own header tells the reader to set on the
    // block: request 0x7E0, response 0x7E8. Without them prepare() fails
    // naming uds_did, which is deliberate - a block that cannot do what its
    // script says is refused while somebody is looking at it, rather than
    // running as a half-ECU nobody can see is wrong.
    //
    // Running the example scripts in the test suite verifies that the same
    // configuration rules apply to shipped examples.
    IsoTpAddress address;
    address.receiveId = 0x7E0;
    address.transmitId = 0x7E8;
    ecu.enableDiagnostics(address, IsoTpConfig{});

    const Result prepared = ecu.prepare(64);
    SCOPED_TRACE(::testing::Message() << std::string{prepared.message()});
    EXPECT_TRUE(prepared.succeeded());

    EXPECT_FALSE(ecu.isFaulted());
    EXPECT_TRUE(ecu.answersDiagnostics());
}

TEST(ExampleScriptTests, SequenceEngineLuaDeclaresItsTestCases)
{
    // Not an ECU: a test sequence, which is a different node with a different
    // vocabulary - test(), expect_frame(). It was the least covered of the five,
    // because nothing in the suite loads a sequence from a file at all.
    //
    // Running it to a verdict would need an engine ECU on the other end to pass
    // or fail against, which is a fixture this does not need: what breaks when
    // the API moves is the *loading*, and the count of declared cases is the
    // cheapest proof that the file was read and understood.
    const std::string source = readScript("sequence_engine.lua");

    LuaTestNode sequence{source, "sequence_engine.lua"};
    EXPECT_TRUE(sequence.prepare(64).succeeded());

    EXPECT_TRUE(sequence.declaredCases() > 0);
}

TEST(ExampleScriptTests, J1939EcuScriptsRunAndTransmitFrames)
{
    const std::vector<const char*> scripts{
        "ecu_engine.lua",
        "ecu_aftertreatment.lua",
        "ecu_body.lua",
        "ecu_brakes.lua",
        "ecu_switches.lua",
        "ecu_cluster_core.lua",
    };

    for (const char* script : scripts) {
        Outcome outcome;
        runEcu(script, 150ms, outcome);

        EXPECT_FALSE(outcome.faulted) << "Faulted script: " << script;
        EXPECT_GT(outcome.emitted, 0U) << "No frames emitted: " << script;
        SCOPED_TRACE(::testing::Message()
                     << "script errors in " << script << ": " << joined(outcome.errors));
        EXPECT_TRUE(outcome.errors.empty());
    }
}

TEST(ExampleScriptTests, TheExampleDatabasesDecodeWhatTheExampleScriptsSend)
{
    // docs/development/databases.md says the shipped databases "match what
    // examples/scripts puts on the bus", and that pairing is the entire point
    // of shipping both: import the database while the example runs and the
    // trace stops being hex. Nothing checked it.
    //
    // The check is the claim, made mechanical: run the script, take the
    // identifiers it actually emitted, and ask the database about each one.

    const auto decodesEverything = [](const char* script,
                                      const char* databaseName,
                                      const char* databaseFile) {
        Outcome outcome;
        runEcu(script,
               250ms,
               outcome,
               std::string_view{databaseName} == "none" ? nullptr : databaseName);

        CanDatabase database;
        const std::filesystem::path path =
            std::filesystem::path{TORQUEBUS_EXAMPLE_DATABASE_DIR} / databaseFile;

        const Result read = DbcParser::parseFile(path.string(), database);
        SCOPED_TRACE(::testing::Message() << "parsing " << databaseFile << ": " << read.message());
        EXPECT_TRUE(read.succeeded());
        EXPECT_TRUE(database.messageCount() > 0);

        const std::vector<CanFrame> frames = outcome.recorder.frames();
        EXPECT_FALSE(frames.empty());

        std::vector<std::uint32_t> undescribed;

        for (const CanFrame& frame : frames) {
            if (frame.direction != CanDirection::Tx) {
                continue;
            }
            if (database.find(frame) == nullptr) {
                undescribed.push_back(frame.identifier);
            }
        }

        std::sort(undescribed.begin(), undescribed.end());
        undescribed.erase(std::unique(undescribed.begin(), undescribed.end()), undescribed.end());

        return undescribed;
    };
    {
        const std::vector<std::uint32_t> undescribed =
            decodesEverything("ecu_vehicle.lua", "none", "vehicle.dbc");

        SCOPED_TRACE(::testing::Message()
                     << "identifiers with no message in vehicle.dbc: " << undescribed.size());
        EXPECT_TRUE(undescribed.empty());
    }
    {
        // The pairing databases.md does not spell out. vehicle.dbc carries 257
        // and 258, which are ecu_vehicle.lua's; ecu.dbc carries 1 and 255, and
        // 1 is ecu_motor.lua's module identifier. If this fails, the two files
        // are not the pair the documentation implies and one of them says so.
        const std::vector<std::uint32_t> undescribed =
            decodesEverything("ecu_motor.lua", "none", "ecu.dbc");

        SCOPED_TRACE(::testing::Message()
                     << "identifiers with no message in ecu.dbc: " << undescribed.size());
        EXPECT_TRUE(undescribed.empty());
    }
    {
        const std::vector<std::uint32_t> undescribed =
            decodesEverything("ecu_engine.lua", "none", "j1939.dbc");

        SCOPED_TRACE(::testing::Message()
                     << "identifiers with no message in j1939.dbc: " << undescribed.size());
        EXPECT_TRUE(undescribed.empty());
    }
}

TEST(ExampleScriptTests, EveryJ1939ScriptSendsOnlyMessagesTheJ1939DatabaseDescribes)
{
    // The check above asks the database about an identifier, which is how a DBC Decoder reads it
    // and is right for an 11-bit bus. A J1939 identifier carries the address of whoever sent it,
    // and the J1939 block - the one the project uses - finds a message by PGN, so that is the
    // question here: not "is 0x18FECA3D in the file" but "is a message with PGN 0xFECA in it".
    //
    // It is the question that found the body controller sending Lighting Data under a PGN the
    // database had never heard of, so that no lamp it drove ever reached the cluster.
    CanDatabase database;
    const std::filesystem::path path =
        std::filesystem::path{TORQUEBUS_EXAMPLE_DATABASE_DIR} / "j1939.dbc";
    ASSERT_TRUE(DbcParser::parseFile(path.string(), database).succeeded());

    std::set<std::uint32_t> described;
    for (const CanMessage& message : database.messages()) {
        if (message.format == CanFrameFormat::Extended) {
            described.insert(j1939Decompose(message.identifier).pgn());
        }
    }
    ASSERT_FALSE(described.empty());

    for (const char* script : {"ecu_engine.lua",
                               "ecu_aftertreatment.lua",
                               "ecu_body.lua",
                               "ecu_brakes.lua",
                               "ecu_switches.lua",
                               "ecu_cluster_core.lua"}) {
        SCOPED_TRACE(script);

        Outcome outcome;
        runEcu(script, 250ms, outcome);

        std::set<std::uint32_t> missing;
        std::size_t sent = 0;

        for (const CanFrame& frame : outcome.recorder.frames()) {
            if (frame.direction != CanDirection::Tx) {
                continue;
            }

            ++sent;
            EXPECT_TRUE(frame.isExtended()) << "0x" << std::hex << frame.identifier;
            EXPECT_EQ(frame.length, 8U);

            const std::uint32_t pgn = j1939Decompose(frame.identifier).pgn();
            if (described.count(pgn) == 0) {
                missing.insert(pgn);
            }
        }

        EXPECT_GT(sent, 0U);

        std::string names;
        for (const std::uint32_t pgn : missing) {
            names += std::format(" 0x{:X}", pgn);
        }
        SCOPED_TRACE("PGNs with no message in j1939.dbc:" + names);
        EXPECT_TRUE(missing.empty());
    }
}

TEST(ExampleScriptTests, EveryShippedExampleScriptIsCoveredHere)
{
    // The guard on this file itself. Adding a sixth example and forgetting to
    // run it would put the project straight back where it was, and nothing else
    // would notice - which is exactly how four of the five got here.
    const std::filesystem::path directory{TORQUEBUS_EXAMPLE_SCRIPT_DIR};
    EXPECT_TRUE(std::filesystem::is_directory(directory));

    const std::vector<std::string> covered{
        "ecu_aftertreatment.lua",
        "ecu_body.lua",
        "ecu_brakes.lua",
        "ecu_cluster_core.lua",
        "ecu_engine.lua",
        "ecu_motor.lua",
        "ecu_switches.lua",
        "ecu_uds.lua",
        "ecu_vehicle.lua",
        "ecu_vehicle_dbc.lua",
        "sequence_engine.lua",
    };

    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            found.push_back(entry.path().filename().string());
        }
    }

    for (const std::string& name : found) {
        SCOPED_TRACE(::testing::Message()
                     << "examples/scripts/" << name << " is not run by any test in this file");
        EXPECT_TRUE(std::find(covered.begin(), covered.end(), name) != covered.end());
    }

    EXPECT_TRUE(found.size() == covered.size());
}
