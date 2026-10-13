// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A script that does not return.
//
// Everything else the scripting layer does about errors rests on one thing: that the
// script gets back to the caller, with a failure or without. A loop that never ends
// does not. It freezes the thread that runs the pipeline - and Stop, which waits for
// that thread - or, at load, the window that pressed Start; and a table that grows
// until the machine swaps takes the application's memory with it.
//
// So the runtime gives every call a deadline and every script a ceiling on memory,
// and these tests hold it to both. They are written so that a failure of the feature
// is a failed assertion and not a hung test: each loop that is meant to be cut short
// is given a budget of a few tens of milliseconds, and the assertion is that it was.

#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/LuaRuntime.h"
#include "core/scripting/LuaTestNode.h"
#include "core/testing/TestReport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

/// A runtime with the standard libraries and short limits, so that a loop meant to be
/// cut short is cut short quickly.
std::unique_ptr<LuaRuntime> quickRuntime()
{
    auto lua = std::make_unique<LuaRuntime>();
    EXPECT_TRUE(lua->openLibraries().succeeded());

    LuaLimits limits;
    limits.loadBudget = 150ms;
    limits.callBudget = 50ms;
    limits.memoryBytes = 8U * 1024U * 1024U;
    lua->setLimits(limits);

    return lua;
}

[[nodiscard]] bool mentions(const Result& result, std::string_view text)
{
    return std::string{result.message()}.find(text) != std::string::npos;
}

/// One frame per pass. At namespace scope because a class defined in a function may
/// not have a static data member, and the port list is one.
class OneFrame final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.one"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    void process(NodeContext& context) override
    {
        frame.identifier = 0x100;
        frame.length = 1;
        frame.dlc = 1;
        context.publish<CanFrame>(0, std::span<const CanFrame>{&frame, 1});
    }

    CanFrame frame;

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

} // namespace

TEST(LuaLimitsTests, AnEndlessLoopInAHandlerIsAnErrorAndNotAHang)
{
    auto lua = quickRuntime();
    ASSERT_TRUE(lua->load("function spin()\n  while true do end\nend\n", "ecu.lua").succeeded());

    const auto started = std::chrono::steady_clock::now();
    const Result result = lua->call("spin");
    const auto took = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(lua->lastFailureWasALimit());
    EXPECT_LT(took, 2s) << "the loop was not interrupted";

    // The line that was looping, and what the limit was - not "an error".
    EXPECT_TRUE(mentions(result, "ecu.lua:2")) << result.message();
    EXPECT_TRUE(mentions(result, "50 ms")) << result.message();
}

TEST(LuaLimitsTests, AnEndlessLoopAtTheTopOfAScriptStopsStartRatherThanFreezingIt)
{
    // Loading runs the file, on the thread that pressed Start. This is the one that
    // would have frozen the window.
    auto lua = quickRuntime();

    const auto started = std::chrono::steady_clock::now();
    const Result result = lua->load("local n = 0\nwhile true do n = n + 1 end\n", "top.lua");
    const auto took = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(lua->lastFailureWasALimit());
    EXPECT_LT(took, 2s);
    EXPECT_TRUE(mentions(result, "top.lua:2")) << result.message();
    EXPECT_TRUE(mentions(result, "150 ms")) << result.message();
}

TEST(LuaLimitsTests, ALoopInsideACoroutineIsCaughtToo)
{
    // A coroutine is a thread of its own to Lua, and a hook belongs to a thread. The
    // test sequences run their cases in coroutines, so this is the common shape of
    // "a case that never finishes".
    auto lua = quickRuntime();
    ASSERT_TRUE(lua->load(R"(
        function spin()
          local co = coroutine.wrap(function() while true do end end)
          co()
        end
    )",
                          "co.lua")
                    .succeeded());

    const Result result = lua->call("spin");

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(lua->lastFailureWasALimit());
}

TEST(LuaLimitsTests, ATableThatGrowsWithoutEndIsAnErrorInsideTheScript)
{
    auto lua = quickRuntime();

    // Time is not what this one is about, and a loop that has to fill megabytes should
    // not lose a race with a deadline on a slow build.
    LuaLimits limits = lua->limits();
    limits.callBudget = 20s;
    limits.memoryBytes = 4U * 1024U * 1024U;
    lua->setLimits(limits);

    ASSERT_TRUE(lua->load(R"(
        function grow()
          local t = {}
          while true do t[#t + 1] = string.rep("x", 1024) end
        end
        function fine() return 7 end
    )",
                          "mem.lua")
                    .succeeded());

    const Result result = lua->call("grow");

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(lua->lastFailureWasALimit());
    EXPECT_TRUE(mentions(result, "not enough memory")) << result.message();
    EXPECT_TRUE(mentions(result, "4 MiB")) << result.message();

    // The interpreter is still there afterwards. It was the script that was refused,
    // and the script's garbage is garbage as soon as the call has unwound.
    LuaValue answer;
    ASSERT_TRUE(lua->call("fine", {}, answer).succeeded());
    EXPECT_EQ(answer.integer, 7);
    EXPECT_FALSE(lua->lastFailureWasALimit());
}

TEST(LuaLimitsTests, AMemoryRefusalTheScriptCaughtItselfIsNotHeldAgainstTheNextCall)
{
    // pcall around an allocation that did not fit is the script handling the limit, and that is
    // over with the call. Left remembered, the next failure of any kind - a nil indexed, a typo -
    // would be reported as running out of memory, and would stop the ECU for it at once.
    auto lua = quickRuntime();

    LuaLimits limits = lua->limits();
    limits.callBudget = 20s;
    limits.memoryBytes = 4U * 1024U * 1024U;
    lua->setLimits(limits);

    ASSERT_TRUE(lua->load(R"(
        function grab_and_catch()
          local ok = pcall(function()
            local t = {}
            while true do t[#t + 1] = string.rep("x", 1024) end
          end)
          collectgarbage()
          return ok and 1 or 0
        end
        function oops() local t = nil; return t.x end
    )",
                          "caught.lua")
                    .succeeded());

    LuaValue answer;
    ASSERT_TRUE(lua->call("grab_and_catch", {}, answer).succeeded());
    EXPECT_EQ(answer.integer, 0) << "the allocation was meant to be refused";

    const Result result = lua->call("oops");

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(lua->lastFailureWasALimit()) << result.message();
    EXPECT_FALSE(mentions(result, "MiB")) << result.message();
}

TEST(LuaLimitsTests, AScriptThatIsMerelyBusyIsNotInterrupted)
{
    // The hook costs a clock read every couple of thousand instructions, and has to
    // leave alone everything that finishes. A few hundred thousand iterations is far
    // more than a handler does.
    auto lua = quickRuntime();

    LuaLimits limits = lua->limits();
    limits.callBudget = 10s;
    lua->setLimits(limits);

    ASSERT_TRUE(lua->load(R"(
        function work()
          local sum = 0
          for i = 1, 300000 do sum = sum + i % 7 end
          return sum
        end
    )",
                          "busy.lua")
                    .succeeded());

    LuaValue answer;
    ASSERT_TRUE(lua->call("work", {}, answer).succeeded());
    EXPECT_FALSE(lua->lastFailureWasALimit());
}

TEST(LuaLimitsTests, AnOrdinaryErrorIsNotMistakenForALimit)
{
    // The two are told apart because they are handled differently: a script that ran
    // out of time is stopped now, one that divided by nil gets the usual few chances.
    auto lua = quickRuntime();
    ASSERT_TRUE(lua->load("function oops() local t = nil; return t.x end", "oops.lua").succeeded());

    const Result result = lua->call("oops");

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(lua->lastFailureWasALimit());
}

TEST(LuaLimitsTests, TheBudgetIsPerCallAndStartsAgainForTheNext)
{
    // Not a total for the run: an ECU that is called a hundred thousand times over an
    // hour spends far more than its budget altogether, and each call spends nothing.
    auto lua = quickRuntime();
    ASSERT_TRUE(lua->load(R"(
        function spin() while true do end end
        function quick() return 1 end
    )",
                          "per-call.lua")
                    .succeeded());

    ASSERT_TRUE(lua->call("spin").failed());

    for (int i = 0; i < 20; ++i) {
        LuaValue answer;
        ASSERT_TRUE(lua->call("quick", {}, answer).succeeded()) << "call " << i;
    }
}

TEST(LuaLimitsTests, ALimitHitInsideAnEcuStopsItAtOnceAndLeavesTheRestAlone)
{
    // Through the node, which is what the pipeline sees: the first frame sends the
    // script into its loop, the limit ends the call, and the ECU is out of the run -
    // not five errors later, each of them a quarter of a second long.
    auto ecu = std::make_unique<LuaEcuNode>(
        "function on_message(id, data, ch, ext) while true do end end", "loop.lua");

    std::vector<std::string> lines;
    ecu->setLogHandler([&lines](const std::string& text, bool) { lines.push_back(text); });

    LuaEcuNode* ecuPointer = ecu.get();

    // A source with one frame per pass, and the ECU after it.
    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<OneFrame>());
    const NodeId node = graph.addNode(std::move(ecu));
    ASSERT_TRUE(graph.connect({source, 0}, {node, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    EXPECT_TRUE(ecuPointer->isFaulted()) << "the ECU kept being called after it ran out of time";

    // And the pass after it is quick, because the script is no longer called.
    const auto started = std::chrono::steady_clock::now();
    graph.execute();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);

    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.front().find("loop.lua"), std::string::npos) << lines.front();
}

TEST(LuaLimitsTests, DefaultsAreGenerousEnoughForAnythingAHandlerReallyDoes)
{
    // Pinned so that tightening them is a decision and not an edit: a handler runs per
    // frame inside a pass of milliseconds, and these are what "generous" was meant as.
    const LuaLimits limits;

    EXPECT_GE(limits.callBudget, 100ms);
    EXPECT_GE(limits.loadBudget, limits.callBudget);
    EXPECT_GE(limits.memoryBytes, 16U * 1024U * 1024U);
}

// ---------------------------------------------------------------------------
// What a script may declare
// ---------------------------------------------------------------------------

TEST(LuaLimitsTests, AnEcuCannotDeclareTimersWithoutEnd)
{
    // every() inside on_message adds a job each time a frame is heard, to a list that is
    // walked in full on every pass. Each pass is slower than the last until the pipeline
    // cannot keep up.
    auto ecu = std::make_unique<LuaEcuNode>(R"(
        function on_message(id, data)
          every(1000, function() end)
        end
    )",
                                            "leaky.lua");

    std::vector<std::string> lines;
    ecu->setLogHandler([&lines](const std::string& text, bool) { lines.push_back(text); });

    LuaEcuNode* ecuPointer = ecu.get();

    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<OneFrame>());
    const NodeId node = graph.addNode(std::move(ecu));
    ASSERT_TRUE(graph.connect({source, 0}, {node, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    // More passes than the limit, one frame each.
    for (int pass = 0; pass < 1200 && !ecuPointer->isFaulted(); ++pass) {
        graph.execute();
    }

    // The 1025th timer was refused, which is an error in the script, five of which stop it:
    // what matters is that the list did not go on growing.
    bool refusedWithTheLimit = false;
    for (const std::string& line : lines) {
        refusedWithTheLimit = refusedWithTheLimit || line.find("1024") != std::string::npos;
    }

    EXPECT_TRUE(refusedWithTheLimit) << "no message named the limit";
    EXPECT_TRUE(ecuPointer->isFaulted());
}

TEST(LuaLimitsTests, ACyclicMessageWithAnIdentifierThatIsNotOneIsRefusedAtTheCall)
{
    // emit() always checked. cyclic() did not, and a message with a number no frame can
    // carry was sent on every cycle for as long as the measurement ran, each one counted
    // as a failed transmit.
    for (const char* declaration : {"cyclic(-1, 100, 'x')",
                                    "cyclic(0x20000000, 100, 'x')",
                                    "cyclic(0x800, 100, 'x', {extended = false})"}) {
        LuaEcuNode ecu{std::string{"function on_enable() "} + declaration + " end", "bad-id.lua"};

        const Result prepared = ecu.prepare(16);

        EXPECT_TRUE(prepared.failed()) << declaration;
    }

    // And the same identifiers where they fit are fine.
    for (const char* declaration : {"cyclic(0x7FF, 100, 'x')",
                                    "cyclic(0x800, 100, 'x')",
                                    "cyclic(0x1FFFFFFF, 100, 'x')",
                                    "cyclic(0x100, 100, 'x', {extended = true})"}) {
        LuaEcuNode ecu{std::string{"function on_enable() "} + declaration + " end", "good-id.lua"};

        EXPECT_TRUE(ecu.prepare(16).succeeded()) << declaration;
    }
}

TEST(LuaLimitsTests, AnIdentifierThatDoesNotFitIsAScriptErrorAndNotACrashOfTheApplication)
{
    // Lua's own formatter knows %s %d %I %f %p %c %U and nothing else. It keeps an unknown
    // conversion as text and does not consume its argument, so the message
    //     "emit: identifier 0x%X does not fit a %s frame"
    // took the identifier for the string of the %s that followed, and read it from address
    // 0x20000000. A script that wrote emit(0x20000000, ...) - or any identifier that does not
    // fit - ended the application with an access violation, in the dispatch thread, where
    // nothing could catch it. The same message was in the test sequence's send().
    for (const char* call : {"emit(0x20000000, 'x')",
                             "emit(0x800, 'x', {extended = false})",
                             "emit(-1, 'x')",
                             "cyclic(0x800, 100, 'x', {extended = false})"}) {
        LuaEcuNode ecu{std::string{"function on_enable() "} + call + " end", "bad-id.lua"};

        Result prepared = Result::ok();
        EXPECT_NO_THROW(prepared = ecu.prepare(16)) << call;

        ASSERT_TRUE(prepared.failed()) << call;
        EXPECT_NE(std::string{prepared.message()}.find("does not fit"), std::string::npos)
            << call << ": " << prepared.message();
    }
}

TEST(LuaLimitsTests, ASequenceThatSendsAnImpossibleIdentifierFailsItsCaseAndDoesNotCrash)
{
    LuaTestNode sequence{R"(
        test("sends nonsense", function()
            send(0x20000000, "x")
        end)
    )",
                         "bad-send.lua"};

    TestReport report;
    sequence.setReport(&report);

    std::vector<std::string> lines;
    sequence.setLogHandler([&lines](const std::string& text, bool) { lines.push_back(text); });

    ASSERT_TRUE(sequence.prepare(16).succeeded());

    // Run it to the end: the call raises inside the case, which the framework reports as an
    // error in that case.
    PortBatch outputs[1]{};
    NodeContext context{{}, outputs};

    for (int pass = 0; pass < 50 && !sequence.isComplete(); ++pass) {
        EXPECT_NO_THROW(sequence.process(context));
    }

    ASSERT_TRUE(sequence.isComplete());

    bool namedTheIdentifier = false;
    for (const TestCaseResult& result : report.results()) {
        namedTheIdentifier =
            namedTheIdentifier || result.message.find("does not fit") != std::string::npos;
    }
    EXPECT_TRUE(namedTheIdentifier);
}
