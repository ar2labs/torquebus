// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/testing/TestReportExport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <iomanip>
#include <map>
#include <sstream>

namespace torquebus {
namespace {

constexpr const char* kColorPass = "#10b981"; // Emerald
constexpr const char* kColorFail = "#ef4444"; // Crimson
constexpr const char* kColorError = "#f59e0b"; // Amber
constexpr const char* kColorRunning = "#64748b"; // Slate
constexpr const char* kColorBackground = "#0f172a"; // Dark Slate
constexpr const char* kColorCard = "#1e293b";
constexpr const char* kColorBorder = "#334155";
constexpr const char* kColorText = "#f8fafc";
constexpr const char* kColorTextMuted = "#94a3b8";

const char* outcomeText(TestOutcome outcome)
{
    switch (outcome) {
    case TestOutcome::Passed:
        return "PASS";
    case TestOutcome::Failed:
        return "FAIL";
    case TestOutcome::Errored:
        return "ERROR";
    case TestOutcome::Running:
        return "RUNNING";
    }
    return "UNKNOWN";
}

const char* outcomeColor(TestOutcome outcome)
{
    switch (outcome) {
    case TestOutcome::Passed:
        return kColorPass;
    case TestOutcome::Failed:
        return kColorFail;
    case TestOutcome::Errored:
        return kColorError;
    case TestOutcome::Running:
        return kColorRunning;
    }
    return kColorRunning;
}

std::string formatHexBytes(const CanFrame& frame)
{
    std::ostringstream ss;
    const std::size_t size = frame.payloadSize();
    for (std::size_t i = 0; i < size; ++i) {
        if (i > 0) {
            ss << ' ';
        }
        ss << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned>(frame.data[i]);
    }
    return ss.str();
}

std::string formatAsciiBytes(const CanFrame& frame)
{
    std::string ascii;
    const std::size_t size = frame.payloadSize();
    ascii.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t byte = frame.data[i];
        if (byte >= 0x20 && byte <= 0x7E) {
            ascii += static_cast<char>(byte);
        } else {
            ascii += '.';
        }
    }
    return ascii;
}

} // namespace

std::string TestReportExport::escapeHtml(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string TestReportExport::formatSeconds(std::uint64_t nanoseconds)
{
    const double seconds = static_cast<double>(nanoseconds) / 1'000'000'000.0;
    return std::format("{:.3f}", seconds);
}

std::string TestReportExport::formatDuration(std::uint64_t nanoseconds)
{
    if (nanoseconds < 1'000'000) {
        const double us = static_cast<double>(nanoseconds) / 1'000.0;
        return std::format("{:.1f} \xC2\xB5s", us); // µs in UTF-8
    }
    if (nanoseconds < 1'000'000'000) {
        const double ms = static_cast<double>(nanoseconds) / 1'000'000.0;
        return std::format("{:.2f} ms", ms);
    }
    const double s = static_cast<double>(nanoseconds) / 1'000'000'000.0;
    return std::format("{:.3f} s", s);
}

std::string TestReportExport::generateTimelineSvg(const std::vector<TestCaseResult>& results,
                                                  double width,
                                                  double height)
{
    if (results.empty()) {
        return {};
    }

    std::uint64_t minStart = results.front().startedNs;
    std::uint64_t maxFinish = results.front().finishedNs;

    for (const auto& r : results) {
        if (r.startedNs < minStart) {
            minStart = r.startedNs;
        }
        if (r.finishedNs > maxFinish) {
            maxFinish = r.finishedNs;
        }
    }

    const std::uint64_t totalSpanNs = (maxFinish > minStart) ? (maxFinish - minStart) : 1'000'000;
    const double rowHeight = 30.0;
    const double topMargin = 32.0;
    const double bottomMargin = 20.0;
    const double leftMargin = 220.0;
    const double rightMargin = 60.0;

    const double computedHeight =
        (height > 0.0) ? height : (topMargin + bottomMargin + results.size() * rowHeight);
    const double chartWidth = std::max(50.0, width - leftMargin - rightMargin);

    std::ostringstream ss;
    ss << std::format(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {:.1f} {:.1f}\" width=\"100%\" "
        "height=\"{:.1f}\" style=\"font-family: -apple-system, Segoe UI, sans-serif; "
        "background: #111827; border-radius: 8px; border: 1px solid #374151;\">\n",
        width,
        computedHeight,
        computedHeight);

    // Grid lines & Time Ticks
    constexpr int kTicks = 5;
    for (int t = 0; t <= kTicks; ++t) {
        const double fraction = static_cast<double>(t) / kTicks;
        const double x = leftMargin + fraction * chartWidth;
        const std::uint64_t timeAtTick =
            static_cast<std::uint64_t>(fraction * static_cast<double>(totalSpanNs));

        ss << std::format(
            "<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" stroke=\"#374151\" "
            "stroke-dasharray=\"3,3\" stroke-width=\"1\" />\n",
            x,
            topMargin - 5.0,
            x,
            computedHeight - bottomMargin);

        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"10\" fill=\"#9ca3af\" "
                          "text-anchor=\"middle\">{}</text>\n",
                          x,
                          topMargin - 10.0,
                          formatDuration(timeAtTick));
    }

    // Rows
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const double y = topMargin + i * rowHeight;
        const double barY = y + 5.0;
        const double barHeight = rowHeight - 10.0;

        const std::uint64_t relStart = (r.startedNs >= minStart) ? (r.startedNs - minStart) : 0;
        const std::uint64_t relEnd =
            (r.finishedNs >= minStart) ? (r.finishedNs - minStart) : relStart;
        const std::uint64_t duration = (relEnd >= relStart) ? (relEnd - relStart) : 0;

        const double barX = leftMargin + (static_cast<double>(relStart) / totalSpanNs) * chartWidth;
        const double barW =
            std::max(5.0, (static_cast<double>(duration) / totalSpanNs) * chartWidth);

        // Case name
        std::string nameLabel = r.name;
        if (nameLabel.size() > 32) {
            nameLabel = nameLabel.substr(0, 30) + "..";
        }

        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"12\" fill=\"#e5e7eb\" "
                          "text-anchor=\"end\">{}</text>\n",
                          leftMargin - 12.0,
                          barY + barHeight - 4.0,
                          escapeHtml(nameLabel));

        // Duration bar
        ss << std::format(
            "<rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"{:.1f}\" height=\"{:.1f}\" rx=\"4\" ry=\"4\" "
            "fill=\"{}\" opacity=\"0.9\" />\n",
            barX,
            barY,
            barW,
            barHeight,
            outcomeColor(r.outcome));

        // Duration label
        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"10\" fill=\"#9ca3af\" "
                          "text-anchor=\"start\">{}</text>\n",
                          barX + barW + 6.0,
                          barY + barHeight - 4.0,
                          formatDuration(duration));
    }

    ss << "</svg>\n";
    return ss.str();
}

std::string TestReportExport::generateSignalChartSvg(
    const std::vector<TestSignalPoint>& signalPoints, double width, double height)
{
    if (signalPoints.empty()) {
        return {};
    }

    // Group signals by name
    std::map<std::string, std::vector<TestSignalPoint>> series;
    std::uint64_t minTime = signalPoints.front().timestampNs;
    std::uint64_t maxTime = signalPoints.front().timestampNs;
    double minVal = signalPoints.front().value;
    double maxVal = signalPoints.front().value;

    for (const auto& pt : signalPoints) {
        series[pt.name].push_back(pt);
        if (pt.timestampNs < minTime) {
            minTime = pt.timestampNs;
        }
        if (pt.timestampNs > maxTime) {
            maxTime = pt.timestampNs;
        }
        if (pt.value < minVal) {
            minVal = pt.value;
        }
        if (pt.value > maxVal) {
            maxVal = pt.value;
        }
    }

    if (maxTime == minTime) {
        maxTime = minTime + 1'000'000; // 1 ms fallback
    }
    if (std::abs(maxVal - minVal) < 1e-6) {
        minVal -= 1.0;
        maxVal += 1.0;
    } else {
        const double pad = (maxVal - minVal) * 0.08;
        minVal -= pad;
        maxVal += pad;
    }

    const double leftMargin = 55.0;
    const double rightMargin = 140.0; // Space for legend
    const double topMargin = 25.0;
    const double bottomMargin = 30.0;

    const double plotWidth = std::max(40.0, width - leftMargin - rightMargin);
    const double plotHeight = std::max(40.0, height - topMargin - bottomMargin);

    const std::uint64_t timeSpanNs = maxTime - minTime;
    const double valSpan = maxVal - minVal;

    std::ostringstream ss;
    ss << std::format(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {:.1f} {:.1f}\" width=\"100%\" "
        "height=\"{:.1f}\" style=\"font-family: -apple-system, Segoe UI, sans-serif; "
        "background: #1e293b; border-radius: 6px; border: 1px solid #334155;\">\n",
        width,
        height,
        height);

    // Grid lines (horizontal)
    constexpr int kYGrid = 4;
    for (int i = 0; i <= kYGrid; ++i) {
        const double frac = static_cast<double>(i) / kYGrid;
        const double y = topMargin + (1.0 - frac) * plotHeight;
        const double v = minVal + frac * valSpan;

        ss << std::format(
            "<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" stroke=\"#334155\" "
            "stroke-dasharray=\"2,2\" />\n",
            leftMargin,
            y,
            leftMargin + plotWidth,
            y);

        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"10\" fill=\"#94a3b8\" "
                          "text-anchor=\"end\">{:.2f}</text>\n",
                          leftMargin - 6.0,
                          y + 3.0,
                          v);
    }

    // Grid lines (vertical)
    constexpr int kXGrid = 4;
    for (int i = 0; i <= kXGrid; ++i) {
        const double frac = static_cast<double>(i) / kXGrid;
        const double x = leftMargin + frac * plotWidth;
        const std::uint64_t t = static_cast<std::uint64_t>(frac * static_cast<double>(timeSpanNs));

        ss << std::format(
            "<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" stroke=\"#334155\" "
            "stroke-dasharray=\"2,2\" />\n",
            x,
            topMargin,
            x,
            topMargin + plotHeight);

        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"10\" fill=\"#94a3b8\" "
                          "text-anchor=\"middle\">{}</text>\n",
                          x,
                          topMargin + plotHeight + 16.0,
                          formatDuration(t));
    }

    // Signal Colors
    const std::vector<std::string> colors = {
        "#38bdf8", // Sky blue
        "#f43f5e", // Rose
        "#10b981", // Emerald
        "#a855f7", // Purple
        "#f59e0b", // Amber
        "#06b6d4" // Cyan
    };

    std::size_t colorIdx = 0;
    double legendY = topMargin + 10.0;

    for (const auto& [name, pts] : series) {
        const std::string& color = colors[colorIdx % colors.size()];
        ++colorIdx;

        // Polyline points
        std::ostringstream ptsSs;
        for (std::size_t j = 0; j < pts.size(); ++j) {
            const auto& pt = pts[j];
            const double x =
                leftMargin
                + (static_cast<double>(pt.timestampNs - minTime) / timeSpanNs) * plotWidth;
            const double y = topMargin + (1.0 - (pt.value - minVal) / valSpan) * plotHeight;

            if (j > 0) {
                ptsSs << ' ';
            }
            ptsSs << std::format("{:.1f},{:.1f}", x, y);
        }

        // Draw line
        ss << std::format(
            "<polyline fill=\"none\" stroke=\"{}\" stroke-width=\"2\" points=\"{}\" />\n",
            color,
            ptsSs.str());

        // Draw point dots
        for (const auto& pt : pts) {
            const double x =
                leftMargin
                + (static_cast<double>(pt.timestampNs - minTime) / timeSpanNs) * plotWidth;
            const double y = topMargin + (1.0 - (pt.value - minVal) / valSpan) * plotHeight;
            ss << std::format(
                "<circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"3\" fill=\"{}\" />\n", x, y, color);
        }

        // Legend entry
        const double lastVal = pts.empty() ? 0.0 : pts.back().value;
        ss << std::format("<rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"10\" height=\"10\" rx=\"2\" "
                          "fill=\"{}\" />\n",
                          leftMargin + plotWidth + 12.0,
                          legendY - 8.0,
                          color);

        ss << std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"11\" fill=\"#e2e8f0\">{}: "
                          "<tspan font-weight=\"bold\">{:.2f}</tspan></text>\n",
                          leftMargin + plotWidth + 26.0,
                          legendY,
                          escapeHtml(name),
                          lastVal);

        legendY += 18.0;
    }

    ss << "</svg>\n";
    return ss.str();
}

std::string TestReportExport::buildHtml(const TestReport& report, const DossierMetadata& meta)
{
    return buildHtml(report.results(), report.summary(), meta);
}

std::string TestReportExport::buildHtml(const std::vector<TestCaseResult>& results,
                                        const TestSummary& summary,
                                        const DossierMetadata& meta)
{
    std::ostringstream ss;

    const std::size_t passed = summary.passed;
    const std::size_t failed = summary.failed;
    const std::size_t errored = summary.errored;
    const std::size_t total = summary.total > 0 ? summary.total : results.size();
    const double passRate = (total > 0) ? (static_cast<double>(passed) / total) * 100.0 : 0.0;
    const bool isGreen = (failed == 0 && errored == 0 && passed > 0);

    // Compute total duration
    std::uint64_t totalDurationNs = 0;
    for (const auto& r : results) {
        if (r.finishedNs > r.startedNs) {
            totalDurationNs += (r.finishedNs - r.startedNs);
        }
    }

    ss << "<!DOCTYPE html>\n";
    ss << "<html lang=\"en\">\n";
    ss << "<head>\n";
    ss << "<meta charset=\"UTF-8\">\n";
    ss << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    ss << "<title>" << escapeHtml(meta.title) << "</title>\n";
    ss << "<style>\n";
    ss << R"CSS(
        :root {
            --bg: #0b0f19;
            --surface: #131b2e;
            --surface-hover: #1e293b;
            --border: #24324d;
            --text: #f1f5f9;
            --text-muted: #94a3b8;
            --pass: #10b981;
            --fail: #ef4444;
            --error: #f59e0b;
            --accent: #38bdf8;
            --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--bg);
            color: var(--text);
            line-height: 1.5;
            padding: 24px;
        }
        .container { max-width: 1200px; margin: 0 auto; }
        header {
            display: flex;
            justify-content: space-between;
            align-items: flex-start;
            border-bottom: 1px solid var(--border);
            padding-bottom: 20px;
            margin-bottom: 24px;
        }
        .header-title h1 { font-size: 24px; font-weight: 700; color: #fff; margin-bottom: 6px; }
        .meta-list { display: flex; flex-wrap: wrap; gap: 16px; font-size: 13px; color: var(--text-muted); }
        .verdict-badge {
            padding: 10px 20px;
            border-radius: 8px;
            font-size: 18px;
            font-weight: 700;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }
        .verdict-pass { background: rgba(16, 185, 129, 0.15); color: var(--pass); border: 1px solid var(--pass); }
        .verdict-fail { background: rgba(239, 68, 68, 0.15); color: var(--fail); border: 1px solid var(--fail); }

        /* KPI Cards */
        .kpi-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }
        .kpi-card {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 16px;
        }
        .kpi-label { font-size: 12px; font-weight: 600; text-transform: uppercase; color: var(--text-muted); margin-bottom: 4px; }
        .kpi-value { font-size: 26px; font-weight: 700; color: #fff; }

        /* Timeline */
        .section-title { font-size: 16px; font-weight: 600; margin-bottom: 12px; display: flex; align-items: center; gap: 8px; }
        .timeline-card {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 24px;
        }

        /* Filter Toolbar */
        .toolbar {
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 12px;
            margin-bottom: 16px;
        }
        .filter-group { display: flex; gap: 8px; }
        .btn {
            background: var(--surface);
            border: 1px solid var(--border);
            color: var(--text);
            padding: 6px 14px;
            border-radius: 6px;
            font-size: 13px;
            cursor: pointer;
            transition: all 0.15s;
        }
        .btn:hover { background: var(--surface-hover); border-color: var(--accent); }
        .btn.active { background: var(--accent); color: #000; font-weight: 600; border-color: var(--accent); }
        .search-input {
            background: var(--surface);
            border: 1px solid var(--border);
            color: #fff;
            padding: 6px 14px;
            border-radius: 6px;
            font-size: 13px;
            width: 260px;
        }

        /* Case Card */
        .case-card {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            margin-bottom: 12px;
            overflow: hidden;
            transition: border-color 0.2s;
        }
        .case-card:hover { border-color: #3b82f6; }
        details > summary {
            list-style: none;
            padding: 14px 18px;
            display: flex;
            align-items: center;
            gap: 12px;
            cursor: pointer;
            background: rgba(255,255,255,0.02);
            user-select: none;
        }
        details > summary::-webkit-details-marker { display: none; }
        .badge {
            font-size: 11px;
            font-weight: 700;
            padding: 3px 8px;
            border-radius: 4px;
            text-transform: uppercase;
        }
        .badge-pass { background: rgba(16, 185, 129, 0.2); color: var(--pass); }
        .badge-fail { background: rgba(239, 68, 68, 0.2); color: var(--fail); }
        .badge-error { background: rgba(245, 158, 11, 0.2); color: var(--error); }
        .badge-running { background: rgba(100, 116, 139, 0.2); color: var(--text-muted); }
        .case-name { font-weight: 600; font-size: 15px; flex-grow: 1; }
        .case-time { font-size: 12px; color: var(--text-muted); font-family: var(--font-mono); }

        .case-content {
            padding: 16px 18px;
            border-top: 1px solid var(--border);
            display: flex;
            flex-direction: column;
            gap: 16px;
        }
        .failure-alert {
            background: rgba(239, 68, 68, 0.1);
            border-left: 4px solid var(--fail);
            padding: 10px 14px;
            border-radius: 4px;
            font-size: 13px;
            color: #fca5a5;
            font-family: var(--font-mono);
        }

        /* Tables */
        table { width: 100%; border-collapse: collapse; font-size: 13px; }
        th { text-align: left; padding: 8px 10px; background: rgba(255,255,255,0.04); color: var(--text-muted); font-weight: 600; border-bottom: 1px solid var(--border); }
        td { padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.05); }
        .mono { font-family: var(--font-mono); }
        .trace-container { max-height: 280px; overflow-y: auto; border: 1px solid var(--border); border-radius: 6px; }

        @media print {
            body { background: #fff !important; color: #000 !important; padding: 0 !important; }
            .no-print { display: none !important; }
            .kpi-card, .timeline-card, .case-card { background: #fff !important; border: 1px solid #ccc !important; color: #000 !important; }
            .kpi-value, .case-name { color: #000 !important; }
            details { display: block !important; }
            .case-card { break-inside: avoid; page-break-inside: avoid; }
        }
    )CSS";
    ss << "</style>\n";
    ss << "</head>\n";
    ss << "<body>\n";
    ss << "<div class=\"container\">\n";

    // Header
    ss << "<header>\n";
    ss << "  <div class=\"header-title\">\n";
    ss << "    <h1>" << escapeHtml(meta.title) << "</h1>\n";
    ss << "    <div class=\"meta-list\">\n";
    ss << "      <div><strong>System:</strong> " << escapeHtml(meta.systemUnderTest) << "</div>\n";
    ss << "      <div><strong>Tester:</strong> " << escapeHtml(meta.tester) << "</div>\n";
    ss << "      <div><strong>Env:</strong> " << escapeHtml(meta.environment) << "</div>\n";
    if (!meta.notes.empty()) {
        ss << "      <div><strong>Notes:</strong> " << escapeHtml(meta.notes) << "</div>\n";
    }
    ss << "    </div>\n";
    ss << "  </div>\n";
    ss << std::format("  <div class=\"verdict-badge {}\">{} ({:.1f}%)</div>\n",
                      isGreen ? "verdict-pass" : "verdict-fail",
                      isGreen ? "PASSED" : "FAILED",
                      passRate);
    ss << "</header>\n";

    // KPI Summary Cards
    ss << "<div class=\"kpi-grid\">\n";
    ss << std::format("  <div class=\"kpi-card\"><div class=\"kpi-label\">Total Cases</div><div "
                      "class=\"kpi-value\">{}</div></div>\n",
                      total);
    ss << std::format(
        "  <div class=\"kpi-card\"><div class=\"kpi-label\">Passed</div><div class=\"kpi-value\" "
        "style=\"color: var(--pass);\">{}</div></div>\n",
        passed);
    ss << std::format(
        "  <div class=\"kpi-card\"><div class=\"kpi-label\">Failed</div><div class=\"kpi-value\" "
        "style=\"color: var(--fail);\">{}</div></div>\n",
        failed);
    ss << std::format(
        "  <div class=\"kpi-card\"><div class=\"kpi-label\">Errored</div><div class=\"kpi-value\" "
        "style=\"color: var(--error);\">{}</div></div>\n",
        errored);
    ss << std::format("  <div class=\"kpi-card\"><div class=\"kpi-label\">Pass Rate</div><div "
                      "class=\"kpi-value\">{:.1f}%</div></div>\n",
                      passRate);
    ss << std::format(
        "  <div class=\"kpi-card\"><div class=\"kpi-label\">Duration</div><div class=\"kpi-value\" "
        "style=\"font-size: 20px;\">{}</div></div>\n",
        formatDuration(totalDurationNs));
    ss << "</div>\n";

    // Execution Timeline
    if (!results.empty()) {
        ss << "<div class=\"timeline-card\">\n";
        ss << "  <div class=\"section-title\">Execution Timeline</div>\n";
        ss << generateTimelineSvg(results);
        ss << "</div>\n";
    }

    // Filter Toolbar
    ss << "<div class=\"toolbar no-print\">\n";
    ss << "  <div class=\"filter-group\">\n";
    ss << std::format(
        "    <button class=\"btn active\" onclick=\"filterStatus('all')\">All ({})</button>\n",
        results.size());
    ss << std::format(
        "    <button class=\"btn\" onclick=\"filterStatus('failed')\">Failed ({})</button>\n",
        failed + errored);
    ss << std::format(
        "    <button class=\"btn\" onclick=\"filterStatus('passed')\">Passed ({})</button>\n",
        passed);
    ss << "    <button class=\"btn\" onclick=\"toggleAll(true)\">Expand All</button>\n";
    ss << "    <button class=\"btn\" onclick=\"toggleAll(false)\">Collapse All</button>\n";
    ss << "  </div>\n";
    ss << "  <input type=\"text\" id=\"search\" class=\"search-input\" placeholder=\"Filter case "
          "or check...\" onkeyup=\"applyFilters()\">\n";
    ss << "</div>\n";

    // Test Cases
    ss << "<div class=\"cases-container\">\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const char* statusClass = (r.outcome == TestOutcome::Passed)    ? "pass"
                                  : (r.outcome == TestOutcome::Failed)  ? "fail"
                                  : (r.outcome == TestOutcome::Errored) ? "error"
                                                                        : "running";
        const std::uint64_t dur = (r.finishedNs > r.startedNs) ? (r.finishedNs - r.startedNs) : 0;
        const bool open = (r.outcome != TestOutcome::Passed);

        ss << std::format("<div class=\"case-card\" data-status=\"{}\">\n", statusClass);
        ss << std::format("  <details {}>\n", open ? "open" : "");
        ss << "    <summary>\n";
        ss << std::format("      <span class=\"badge badge-{}\">{}</span>\n",
                          statusClass,
                          outcomeText(r.outcome));
        ss << std::format("      <span class=\"case-name\">{}</span>\n", escapeHtml(r.name));
        ss << std::format("      <span class=\"case-time\">{} | {}</span>\n",
                          formatSeconds(r.startedNs) + "s",
                          formatDuration(dur));
        ss << "    </summary>\n";
        ss << "    <div class=\"case-content\">\n";

        if (!r.message.empty()) {
            ss << std::format("      <div class=\"failure-alert\"><strong>Failure "
                              "Detail:</strong> {}</div>\n",
                              escapeHtml(r.message));
        }

        // Checks table
        if (!r.checks.empty()) {
            ss << "      <div>\n";
            ss << "        <div class=\"section-title\">Assertions &amp; Checks ("
               << r.checks.size() << ")</div>\n";
            ss << "        <table>\n";
            ss << "          <thead><tr><th style=\"width: 80px;\">Result</th><th>Check "
                  "Description</th><th>Observed Detail</th></tr></thead>\n";
            ss << "          <tbody>\n";
            for (const auto& c : r.checks) {
                ss << "            <tr>\n";
                ss << std::format(
                    "              <td><span class=\"badge badge-{}\">{}</span></td>\n",
                    c.passed ? "pass" : "fail",
                    c.passed ? "OK" : "FAIL");
                ss << std::format("              <td>{}</td>\n", escapeHtml(c.description));
                ss << std::format("              <td class=\"mono\" style=\"color: {};\">{}</td>\n",
                                  c.passed ? "var(--text-muted)" : "var(--fail)",
                                  escapeHtml(c.detail.empty() ? "-" : c.detail));
                ss << "            </tr>\n";
            }
            ss << "          </tbody>\n";
            ss << "        </table>\n";
            ss << "      </div>\n";
        }

        // Associated Signal Waveforms (SVG)
        if (!r.capturedSignals.empty()) {
            ss << "      <div>\n";
            ss << "        <div class=\"section-title\">Associated Signal Waveforms ("
               << r.capturedSignals.size() << " samples)</div>\n";
            ss << generateSignalChartSvg(r.capturedSignals);
            ss << "      </div>\n";
        }

        // CAN Trace Snippet
        if (!r.capturedFrames.empty()) {
            ss << "      <div>\n";
            ss << "        <div class=\"section-title\">CAN Bus Traffic Snippet ("
               << r.capturedFrames.size() << " frames captured)</div>\n";
            ss << "        <div class=\"trace-container\">\n";
            ss << "          <table>\n";
            ss << "            <thead><tr><th style=\"width: 40px;\">#</th><th style=\"width: "
                  "90px;\">Time (s)</th><th style=\"width: 50px;\">Ch</th><th style=\"width: "
                  "90px;\">ID</th><th style=\"width: 60px;\">Dir</th><th style=\"width: "
                  "50px;\">DLC</th><th>Data (Hex)</th><th>ASCII</th></tr></thead>\n";
            ss << "            <tbody>\n";
            for (std::size_t fIdx = 0; fIdx < r.capturedFrames.size(); ++fIdx) {
                const auto& f = r.capturedFrames[fIdx];
                const char* idHexFormat = f.isExtended() ? "0x{:08X}" : "0x{:03X}";
                const std::string idStr =
                    std::vformat(idHexFormat, std::make_format_args(f.identifier));
                ss << "              <tr class=\"mono\">\n";
                ss << std::format("                <td>{}</td>\n", fIdx + 1);
                ss << std::format("                <td>{:.4f}</td>\n",
                                  static_cast<double>(f.timestampNs) / 1e9);
                ss << std::format("                <td>{}</td>\n", f.channel);
                ss << std::format("                <td><strong>{}</strong></td>\n", idStr);
                ss << std::format("                <td style=\"color: {};\">{}</td>\n",
                                  f.direction == CanDirection::Tx ? "var(--accent)" : "var(--pass)",
                                  f.direction == CanDirection::Tx ? "Tx" : "Rx");
                ss << std::format("                <td>{}</td>\n", f.payloadSize());
                ss << std::format("                <td>{}</td>\n", formatHexBytes(f));
                ss << std::format(
                    "                <td style=\"color: var(--text-muted);\">{}</td>\n",
                    escapeHtml(formatAsciiBytes(f)));
                ss << "              </tr>\n";
            }
            ss << "            </tbody>\n";
            ss << "          </table>\n";
            ss << "        </div>\n";
            ss << "      </div>\n";
        }

        ss << "    </div>\n";
        ss << "  </details>\n";
        ss << "</div>\n";
    }
    ss << "</div>\n"; // cases-container

    // Vanilla JS Filter Logic
    ss << "<script>\n";
    ss << R"JS(
        let activeStatus = 'all';
        function filterStatus(status) {
            activeStatus = status;
            document.querySelectorAll('.filter-group .btn').forEach(btn => {
                btn.classList.toggle('active', btn.textContent.toLowerCase().startsWith(status));
            });
            applyFilters();
        }
        function applyFilters() {
            const query = document.getElementById('search').value.toLowerCase();
            document.querySelectorAll('.case-card').forEach(card => {
                const st = card.getAttribute('data-status');
                const text = card.textContent.toLowerCase();
                const matchesStatus = (activeStatus === 'all') ||
                    (activeStatus === 'failed' && (st === 'fail' || st === 'error')) ||
                    (activeStatus === 'passed' && st === 'pass');
                const matchesSearch = !query || text.includes(query);
                card.style.display = (matchesStatus && matchesSearch) ? 'block' : 'none';
            });
        }
        function toggleAll(open) {
            document.querySelectorAll('.case-card details').forEach(d => d.open = open);
        }
    )JS";
    ss << "</script>\n";

    ss << "</div>\n"; // container
    ss << "</body>\n";
    ss << "</html>\n";

    return ss.str();
}

std::string TestReportExport::buildPrintHtml(const TestReport& report, const DossierMetadata& meta)
{
    return buildPrintHtml(report.results(), report.summary(), meta);
}

std::string TestReportExport::buildPrintHtml(const std::vector<TestCaseResult>& results,
                                             const TestSummary& summary,
                                             const DossierMetadata& meta)
{
    std::ostringstream ss;

    const std::size_t passed = summary.passed;
    const std::size_t failed = summary.failed;
    const std::size_t errored = summary.errored;
    const std::size_t total = summary.total > 0 ? summary.total : results.size();
    const double passRate = (total > 0) ? (static_cast<double>(passed) / total) * 100.0 : 0.0;
    const bool isGreen = (failed == 0 && errored == 0 && passed > 0);

    ss << "<!DOCTYPE html>\n<html><head><meta charset=\"UTF-8\">\n";
    ss << "<style>\n";
    ss << "body { font-family: 'Segoe UI', Arial, sans-serif; color: #1e293b; margin: 0; padding: "
          "10px; font-size: 11pt; }\n";
    ss << "h1 { color: #0f172a; font-size: 18pt; margin-bottom: 4px; }\n";
    ss << "h2 { color: #1e293b; font-size: 14pt; margin-top: 14px; margin-bottom: 6px; }\n";
    ss << "h3 { color: #334155; font-size: 12pt; margin-top: 10px; margin-bottom: 4px; }\n";
    ss << "table { width: 100%; border-collapse: collapse; margin-bottom: 12px; font-size: 10pt; "
          "}\n";
    ss << "th { background-color: #f1f5f9; color: #334155; text-align: left; padding: 6px; border: "
          "1px solid #cbd5e1; }\n";
    ss << "td { padding: 6px; border: 1px solid #cbd5e1; }\n";
    ss << ".badge-pass { background-color: #dcfce7; color: #15803d; font-weight: bold; }\n";
    ss << ".badge-fail { background-color: #fee2e2; color: #b91c1c; font-weight: bold; }\n";
    ss << ".badge-error { background-color: #fef3c7; color: #b45309; font-weight: bold; }\n";
    ss << ".mono { font-family: Consolas, monospace; font-size: 9pt; }\n";
    ss << "</style></head><body>\n";

    ss << "<h1>" << escapeHtml(meta.title) << "</h1>\n";
    ss << "<p style=\"color: #64748b; font-size: 10pt;\">"
       << "<strong>System:</strong> " << escapeHtml(meta.systemUnderTest) << " &nbsp;|&nbsp; "
       << "<strong>Tester:</strong> " << escapeHtml(meta.tester) << " &nbsp;|&nbsp; "
       << "<strong>Env:</strong> " << escapeHtml(meta.environment) << "</p>\n";

    // Summary Table
    ss << "<h2>Executive Summary</h2>\n";
    ss << "<table style=\"margin-bottom: 16px;\">\n";
    ss << "<tr><th>Status</th><th>Total</th><th>Passed</th><th>Failed</th><th>Errored</"
          "th><th>Pass Rate</th></tr>\n";
    ss << std::format("<tr><td class=\"{}\">{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</"
                      "td><td><strong>{:.1f}%</strong></td></tr>\n",
                      isGreen ? "badge-pass" : "badge-fail",
                      isGreen ? "PASSED" : "FAILED",
                      total,
                      passed,
                      failed,
                      errored,
                      passRate);
    ss << "</table>\n";

    // Embedded timeline image reference
    ss << "<p><img src=\"cid:timeline\" width=\"680\" /></p>\n";

    // Case breakdown
    ss << "<h2>Test Cases Details</h2>\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const std::uint64_t dur = (r.finishedNs > r.startedNs) ? (r.finishedNs - r.startedNs) : 0;
        const char* badgeClass = (r.outcome == TestOutcome::Passed)    ? "badge-pass"
                                 : (r.outcome == TestOutcome::Failed)  ? "badge-fail"
                                 : (r.outcome == TestOutcome::Errored) ? "badge-error"
                                                                       : "badge-running";

        ss << std::format("<h3><span class=\"{}\">&nbsp;{}&nbsp;</span> - {} <span "
                          "style=\"font-size: 10pt; color: #64748b;\">({} | duration: "
                          "{})</span></h3>\n",
                          badgeClass,
                          outcomeText(r.outcome),
                          escapeHtml(r.name),
                          formatSeconds(r.startedNs) + "s",
                          formatDuration(dur));

        if (!r.message.empty()) {
            ss << std::format(
                "<p style=\"color: #b91c1c; background-color: #fee2e2; padding: 6px; border: 1px "
                "solid #fca5a5;\"><strong>Failure:</strong> {}</p>\n",
                escapeHtml(r.message));
        }

        // Checks
        if (!r.checks.empty()) {
            ss << "<table>\n";
            ss << "<tr><th style=\"width: 60px;\">Result</th><th>Description</th><th>Detail</th></"
                  "tr>\n";
            for (const auto& c : r.checks) {
                ss << std::format("<tr><td class=\"{}\">{}</td><td>{}</td><td "
                                  "class=\"mono\">{}</td></tr>\n",
                                  c.passed ? "badge-pass" : "badge-fail",
                                  c.passed ? "OK" : "FAIL",
                                  escapeHtml(c.description),
                                  escapeHtml(c.detail.empty() ? "-" : c.detail));
            }
            ss << "</table>\n";
        }

        // Signal chart image reference
        if (!r.capturedSignals.empty()) {
            ss << std::format("<p><img src=\"cid:signals_{}\" width=\"650\" /></p>\n", i);
        }

        // CAN trace snippet
        if (!r.capturedFrames.empty()) {
            ss << std::format(
                "<p style=\"font-weight: bold; margin-bottom: 2px;\">CAN Bus Trace ({} frames "
                "captured):</p>\n",
                r.capturedFrames.size());
            ss << "<table class=\"mono\" style=\"font-size: 8.5pt;\">\n";
            ss << "<tr><th>#</th><th>Time (s)</th><th>Ch</th><th>ID</th><th>Dir</th><th>DLC</"
                  "th><th>Data (Hex)</th><th>ASCII</th></tr>\n";
            for (std::size_t fIdx = 0; fIdx < r.capturedFrames.size(); ++fIdx) {
                const auto& f = r.capturedFrames[fIdx];
                const char* idHexFormat = f.isExtended() ? "0x{:08X}" : "0x{:03X}";
                const std::string idStr =
                    std::vformat(idHexFormat, std::make_format_args(f.identifier));
                ss << std::format("<tr><td>{}</td><td>{:.4f}</td><td>{}</td><td>{}</td><td>{}</"
                                  "td><td>{}</td><td>{}</td><td>{}</td></tr>\n",
                                  fIdx + 1,
                                  static_cast<double>(f.timestampNs) / 1e9,
                                  f.channel,
                                  idStr,
                                  f.direction == CanDirection::Tx ? "Tx" : "Rx",
                                  f.payloadSize(),
                                  formatHexBytes(f),
                                  escapeHtml(formatAsciiBytes(f)));
            }
            ss << "</table>\n";
        }
    }

    ss << "</body></html>\n";
    return ss.str();
}

std::string TestReportExport::buildMarkdown(const TestReport& report)
{
    return buildMarkdown(report.results(), report.summary());
}

std::string TestReportExport::buildMarkdown(const std::vector<TestCaseResult>& results,
                                            const TestSummary& summary)
{
    std::ostringstream ss;

    ss << "# TorqueBus Test Report\n\n";

    const std::size_t total = summary.total > 0 ? summary.total : results.size();
    ss << (summary.complete ? "**Complete.**" : "**Stopped before the end.**") << "  \n";
    ss << std::format("{} passed, {} failed, {} in error, of {} declared.\n\n",
                      summary.passed,
                      summary.failed,
                      summary.errored,
                      total);

    // Summary table
    ss << "| Result | Case | Started | Duration | Detail |\n";
    ss << "|---|---|---|---|---|\n";

    for (const auto& r : results) {
        const std::uint64_t dur = (r.finishedNs > r.startedNs) ? (r.finishedNs - r.startedNs) : 0;
        ss << std::format("| {} | {} | {} | {} | {} |\n",
                          outcomeText(r.outcome),
                          r.name,
                          formatSeconds(r.startedNs) + "s",
                          formatDuration(dur),
                          r.message);
    }
    ss << "\n";

    // Non-passing cases breakdown
    for (const auto& r : results) {
        if (r.outcome == TestOutcome::Passed && r.checks.empty()) {
            continue;
        }

        ss << "## " << r.name << " (" << outcomeText(r.outcome) << ")\n\n";

        if (!r.message.empty()) {
            ss << "> **Failure:** " << r.message << "\n\n";
        }

        for (const auto& c : r.checks) {
            ss << "- " << (c.passed ? "OK" : "FAIL") << ": " << c.description;
            if (!c.detail.empty()) {
                ss << " — " << c.detail;
            }
            ss << "\n";
        }
        ss << "\n";

        if (!r.capturedFrames.empty()) {
            ss << "### CAN Bus Traffic Snippet (" << r.capturedFrames.size() << " frames)\n\n";
            ss << "| # | Time (s) | Ch | ID | Dir | DLC | Data (Hex) |\n";
            ss << "|---|---|---|---|---|---|---|\n";
            for (std::size_t fIdx = 0; fIdx < r.capturedFrames.size(); ++fIdx) {
                const auto& f = r.capturedFrames[fIdx];
                const char* idHexFormat = f.isExtended() ? "0x{:08X}" : "0x{:03X}";
                const std::string idStr =
                    std::vformat(idHexFormat, std::make_format_args(f.identifier));
                ss << std::format("| {} | {:.4f} | {} | {} | {} | {} | {} |\n",
                                  fIdx + 1,
                                  static_cast<double>(f.timestampNs) / 1e9,
                                  f.channel,
                                  idStr,
                                  f.direction == CanDirection::Tx ? "Tx" : "Rx",
                                  f.payloadSize(),
                                  formatHexBytes(f));
            }
            ss << "\n";
        }
    }

    return ss.str();
}

} // namespace torquebus
