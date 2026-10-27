// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Test & homologation dossier export in interactive HTML, vector PDF, and Markdown.

#pragma once

#include "core/can/CanFrame.h"
#include "core/testing/TestReport.h"

#include <string>
#include <vector>

namespace torquebus {

/// Metadata included in the executive dossier header.
struct DossierMetadata final {
    std::string title{"TorqueBus Test & Homologation Dossier"};
    std::string tester{"TorqueBus Automated Test Suite"};
    std::string systemUnderTest{"CAN Bus Network / ECU SUT"};
    std::string environment{"Bench Hardware-in-the-Loop / Simulation"};
    std::string notes;
    std::string timestamp;
};

class TestReportExport final {
public:
    /// Builds a standalone, interactive HTML dossier with modern styling, executive KPIs,
    /// execution timeline SVG, collapsible test case cards with check tables, captured CAN trace
    /// snippets, associated signal waveforms (SVG), filter buttons and search.
    [[nodiscard]] static std::string buildHtml(const TestReport& report,
                                               const DossierMetadata& meta = {});
    [[nodiscard]] static std::string buildHtml(const std::vector<TestCaseResult>& results,
                                               const TestSummary& summary,
                                               const DossierMetadata& meta = {});

    /// Builds a clean semantic HTML report optimized for QTextDocument and PDF vector printing.
    [[nodiscard]] static std::string buildPrintHtml(const TestReport& report,
                                                    const DossierMetadata& meta = {});
    [[nodiscard]] static std::string buildPrintHtml(const std::vector<TestCaseResult>& results,
                                                    const TestSummary& summary,
                                                    const DossierMetadata& meta = {});

    /// Builds a comprehensive Markdown report.
    [[nodiscard]] static std::string buildMarkdown(const TestReport& report);
    [[nodiscard]] static std::string buildMarkdown(const std::vector<TestCaseResult>& results,
                                                   const TestSummary& summary);

    /// Generates a standalone SVG Gantt timeline visualizing test case executions and durations.
    [[nodiscard]] static std::string generateTimelineSvg(const std::vector<TestCaseResult>& results,
                                                         double width = 800.0,
                                                         double height = 0.0);

    /// Generates a standalone SVG chart plotting recorded signal waveforms over time.
    [[nodiscard]] static std::string
    generateSignalChartSvg(const std::vector<TestSignalPoint>& signalPoints,
                           double width = 720.0,
                           double height = 240.0);

    /// Formats nanoseconds to a human-readable duration string (e.g. "12.45 ms" or "1.520 s").
    [[nodiscard]] static std::string formatDuration(std::uint64_t nanoseconds);

    /// Formats nanoseconds to seconds string (e.g. "12.480").
    [[nodiscard]] static std::string formatSeconds(std::uint64_t nanoseconds);

    /// Helper to escape XML/HTML special characters (&, <, >, ", ').
    [[nodiscard]] static std::string escapeHtml(const std::string& text);
};

} // namespace torquebus
