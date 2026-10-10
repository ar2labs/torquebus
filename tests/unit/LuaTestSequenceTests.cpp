// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Tests for the thing that runs tests, which is worth being careful about: a
// test framework that reports a pass it did not earn is worse than no framework
// at all, because somebody signs off on it.
//
// So most of what is checked here is the *negative* side. That a failure is
// reported as a failure. That a case which never got its frame does not quietly
// pass. That a sequence declaring nothing says so instead of reading green.
// That an error in the test itself is not filed as a failure of the network.
//
// These wait on a real clock, like LuaTimingTests, because the node reads
// steady_clock inside process(). Every timeout here is tens of milliseconds and
// every assertion is a range.

#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/LuaTestNode.h"
#include "core/testing/TestReport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// Collects whatever the sequence sent.
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

/// A sequence, optionally with a scripted ECU producing traffic for it.
///
/// The ECU feeds the sequence rather than the other way round, because the
/// graph refuses cycles - which is the right refusal and means that a test
/// which both sends and waits for the answer runs across a real channel. That
/// arrangement is exercised by hand; what is exercised here is everything else.
struct Bench final {
    PipelineGraph graph;
    TestReport report;

    LuaTestNode* sequence{nullptr};
    LuaEcuNode* ecu{nullptr};
    CollectNode* collector{nullptr};

    std::vector<std::string> log;
    std::vector<std::string> errors;

    Bench(const std::string& source, const std::string& ecuSource = {})
    {
        auto node = std::make_unique<LuaTestNode>(source, "sequence.lua");
        sequence = node.get();
        sequence->setReport(&report);
        sequence->setLogHandler([this](const std::string& text, bool isError) {
            (isError ? errors : log).push_back(text);
        });

        const NodeId sequenceId = graph.addNode(std::move(node));

        auto sink = std::make_unique<CollectNode>();
        collector = sink.get();
        const NodeId sinkId = graph.addNode(std::move(sink));

        EXPECT_TRUE(graph.connect(PortRef{sequenceId, 0}, PortRef{sinkId, 0}).succeeded());

        if (!ecuSource.empty()) {
            auto simulated = std::make_unique<LuaEcuNode>(ecuSource, "ecu.lua");
            ecu = simulated.get();
            const NodeId ecuId = graph.addNode(std::move(simulated));

            EXPECT_TRUE(graph.connect(PortRef{ecuId, 0}, PortRef{sequenceId, 0}).succeeded());
        }

        EXPECT_TRUE(graph.compile().succeeded());
    }

    void run(int milliseconds)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};

        while (std::chrono::steady_clock::now() < until && !sequence->isComplete()) {
            graph.execute();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
};

} // namespace

TEST(LuaTestSequenceTests, ACaseThatSeesWhatItExpectedPasses)
{
    Bench bench{R"(
        test("0x100 arrives", function()
            local frame = expect_frame(0x100, { within = 300 })
            assert_equal(frame.data, "\xAA\xBB", "payload")
        end)
    )",
                R"(
        function on_enable()
            cyclic(0x100, 20, "\xAA\xBB")
        end
    )"};

    bench.run(500);

    ASSERT_TRUE(bench.sequence->isComplete());

    const TestSummary summary = bench.report.summary();
    EXPECT_TRUE(summary.started);
    EXPECT_TRUE(summary.complete);
    EXPECT_TRUE(summary.total == 1);
    EXPECT_TRUE(summary.passed == 1);
    EXPECT_TRUE(summary.failed == 0);

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 1);
    EXPECT_TRUE(results.front().name == "0x100 arrives");
    EXPECT_TRUE(results.front().outcome == TestOutcome::Passed);

    // Both checks are recorded, not only the failing ones - a report listing
    // only failures cannot be read as evidence that anything was checked.
    EXPECT_TRUE(results.front().checks.size() == 2);
}

TEST(LuaTestSequenceTests, ACaseThatNeverSeesItsFrameFails)
{
    // The one that matters most. A timeout that passed quietly would make every
    // green run meaningless.
    Bench bench{R"(
        test("0x200 arrives", function()
            expect_frame(0x200, { within = 40 })
        end)
    )"};

    bench.run(400);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 1);
    EXPECT_TRUE(results.front().outcome == TestOutcome::Failed);
    EXPECT_TRUE(results.front().message.find("0x200") != std::string::npos);

    EXPECT_TRUE(bench.report.summary().failed == 1);
    EXPECT_TRUE(bench.report.summary().passed == 0);
}

TEST(LuaTestSequenceTests, SilenceCanBeTheThingUnderTest)
{
    // The assertion most tools cannot express, and the one fault injection
    // exists to provoke: nothing on this identifier for this long.
    Bench bench{R"(
        test("0x300 stays quiet", function()
            expect_silence(0x300, 40)
        end)

        test("0x100 does not", function()
            expect_silence(0x100, 60)
        end)
    )",
                R"(
        function on_enable()
            cyclic(0x100, 10, "\1")
        end
    )"};

    bench.run(600);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 2);

    EXPECT_TRUE(results[0].outcome == TestOutcome::Passed);
    EXPECT_TRUE(results[1].outcome == TestOutcome::Failed);
}

TEST(LuaTestSequenceTests, AFailingCheckEndsItsCaseAndNoOther)
{
    // One requirement not being met says nothing about the others. A run that
    // stopped at the first failure would have to be repeated once per bug.
    Bench bench{R"(
        test("first", function()
            assert_equal(1, 2, "one is two")
            assert_true(false, "never reached")
        end)

        test("second", function()
            assert_true(true, "still runs")
        end)

        test("third", function()
            fail("deliberate")
        end)
    )"};

    bench.run(400);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 3);

    EXPECT_TRUE(results[0].outcome == TestOutcome::Failed);
    EXPECT_TRUE(results[1].outcome == TestOutcome::Passed);
    EXPECT_TRUE(results[2].outcome == TestOutcome::Failed);

    // The case stopped where it failed: the second assertion never ran.
    EXPECT_TRUE(results[0].checks.size() == 1);

    // And the message says what was seen, not only that something was wrong.
    SCOPED_TRACE(::testing::Message() << results[0].message);
    EXPECT_TRUE(results[0].message.find("2") != std::string::npos);
}

TEST(LuaTestSequenceTests, AnErrorInTheTestIsNotAFailureOfTheNetwork)
{
    // Different outcomes because they need different people: a failure is about
    // the bus, an error is about the test. A tool that conflates them sends
    // somebody to the bench to debug a nil index.
    Bench bench{R"(
        test("broken", function()
            local nothing = nil
            return nothing.field
        end)

        test("fine", function()
            assert_true(true, "ok")
        end)
    )"};

    bench.run(400);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 2);

    EXPECT_TRUE(results[0].outcome == TestOutcome::Errored);
    EXPECT_FALSE(results[0].message.empty());
    EXPECT_TRUE(results[1].outcome == TestOutcome::Passed);

    const TestSummary summary = bench.report.summary();
    EXPECT_TRUE(summary.errored == 1);
    EXPECT_TRUE(summary.failed == 0);
    EXPECT_TRUE(summary.passed == 1);
}

TEST(LuaTestSequenceTests, ASequenceThatDeclaresNothingSaysSo)
{
    // "0 of 0 passed" is the most dangerous sentence a test report can print.
    Bench bench{"-- a file somebody emptied by accident"};

    bench.run(100);

    const TestSummary summary = bench.report.summary();

    EXPECT_TRUE(summary.started);
    EXPECT_TRUE(summary.total == 0);
    EXPECT_TRUE(summary.passed == 0);

    // And it was said out loud, in the error colour.
    EXPECT_FALSE(bench.errors.empty());
}

TEST(LuaTestSequenceTests, CasesRunOneAtATimeInTheOrderTheyWereDeclared)
{
    // A network is a shared thing. Two cases driving it at once would make each
    // one's result depend on the other's timing, which is the property a test
    // exists not to have.
    Bench bench{R"(
        test("first", function()
            wait(20)
            log("first")
        end)

        test("second", function()
            log("second")
        end)

        test("third", function()
            wait(20)
            log("third")
        end)
    )"};

    bench.run(500);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 3);

    EXPECT_TRUE(results[0].name == "first");
    EXPECT_TRUE(results[1].name == "second");
    EXPECT_TRUE(results[2].name == "third");

    // And they did not overlap: each started after the one before finished.
    EXPECT_TRUE(results[0].finishedNs <= results[1].startedNs);
    EXPECT_TRUE(results[1].finishedNs <= results[2].startedNs);

    // The first case really did wait, rather than the framework running the
    // whole sequence inside one pass.
    EXPECT_TRUE(results[0].finishedNs - results[0].startedNs >= 15'000'000ULL);
}

TEST(LuaTestSequenceTests, ASequenceCanSendAndWhatItSendsLeavesTheNode)
{
    Bench bench{R"(
        test("sends a request", function()
            send(0x7E0, "\x22\xF1\x90")
            wait(10)
        end)
    )"};

    bench.run(300);

    ASSERT_TRUE(bench.sequence->isComplete());
    ASSERT_TRUE(bench.collector->frames.size() == 1);

    const CanFrame& frame = bench.collector->frames.front();
    EXPECT_TRUE(frame.identifier == 0x7E0);
    EXPECT_TRUE(frame.length == 3);
    EXPECT_TRUE(frame.data[0] == 0x22);
    EXPECT_TRUE(frame.direction == CanDirection::Tx);
}

TEST(LuaTestSequenceTests, AFilterKeepsWaitingForTheFrameTheCaseMeant)
{
    // `where` narrows what counts, and the deadline belongs to the wait rather
    // than to each candidate - a filter that reset the clock on every frame it
    // rejected would turn a busy bus into an infinite wait.
    Bench bench{R"(
        test("the second byte is what matters", function()
            local frame = expect(0x100, { within = 400, where = function(f)
                return f.data:byte(1) == 0x02
            end })

            assert_true(frame ~= nil, "a frame with 0x02 arrives")
            assert_equal(frame.data:byte(1), 0x02, "first byte")
        end)
    )",
                R"(
        local next_value = tb.counter(2)

        function on_enable()
            cyclic(0x100, 10, function()
                return string.char(next_value())
            end)
        end
    )"};

    bench.run(700);

    ASSERT_TRUE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 1);
    SCOPED_TRACE(::testing::Message() << results.front().message);
    EXPECT_TRUE(results.front().outcome == TestOutcome::Passed);
}

TEST(LuaTestSequenceTests, ARunStoppedHalfwayDoesNotReadAsPassed)
{
    // Somebody presses Stop while a case is waiting. The case did not pass - it
    // did not finish - and a report that dropped it would be missing exactly
    // the case they stopped to look at.
    Bench bench{R"(
        test("waits for something that never comes", function()
            expect_frame(0x999, { within = 5000 })
        end)
    )"};

    bench.run(60);

    ASSERT_FALSE(bench.sequence->isComplete());

    bench.graph.finish();

    const std::vector<TestCaseResult> results = bench.report.results();
    ASSERT_TRUE(results.size() == 1);
    EXPECT_TRUE(results.front().outcome == TestOutcome::Errored);

    const TestSummary summary = bench.report.summary();
    EXPECT_TRUE(summary.passed == 0);
    EXPECT_TRUE(summary.total == 1);
}

TEST(LuaTestSequenceTests, TheReportHandsAPanelOnlyWhatIsNew)
{
    TestReport report;
    report.begin(3);

    TestCaseResult first;
    first.name = "first";
    first.outcome = TestOutcome::Passed;
    report.add(first);

    ASSERT_TRUE(report.takeNew().size() == 1);
    EXPECT_TRUE(report.takeNew().empty());

    TestCaseResult second;
    second.name = "second";
    second.outcome = TestOutcome::Failed;
    report.add(second);

    const std::vector<TestCaseResult> fresh = report.takeNew();
    ASSERT_TRUE(fresh.size() == 1);
    EXPECT_TRUE(fresh.front().name == "second");

    // results() still has both: takeNew is a cursor, not a queue.
    EXPECT_TRUE(report.results().size() == 2);

    // Total is what was *declared*, so a run halfway through reads "1 of 3"
    // rather than "1 of 2, all passed".
    const TestSummary summary = report.summary();
    EXPECT_TRUE(summary.total == 3);
    EXPECT_TRUE(summary.passed == 1);
    EXPECT_TRUE(summary.failed == 1);
    EXPECT_FALSE(summary.complete);
}

TEST(LuaTestSequenceTests, AnEmptyReportIsNotAPassingReport)
{
    TestReport report;

    const TestSummary summary = report.summary();

    EXPECT_FALSE(summary.started);
    EXPECT_FALSE(summary.complete);
    EXPECT_TRUE(summary.total == 0);
}

TEST(LuaTestSequenceTests, ASequenceReadsItsParametersLikeAnyOtherScript)
{
    Bench bench{R"(
        test("the identifier comes from the block", function()
            send(parameters.request_id, "\x01")
            wait(5)
        end)
    )"};

    bench.sequence->setScriptParameters({{"request_id", LuaValue::fromInteger(0x123)}});

    // Set after construction, so the graph has to be prepared again for the
    // node to see them - which is what compile() does.
    ASSERT_TRUE(bench.graph.compile().succeeded());

    bench.run(300);

    ASSERT_TRUE(bench.sequence->isComplete());
    ASSERT_FALSE(bench.collector->frames.empty());
    EXPECT_TRUE(bench.collector->frames.back().identifier == 0x123);
}

TEST(LuaTestSequenceTests, ASequenceThatWillNotCompileFailsTheGraph)
{
    // Finding out that a test file has a typo *after* setting up the bench is
    // the wrong moment.
    LuaTestNode node{"test('unclosed", "sequence.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("sequence.lua") != std::string::npos);
}

TEST(LuaTestSequenceTests, TestRefusesACaseWithNoBody)
{
    // Refused where it is written, at Start, rather than when the runner
    // reaches it - by which time the bench is set up and somebody is watching.
    LuaTestNode node{"test('no body')", "sequence.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("function") != std::string::npos);
}

TEST(LuaTestSequenceTests, ASequenceKeepsTheOptionsOfItsSend)
{
    // send(id, data, { extended = ... }) is in the testing guide, and the J1939 helpers once took
    // its third argument for a source address: the call raised an error about a bitwise operation
    // on a string, from a line that had been correct all along.
    Bench bench{R"(
        test("sends with options", function()
            send(0x123, "\x01", { extended = true })
            send(0x7E0, "\x02")
            wait(10)
        end)
    )"};

    bench.run(300);

    ASSERT_TRUE(bench.sequence->isComplete());
    EXPECT_TRUE(bench.errors.empty());
    ASSERT_EQ(bench.collector->frames.size(), 2U);

    EXPECT_EQ(bench.collector->frames[0].identifier, 0x123U);
    EXPECT_TRUE(bench.collector->frames[0].isExtended());
    EXPECT_EQ(bench.collector->frames[1].identifier, 0x7E0U);
    EXPECT_FALSE(bench.collector->frames[1].isExtended());
}

TEST(LuaTestSequenceTests, ASequenceCanSendAJ1939Message)
{
    Bench bench{R"(
        test("requests a PGN", function()
            j1939_send(6, 0xEA00, 0xF9, "\x00\xF0\x00")
            wait(10)
        end)
    )"};

    bench.run(300);

    ASSERT_TRUE(bench.sequence->isComplete());
    EXPECT_TRUE(bench.errors.empty());
    ASSERT_EQ(bench.collector->frames.size(), 1U);

    const CanFrame& frame = bench.collector->frames.front();
    EXPECT_EQ(frame.identifier, 0x18EA00F9U);
    EXPECT_TRUE(frame.isExtended());
    EXPECT_EQ(frame.length, 8U); // padded, as J1939 pads
    EXPECT_EQ(frame.data[1], 0xF0U);
    EXPECT_EQ(frame.data[3], 0xFFU);
}
