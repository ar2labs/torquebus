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

#include <catch2/catch_test_macros.hpp>

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

        REQUIRE(graph.connect(PortRef{sequenceId, 0}, PortRef{sinkId, 0}).succeeded());

        if (!ecuSource.empty()) {
            auto simulated = std::make_unique<LuaEcuNode>(ecuSource, "ecu.lua");
            ecu = simulated.get();
            const NodeId ecuId = graph.addNode(std::move(simulated));

            REQUIRE(graph.connect(PortRef{ecuId, 0}, PortRef{sequenceId, 0}).succeeded());
        }

        REQUIRE(graph.compile().succeeded());
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

TEST_CASE("A case that sees what it expected passes", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const TestSummary summary = bench.report.summary();
    CHECK(summary.started);
    CHECK(summary.complete);
    CHECK(summary.total == 1);
    CHECK(summary.passed == 1);
    CHECK(summary.failed == 0);

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 1);
    CHECK(results.front().name == "0x100 arrives");
    CHECK(results.front().outcome == TestOutcome::Passed);

    // Both checks are recorded, not only the failing ones - a report listing
    // only failures cannot be read as evidence that anything was checked.
    CHECK(results.front().checks.size() == 2);
}

TEST_CASE("A case that never sees its frame fails", "[lua][sequence]")
{
    // The one that matters most. A timeout that passed quietly would make every
    // green run meaningless.
    Bench bench{R"(
        test("0x200 arrives", function()
            expect_frame(0x200, { within = 40 })
        end)
    )"};

    bench.run(400);

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 1);
    CHECK(results.front().outcome == TestOutcome::Failed);
    CHECK(results.front().message.find("0x200") != std::string::npos);

    CHECK(bench.report.summary().failed == 1);
    CHECK(bench.report.summary().passed == 0);
}

TEST_CASE("Silence can be the thing under test", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 2);

    CHECK(results[0].outcome == TestOutcome::Passed);
    CHECK(results[1].outcome == TestOutcome::Failed);
}

TEST_CASE("A failing check ends its case and no other", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 3);

    CHECK(results[0].outcome == TestOutcome::Failed);
    CHECK(results[1].outcome == TestOutcome::Passed);
    CHECK(results[2].outcome == TestOutcome::Failed);

    // The case stopped where it failed: the second assertion never ran.
    CHECK(results[0].checks.size() == 1);

    // And the message says what was seen, not only that something was wrong.
    INFO(results[0].message);
    CHECK(results[0].message.find("2") != std::string::npos);
}

TEST_CASE("An error in the test is not a failure of the network", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 2);

    CHECK(results[0].outcome == TestOutcome::Errored);
    CHECK_FALSE(results[0].message.empty());
    CHECK(results[1].outcome == TestOutcome::Passed);

    const TestSummary summary = bench.report.summary();
    CHECK(summary.errored == 1);
    CHECK(summary.failed == 0);
    CHECK(summary.passed == 1);
}

TEST_CASE("A sequence that declares nothing says so", "[lua][sequence]")
{
    // "0 of 0 passed" is the most dangerous sentence a test report can print.
    Bench bench{"-- a file somebody emptied by accident"};

    bench.run(100);

    const TestSummary summary = bench.report.summary();

    CHECK(summary.started);
    CHECK(summary.total == 0);
    CHECK(summary.passed == 0);

    // And it was said out loud, in the error colour.
    CHECK_FALSE(bench.errors.empty());
}

TEST_CASE("Cases run one at a time, in the order they were declared", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 3);

    CHECK(results[0].name == "first");
    CHECK(results[1].name == "second");
    CHECK(results[2].name == "third");

    // And they did not overlap: each started after the one before finished.
    CHECK(results[0].finishedNs <= results[1].startedNs);
    CHECK(results[1].finishedNs <= results[2].startedNs);

    // The first case really did wait, rather than the framework running the
    // whole sequence inside one pass.
    CHECK(results[0].finishedNs - results[0].startedNs >= 15'000'000ULL);
}

TEST_CASE("A sequence can send, and what it sends leaves the node", "[lua][sequence]")
{
    Bench bench{R"(
        test("sends a request", function()
            send(0x7E0, "\x22\xF1\x90")
            wait(10)
        end)
    )"};

    bench.run(300);

    REQUIRE(bench.sequence->isComplete());
    REQUIRE(bench.collector->frames.size() == 1);

    const CanFrame& frame = bench.collector->frames.front();
    CHECK(frame.identifier == 0x7E0);
    CHECK(frame.length == 3);
    CHECK(frame.data[0] == 0x22);
    CHECK(frame.direction == CanDirection::Tx);
}

TEST_CASE("A filter keeps waiting for the frame the case meant", "[lua][sequence]")
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

    REQUIRE(bench.sequence->isComplete());

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 1);
    INFO(results.front().message);
    CHECK(results.front().outcome == TestOutcome::Passed);
}

TEST_CASE("A run stopped halfway does not read as passed", "[lua][sequence]")
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

    REQUIRE_FALSE(bench.sequence->isComplete());

    bench.graph.finish();

    const std::vector<TestCaseResult> results = bench.report.results();
    REQUIRE(results.size() == 1);
    CHECK(results.front().outcome == TestOutcome::Errored);

    const TestSummary summary = bench.report.summary();
    CHECK(summary.passed == 0);
    CHECK(summary.total == 1);
}

TEST_CASE("The report hands a panel only what is new", "[lua][sequence][report]")
{
    TestReport report;
    report.begin(3);

    TestCaseResult first;
    first.name = "first";
    first.outcome = TestOutcome::Passed;
    report.add(first);

    REQUIRE(report.takeNew().size() == 1);
    CHECK(report.takeNew().empty());

    TestCaseResult second;
    second.name = "second";
    second.outcome = TestOutcome::Failed;
    report.add(second);

    const std::vector<TestCaseResult> fresh = report.takeNew();
    REQUIRE(fresh.size() == 1);
    CHECK(fresh.front().name == "second");

    // results() still has both: takeNew is a cursor, not a queue.
    CHECK(report.results().size() == 2);

    // Total is what was *declared*, so a run halfway through reads "1 of 3"
    // rather than "1 of 2, all passed".
    const TestSummary summary = report.summary();
    CHECK(summary.total == 3);
    CHECK(summary.passed == 1);
    CHECK(summary.failed == 1);
    CHECK_FALSE(summary.complete);
}

TEST_CASE("An empty report is not a passing report", "[lua][sequence][report]")
{
    TestReport report;

    const TestSummary summary = report.summary();

    CHECK_FALSE(summary.started);
    CHECK_FALSE(summary.complete);
    CHECK(summary.total == 0);
}

TEST_CASE("A sequence reads its parameters like any other script", "[lua][sequence]")
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
    REQUIRE(bench.graph.compile().succeeded());

    bench.run(300);

    REQUIRE(bench.sequence->isComplete());
    REQUIRE_FALSE(bench.collector->frames.empty());
    CHECK(bench.collector->frames.back().identifier == 0x123);
}

TEST_CASE("A sequence that will not compile fails the graph", "[lua][sequence]")
{
    // Finding out that a test file has a typo *after* setting up the bench is
    // the wrong moment.
    LuaTestNode node{"test('unclosed", "sequence.lua"};

    const Result result = node.prepare(64);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("sequence.lua") != std::string::npos);
}

TEST_CASE("test() refuses a case with no body", "[lua][sequence]")
{
    // Refused where it is written, at Start, rather than when the runner
    // reaches it - by which time the bench is set up and somebody is watching.
    LuaTestNode node{"test('no body')", "sequence.lua"};

    const Result result = node.prepare(64);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("function") != std::string::npos);
}
