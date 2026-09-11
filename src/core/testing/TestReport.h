// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a test sequence found, on its way from the executor to a panel.
//
// A simulation that only *runs* answers one question: does anything crash. The
// question worth asking is whether the network behaved - whether the answer
// came within 200 ms, whether the counter incremented, whether the ECU went
// quiet when its supply was cut. A person watching a trace can see all of that
// and cannot see it a hundred times in a row, which is precisely when it
// matters.
//
// So a sequence writes here and a panel reads. The shape is the diagnostic
// console's, with one deliberate difference: **appending a result blocks.**
//
// The executor never waits on the GUI *on the frame path* - that rule stands,
// and everything on the frame path here is lock-free. But a case result happens
// once per test case, not once per frame: a whole sequence writes a few dozen
// times. Dropping one to save a microsecond would mean a report that is missing
// the failure somebody was looking for, which is worse than no report at all,
// because it reads as a pass.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace torquebus {

/// One check inside a case: what was compared, and what was seen.
///
/// Kept even when it passed. A report that lists only failures cannot be read
/// as evidence that anything was actually checked - and "17 checks, all passed"
/// is the sentence somebody signs off on.
struct TestCheck final {
    std::string description;
    bool passed{false};

    /// Empty on a pass. On a failure: what was expected and what arrived.
    std::string detail;
};

enum class TestOutcome : std::uint8_t {
    /// Started and still running. One case at a time, so at most one of these.
    Running,

    Passed,

    /// A check failed. The sequence carries on with the next case: one
    /// requirement not being met says nothing about the others, and a run that
    /// stopped at the first failure would have to be repeated once per bug.
    Failed,

    /// The script itself threw - a nil index, a bad argument. Separate from
    /// Failed because they need different people: a failure is about the
    /// network under test, an error is about the test.
    Errored,
};

struct TestCaseResult final {
    std::string name;
    TestOutcome outcome{TestOutcome::Running};

    /// The failing check, or the Lua error. Empty on a pass.
    std::string message;

    /// Nanoseconds on the measurement clock, so a result lines up with the
    /// trace beside it. That alignment is the point: "it failed at 12.480 s"
    /// and a trace showing 12.480 s are one investigation rather than two.
    std::uint64_t startedNs{0};
    std::uint64_t finishedNs{0};

    std::vector<TestCheck> checks;
};

/// Where a run stands as a whole.
struct TestSummary final {
    std::size_t total{0};
    std::size_t passed{0};
    std::size_t failed{0};
    std::size_t errored{0};

    /// True once every declared case has finished.
    bool complete{false};

    /// The sequence never started - no test block in the graph, or a script
    /// that declared nothing. Distinguished from "0 of 0 passed", which reads
    /// like a green run and is the most dangerous thing a test report can say.
    bool started{false};
};

class TestReport final {
public:
    TestReport() = default;

    TestReport(const TestReport&) = delete;
    TestReport& operator=(const TestReport&) = delete;
    TestReport(TestReport&&) = delete;
    TestReport& operator=(TestReport&&) = delete;

    // --- The executor's side ----------------------------------------------

    /// Forgets the last run and marks this one started. Called at Start.
    void begin(std::size_t declaredCases);

    /// Records one finished case.
    void add(TestCaseResult result);

    /// Marks the run finished. Called when the last case returns.
    void finish();

    // --- The panel's side --------------------------------------------------

    /// Every case recorded so far, oldest first.
    [[nodiscard]] std::vector<TestCaseResult> results() const;

    /// Cases recorded since the last call to this, so a panel can append
    /// rather than rebuild its table every 50 ms.
    [[nodiscard]] std::vector<TestCaseResult> takeNew();

    [[nodiscard]] TestSummary summary() const;

    /// Everything back to before a run. Called when a measurement is built, so
    /// the panel does not show the last run's failures next to this one's.
    void clear();

private:
    mutable std::mutex m_mutex;

    std::vector<TestCaseResult> m_results;

    /// How many of m_results a panel has already been handed.
    std::size_t m_taken{0};

    std::size_t m_declared{0};
    bool m_started{false};
    bool m_complete{false};
};

} // namespace torquebus
