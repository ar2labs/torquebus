// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Editing a simulated ECU without stopping the measurement.
//
// Every case here is about the same promise, stated from a different side:
// **a script that fails to load leaves the running one alone.** A half-typed
// function is the normal state of a script being written, and the moment one
// can take an ECU off the bus, nobody edits during a measurement again - which
// would leave the feature technically present and practically unused.
//
// The clock cases wait on a real one, like LuaTimingTests, and for the same
// reason: the node reads steady_clock inside process() and there is nowhere to
// hand it a different one. Assertions are ranges.

#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/ScriptLibrary.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

class CollectNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.collect"; }
    [[nodiscard]] std::string displayName() const override { return "Collect"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
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
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

/// One scripted ECU with an editor attached, run in slices.
struct Bench final {
    PipelineGraph graph;
    ScriptLibrary library;
    LuaEcuNode* ecu{nullptr};
    CollectNode* collector{nullptr};

    std::vector<std::string> log;
    std::vector<std::string> errors;

    explicit Bench(const std::string& source)
    {
        auto node = std::make_unique<LuaEcuNode>(source, "reload.lua");
        ecu = node.get();
        ecu->setScriptLibrary(&library, "ecu_1");
        ecu->setLogHandler([this](const std::string& text, bool isError) {
            (isError ? errors : log).push_back(text);
        });

        const NodeId ecuId = graph.addNode(std::move(node));

        auto sink = std::make_unique<CollectNode>();
        collector = sink.get();
        const NodeId sinkId = graph.addNode(std::move(sink));

        EXPECT_TRUE(graph.connect(PortRef{ecuId, 0}, PortRef{sinkId, 0}).succeeded());
        EXPECT_TRUE(graph.compile().succeeded());
    }

    void run(int milliseconds)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};

        while (std::chrono::steady_clock::now() < until) {
            graph.execute();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    [[nodiscard]] std::size_t countOf(std::uint32_t identifier) const
    {
        std::size_t count = 0;

        for (const CanFrame& frame : collector->frames) {
            if (frame.identifier == identifier) {
                ++count;
            }
        }

        return count;
    }
};

} // namespace

TEST(LuaHotReloadTests, AnEditedScriptReplacesTheRunningOneOnTheNextPass)
{
    Bench bench{R"(
        function on_enable()
            cyclic(0x100, 10, "\1")
        end
    )"};

    bench.run(60);
    ASSERT_TRUE(bench.countOf(0x100) >= 2);

    bench.library.offer("ecu_1", R"(
        function on_enable()
            cyclic(0x200, 10, "\2")
        end
    )");

    bench.run(80);

    const std::size_t before = bench.countOf(0x100);
    EXPECT_TRUE(bench.countOf(0x200) >= 2);

    bench.run(60);

    // The old message stopped where the reload landed: its job belonged to the
    // interpreter that no longer exists.
    EXPECT_TRUE(bench.countOf(0x100) == before);

    const std::vector<ScriptReload> reports = bench.library.takeReports();
    ASSERT_TRUE(reports.size() == 1);
    EXPECT_TRUE(reports.front().nodeId == "ecu_1");
    EXPECT_TRUE(reports.front().succeeded);
    EXPECT_TRUE(reports.front().message.empty());
}

TEST(LuaHotReloadTests, AScriptThatDoesNotCompileLeavesTheRunningOneAlone)
{
    // The case the whole feature stands on. An edit is usually broken - that is
    // what editing is - and a syntax error must not take an ECU off the bus.
    Bench bench{R"(
        function on_enable()
            cyclic(0x300, 10, "\1")
        end
    )"};

    bench.run(60);
    const std::size_t before = bench.countOf(0x300);
    ASSERT_TRUE(before >= 2);

    bench.library.offer("ecu_1", "function on_enable( -- half typed");

    bench.run(80);

    // Still sending, at its own rate, with the frames it was declared with.
    EXPECT_TRUE(bench.countOf(0x300) > before + 1);

    const std::vector<ScriptReload> reports = bench.library.takeReports();
    ASSERT_TRUE(reports.size() == 1);
    EXPECT_FALSE(reports.front().succeeded);
    EXPECT_FALSE(reports.front().message.empty());

    // And it said so where somebody watching the trace would see it.
    EXPECT_FALSE(bench.errors.empty());
}

TEST(LuaHotReloadTests, AScriptThatThrowsInOnEnableLeavesTheRunningOneAlone)
{
    // Compiling is not loading. A script whose setup fails halfway has already
    // registered some of its timers, and those have to go with it rather than
    // being left running next to the old ones.
    Bench bench{R"(
        function on_enable()
            cyclic(0x400, 10, "\1")
        end
    )"};

    bench.run(60);
    const std::size_t before = bench.countOf(0x400);
    ASSERT_TRUE(before >= 2);

    bench.library.offer("ecu_1", R"(
        function on_enable()
            cyclic(0x500, 10, "\2")
            error("no")
        end
    )");

    bench.run(90);

    EXPECT_TRUE(bench.countOf(0x400) > before + 1);

    // Nothing from the script that failed: the timer it managed to declare died
    // with the interpreter it declared it in.
    EXPECT_TRUE(bench.countOf(0x500) == 0);

    const std::vector<ScriptReload> reports = bench.library.takeReports();
    ASSERT_TRUE(reports.size() == 1);
    EXPECT_FALSE(reports.front().succeeded);
}

TEST(LuaHotReloadTests, TheMeasurementClockSurvivesAReload)
{
    // A script reloaded at 40 seconds that suddenly believed it was at zero
    // would be a worse lie than no reload: every generator in the prelude is
    // arithmetic over this clock, and every one of them would jump.
    Bench bench{R"(
        function on_enable()
        end
    )"};

    bench.run(80);

    bench.library.offer("ecu_1", R"(
        function on_enable()
            emit(0x600, string.pack("<I4", math.floor(tb.now() * 1000)))
        end
    )");

    bench.run(40);

    ASSERT_TRUE(bench.countOf(0x600) == 1);

    const CanFrame& frame = bench.collector->frames.back();
    const std::uint32_t milliseconds = static_cast<std::uint32_t>(frame.data[0])
                                       | (static_cast<std::uint32_t>(frame.data[1]) << 8)
                                       | (static_cast<std::uint32_t>(frame.data[2]) << 16)
                                       | (static_cast<std::uint32_t>(frame.data[3]) << 24);

    SCOPED_TRACE(::testing::Message() << milliseconds);
    EXPECT_TRUE(milliseconds >= 60);
}

TEST(LuaHotReloadTests, WhatOnEnableEmitsReachesTheBusInThePassThatReloaded)
{
    // An ECU announces itself when it powers on. A reload is a power-on, and
    // dropping that first frame would be the silent-loss failure this project
    // refuses everywhere else.
    Bench bench{"function on_enable() end"};

    bench.run(20);

    bench.library.offer("ecu_1", R"(
        function on_enable()
            emit(0x700, "\xAA")
        end
    )");

    bench.run(20);

    ASSERT_TRUE(bench.countOf(0x700) == 1);
    EXPECT_TRUE(bench.collector->frames.back().data[0] == 0xAA);
}

TEST(LuaHotReloadTests, TheReplacedScriptGetsItsOnDisable)
{
    // Run against its own interpreter, after the new one has loaded - so a
    // teardown means what it meant while that script was running.
    Bench bench{R"(
        function on_enable()
            emit(0x800, "\1")
        end

        function on_disable()
            log_message("goodbye")
        end
    )"};

    bench.run(20);

    bench.library.offer("ecu_1", "function on_enable() end");
    bench.run(20);

    bool sawGoodbye = false;
    for (const std::string& line : bench.log) {
        if (line.find("goodbye") != std::string::npos) {
            sawGoodbye = true;
        }
    }

    EXPECT_TRUE(sawGoodbye);
}

TEST(LuaHotReloadTests, ARefusedReloadDoesNotRunTheOldScriptSOnDisable)
{
    // Nothing happened, so nothing should be torn down. An ECU that ran its
    // shutdown because somebody mistyped would be a fault injected by the tool.
    Bench bench{R"(
        function on_disable()
            log_message("goodbye")
        end
    )"};

    bench.run(20);

    bench.library.offer("ecu_1", "function on_enable( -- half typed");
    bench.run(30);

    for (const std::string& line : bench.log) {
        EXPECT_TRUE(line.find("goodbye") == std::string::npos);
    }
}

TEST(LuaHotReloadTests, AReloadRevivesAScriptThatHadFaulted)
{
    // The one that would be easy to get wrong: a node that stopped reading
    // offers after its error limit could only be revived by the restart this
    // feature exists to avoid - and a faulted ECU is exactly the one somebody
    // is about to edit.
    Bench bench{R"(
        function on_enable()
            every(2, function() error("always") end)
        end
    )"};

    bench.run(120);
    ASSERT_TRUE(bench.ecu->isFaulted());

    bench.library.offer("ecu_1", R"(
        function on_enable()
            cyclic(0x900, 10, "\1")
        end
    )");

    bench.run(80);

    EXPECT_FALSE(bench.ecu->isFaulted());
    EXPECT_TRUE(bench.countOf(0x900) >= 2);
}

TEST(LuaHotReloadTests, ReloadingSwapsTheParametersAndTheNodeSOwnGlobalsIn)
{
    // A reloaded script is set up by the same loader as one loaded at Start.
    // If it were not, the first thing to break would be the globals - and it
    // would break as "parameters is nil", ten minutes into somebody's session.
    Bench bench{"function on_enable() end"};

    bench.ecu->setScriptParameters({{"can_id", LuaValue::fromInteger(0xA00)}});

    bench.run(20);

    // node_name is "reload.lua" - ten bytes, which classic CAN refuses. Sliced
    // rather than sent whole, because the first version of this case sent it
    // whole, the emit failed, the reload was correctly refused, and the test
    // read as "parameters did not survive". The node was right and the test was
    // wrong, which is the failure mode worth leaving a note about.
    bench.library.offer("ecu_1", R"(
        function on_enable()
            emit(parameters.can_id, string.sub(node_name, 1, 6))
        end
    )");

    bench.run(30);

    // The parameters set on the node are still the ones the script reads, and
    // node_name is still defined.
    ASSERT_TRUE(bench.countOf(0xA00) == 1);
    EXPECT_TRUE(bench.collector->frames.back().length > 0);
}

TEST(LuaHotReloadTests, AnOfferReplacesOneNotYetTaken)
{
    // Somebody pressing Reload three times while typing means the last version.
    // Playing the intermediate ones through an ECU would be a slideshow of
    // half-finished edits.
    ScriptLibrary library;

    library.offer("ecu_1", "first");
    library.offer("ecu_1", "second");

    EXPECT_TRUE(library.isPending("ecu_1"));

    std::string taken;
    ASSERT_TRUE(library.take("ecu_1", taken));
    EXPECT_TRUE(taken == "second");

    EXPECT_FALSE(library.isPending("ecu_1"));
    EXPECT_FALSE(library.take("ecu_1", taken));
}

TEST(LuaHotReloadTests, ReportsAreTakenOnce)
{
    ScriptLibrary library;

    library.report(ScriptReload{"ecu_1", true, "", 5});
    library.report(ScriptReload{"ecu_2", false, "boom", 7});

    const std::vector<ScriptReload> first = library.takeReports();
    ASSERT_TRUE(first.size() == 2);
    EXPECT_TRUE(first[0].nodeId == "ecu_1");
    EXPECT_TRUE(first[1].message == "boom");

    EXPECT_TRUE(library.takeReports().empty());
}

TEST(LuaHotReloadTests, ANodeWithNoLibraryNeverLooksForOne)
{
    // The headless case, which is every test and every scripted run: a node
    // that was never given a library has to behave exactly as it did before
    // this feature existed.
    PipelineGraph graph;

    auto node = std::make_unique<LuaEcuNode>(R"(
        function on_enable()
            cyclic(0xB00, 10, "\1")
        end
    )",
                                             "plain.lua");
    LuaEcuNode* ecu = node.get();
    const NodeId ecuId = graph.addNode(std::move(node));

    auto sink = std::make_unique<CollectNode>();
    CollectNode* collector = sink.get();
    const NodeId sinkId = graph.addNode(std::move(sink));

    ASSERT_TRUE(graph.connect(PortRef{ecuId, 0}, PortRef{sinkId, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{60};
    while (std::chrono::steady_clock::now() < until) {
        graph.execute();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    EXPECT_FALSE(collector->frames.empty());
    EXPECT_FALSE(ecu->isFaulted());
}

TEST(LuaHotReloadTests, ReloadCountsBothOutcomes)
{
    // Two numbers rather than one: a refused reload changes nothing visible on
    // the bus, and somebody wondering why their edit did nothing should be able
    // to read the answer off the Statistics panel.
    Bench bench{"function on_enable() end"};

    bench.run(20);

    bench.library.offer("ecu_1", "function on_enable() end");
    bench.run(30);

    bench.library.offer("ecu_1", "function on_enable( -- half typed");
    bench.run(30);

    std::uint64_t reloaded = 0;
    std::uint64_t refused = 0;

    for (const NodeStatistic& statistic : bench.ecu->statistics()) {
        if (statistic.label == "Scripts reloaded") {
            reloaded = statistic.value;
        }
        if (statistic.label == "Reloads refused") {
            refused = statistic.value;
        }
    }

    EXPECT_TRUE(reloaded == 1);
    EXPECT_TRUE(refused == 1);
}

TEST(LuaHotReloadTests, AReloadedECUAnswersForTheIdentifiersItNowDeclares)
{
    // The trap: uds_did *adds* to the server, so a reload against the same one
    // would leave every identifier the old script declared still answering.
    // Deleting a DID from a script and having a tester go on reading it is the
    // kind of bug that gets blamed on the tester for an afternoon.
    LuaEcuNode node{R"(
        function on_enable()
            uds_did(0xF190, "OLD-VIN")
        end
    )",
                    "ecu.lua"};

    IsoTpAddress address;
    address.receiveId = 0x7E0;
    address.transmitId = 0x7E8;

    node.enableDiagnostics(address, IsoTpConfig{});

    ASSERT_TRUE(node.prepare(64).succeeded());
    ASSERT_TRUE(node.answersDiagnostics());

    ASSERT_TRUE(node.reload(R"(
        function on_enable()
            uds_did(0xF191, "NEW-PART")
        end
    )")
                    .succeeded());

    // Asked through the node's own server, which is what a tester reaches.
    const std::vector<std::uint8_t> readOld{0x22, 0xF1, 0x90};
    const std::vector<std::uint8_t> readNew{0x22, 0xF1, 0x91};

    UdsServer* server = node.diagnosticServer();
    ASSERT_TRUE(server != nullptr);

    const auto oldAnswer = server->handle(readOld, 0);
    const auto newAnswer = server->handle(readNew, 0);

    ASSERT_TRUE(oldAnswer.has_value());
    ASSERT_TRUE(newAnswer.has_value());

    // The identifier the old script declared is gone: refused, not answered.
    EXPECT_TRUE(oldAnswer->front() == 0x7F);
    EXPECT_TRUE(newAnswer->front() == 0x62);
}

TEST(LuaHotReloadTests, AReloadCanBeOfferedDirectlyWithoutTheLibrary)
{
    // reload() is the whole mechanism; the library is only how an editor in
    // another thread reaches it. Worth one case on its own, because it is the
    // form a test or a script-driven run would use.
    LuaEcuNode node{R"(
        function on_enable()
            cyclic(0xC00, 10, "\1")
        end
    )",
                    "direct.lua"};

    ASSERT_TRUE(node.prepare(64).succeeded());

    const Result refused = node.reload("function on_enable( -- half typed");
    ASSERT_TRUE(refused.failed());

    // The source did not change, which is what "the running one is untouched"
    // means when read back rather than watched.
    EXPECT_TRUE(node.source().find("0xC00") != std::string::npos);

    const Result accepted = node.reload("function on_enable() end");
    ASSERT_TRUE(accepted.succeeded());
    EXPECT_TRUE(node.source() == "function on_enable() end");
}
