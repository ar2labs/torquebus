// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/testing/TestReportExport.h"

#include "core/can/CanFrame.h"
#include "core/testing/TestReport.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace torquebus;

namespace {

void populateSampleReport(TestReport& report)
{
    report.begin(2);

    // Case 1: Passing
    TestCaseResult passCase;
    passCase.name = "Engine Speed Plausibility";
    passCase.outcome = TestOutcome::Passed;
    passCase.startedNs = 100'000'000ULL;
    passCase.finishedNs = 250'000'000ULL;

    TestCheck c1;
    c1.description = "0x100 message arrives within 200 ms";
    c1.passed = true;
    passCase.checks.push_back(c1);

    TestCheck c2;
    c2.description = "DLC equals 8";
    c2.passed = true;
    passCase.checks.push_back(c2);

    report.add(std::move(passCase));

    // Case 2: Failing with captured frames & signals
    TestCaseResult failCase;
    failCase.name = "Fault Lamp Response Time";
    failCase.outcome = TestOutcome::Failed;
    failCase.message = "0x300 lamp frame was not silenced within 100 ms";
    failCase.startedNs = 300'000'000ULL;
    failCase.finishedNs = 450'000'000ULL;

    TestCheck c3;
    c3.description = "Diagnostic command sent";
    c3.passed = true;
    failCase.checks.push_back(c3);

    TestCheck c4;
    c4.description = "0x300 stays silent for 100 ms";
    c4.passed = false;
    c4.detail = "expected silence, saw frame 0x300 after 42 ms";
    failCase.checks.push_back(c4);

    // Captured frames
    CanFrame f1;
    f1.identifier = 0x7E0;
    f1.channel = 0;
    f1.direction = CanDirection::Tx;
    f1.format = CanFrameFormat::Standard;
    f1.length = 8;
    f1.dlc = 8;
    f1.timestampNs = 310'000'000ULL;
    f1.data = {0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
    failCase.capturedFrames.push_back(f1);

    CanFrame f2;
    f2.identifier = 0x300;
    f2.channel = 0;
    f2.direction = CanDirection::Rx;
    f2.format = CanFrameFormat::Standard;
    f2.length = 4;
    f2.dlc = 4;
    f2.timestampNs = 342'000'000ULL;
    f2.data = {0x01, 0xFF, 0x00, 0x55};
    failCase.capturedFrames.push_back(f2);

    // Captured signals
    failCase.capturedSignals.push_back(TestSignalPoint{
        .name = "LampState",
        .value = 1.0,
        .timestampNs = 300'000'000ULL,
    });
    failCase.capturedSignals.push_back(TestSignalPoint{
        .name = "LampState",
        .value = 1.0,
        .timestampNs = 350'000'000ULL,
    });
    failCase.capturedSignals.push_back(TestSignalPoint{
        .name = "BatteryVoltage",
        .value = 12.6,
        .timestampNs = 300'000'000ULL,
    });
    failCase.capturedSignals.push_back(TestSignalPoint{
        .name = "BatteryVoltage",
        .value = 12.5,
        .timestampNs = 400'000'000ULL,
    });

    report.add(std::move(failCase));
    report.finish();
}

} // namespace

TEST(TestReportExportTests, SvgTimelineGeneratesValidXml)
{
    TestReport report;
    populateSampleReport(report);
    const auto results = report.results();

    const std::string svg = TestReportExport::generateTimelineSvg(results, 800.0);
    EXPECT_FALSE(svg.empty());
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_NE(svg.find("Engine Speed Plausibility"), std::string::npos);
    EXPECT_NE(svg.find("Fault Lamp Response Time"), std::string::npos);
    EXPECT_NE(svg.find("</svg>"), std::string::npos);
}

TEST(TestReportExportTests, SvgSignalChartGeneratesLinesAndLegend)
{
    std::vector<TestSignalPoint> pts = {
        {"RPM", 800.0, 0},
        {"RPM", 1200.0, 10'000'000},
        {"RPM", 1600.0, 20'000'000},
        {"Speed", 0.0, 0},
        {"Speed", 15.0, 20'000'000},
    };

    const std::string svg = TestReportExport::generateSignalChartSvg(pts, 600.0, 200.0);
    EXPECT_FALSE(svg.empty());
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_NE(svg.find("<polyline"), std::string::npos);
    EXPECT_NE(svg.find("RPM"), std::string::npos);
    EXPECT_NE(svg.find("Speed"), std::string::npos);
    EXPECT_NE(svg.find("</svg>"), std::string::npos);
}

TEST(TestReportExportTests, HtmlDossierContainsAllSectionsAndTraffic)
{
    TestReport report;
    populateSampleReport(report);
    DossierMetadata meta;
    meta.title = "CAN Bus Homologation Test Run";
    meta.tester = "Antigravity Engineer";
    meta.systemUnderTest = "Instrument Cluster ECU";

    const std::string html = TestReportExport::buildHtml(report, meta);

    // Structure & Metadata
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("CAN Bus Homologation Test Run"), std::string::npos);
    EXPECT_NE(html.find("Instrument Cluster ECU"), std::string::npos);
    EXPECT_NE(html.find("Antigravity Engineer"), std::string::npos);

    // Executive summary & verdict
    EXPECT_NE(html.find("FAILED (50.0%)"), std::string::npos);
    EXPECT_NE(html.find("Total Cases"), std::string::npos);

    // Cases & assertions
    EXPECT_NE(html.find("Engine Speed Plausibility"), std::string::npos);
    EXPECT_NE(html.find("Fault Lamp Response Time"), std::string::npos);
    EXPECT_NE(html.find("0x300 lamp frame was not silenced"), std::string::npos);
    EXPECT_NE(html.find("DLC equals 8"), std::string::npos);

    // CAN trace traffic
    EXPECT_NE(html.find("0x7E0"), std::string::npos);
    EXPECT_NE(html.find("0x300"), std::string::npos);
    EXPECT_NE(html.find("02 10 03 00 00 00 00 00"), std::string::npos);

    // Associated signal waveforms
    EXPECT_NE(html.find("LampState"), std::string::npos);
    EXPECT_NE(html.find("BatteryVoltage"), std::string::npos);

    // Interactive JS filtering
    EXPECT_NE(html.find("filterStatus"), std::string::npos);
    EXPECT_NE(html.find("applyFilters"), std::string::npos);
}

TEST(TestReportExportTests, PrintHtmlContainsCidImages)
{
    TestReport report;
    populateSampleReport(report);
    DossierMetadata meta;
    meta.title = "Printable Test Dossier";

    const std::string printHtml = TestReportExport::buildPrintHtml(report, meta);

    EXPECT_NE(printHtml.find("cid:timeline"), std::string::npos);
    EXPECT_NE(printHtml.find("cid:signals_1"), std::string::npos);
    EXPECT_NE(printHtml.find("Executive Summary"), std::string::npos);
    EXPECT_NE(printHtml.find("0x7E0"), std::string::npos);
}

TEST(TestReportExportTests, MarkdownContainsTablesAndFailureDetails)
{
    TestReport report;
    populateSampleReport(report);

    const std::string md = TestReportExport::buildMarkdown(report);

    EXPECT_NE(md.find("# TorqueBus Test Report"), std::string::npos);
    EXPECT_NE(md.find("1 passed, 1 failed"), std::string::npos);
    EXPECT_NE(md.find("| Result | Case | Started | Duration | Detail |"), std::string::npos);
    EXPECT_NE(md.find("Fault Lamp Response Time"), std::string::npos);
    EXPECT_NE(md.find("0x300 lamp frame was not silenced"), std::string::npos);
    EXPECT_NE(md.find("0x7E0"), std::string::npos);
}
