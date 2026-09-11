// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/testing/TestReport.h"

#include <utility>

namespace torquebus {

void TestReport::begin(std::size_t declaredCases)
{
    const std::lock_guard lock{m_mutex};

    m_results.clear();
    m_taken = 0;
    m_declared = declaredCases;
    m_started = true;
    m_complete = false;
}

void TestReport::add(TestCaseResult result)
{
    const std::lock_guard lock{m_mutex};
    m_results.push_back(std::move(result));
}

void TestReport::finish()
{
    const std::lock_guard lock{m_mutex};
    m_complete = true;
}

std::vector<TestCaseResult> TestReport::results() const
{
    const std::lock_guard lock{m_mutex};
    return m_results;
}

std::vector<TestCaseResult> TestReport::takeNew()
{
    const std::lock_guard lock{m_mutex};

    std::vector<TestCaseResult> fresh{m_results.begin() + static_cast<std::ptrdiff_t>(m_taken),
                                      m_results.end()};
    m_taken = m_results.size();

    return fresh;
}

TestSummary TestReport::summary() const
{
    const std::lock_guard lock{m_mutex};

    TestSummary summary;
    summary.started = m_started;
    summary.complete = m_complete;

    // Declared rather than recorded, so that a run halfway through reads "3 of
    // 8" instead of "3 of 3, all passed" - which is the same lie the `started`
    // flag exists to prevent, one level down.
    summary.total = m_declared;

    for (const TestCaseResult& result : m_results) {
        switch (result.outcome) {
        case TestOutcome::Passed:
            ++summary.passed;
            break;
        case TestOutcome::Failed:
            ++summary.failed;
            break;
        case TestOutcome::Errored:
            ++summary.errored;
            break;
        case TestOutcome::Running:
            break;
        }
    }

    return summary;
}

void TestReport::clear()
{
    const std::lock_guard lock{m_mutex};

    m_results.clear();
    m_taken = 0;
    m_declared = 0;
    m_started = false;
    m_complete = false;
}

} // namespace torquebus
