// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFrame.h"
#include "core/log/TraceExport.h"
#include "core/log/TraceImport.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/NodeCatalog.h"

#include "UniqueTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace torquebus;
using torquebus::tests::uniqueTempPath;

namespace {

class ScopedTestFile final {
public:
    explicit ScopedTestFile(std::string extension)
        : m_path{uniqueTempPath("torquebus_import_test", counter(), "." + extension).string()}
    {
        std::filesystem::remove(m_path);
    }

    ~ScopedTestFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ScopedTestFile(const ScopedTestFile&) = delete;
    ScopedTestFile& operator=(const ScopedTestFile&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

    void writeText(const std::string& content)
    {
        std::ofstream out{m_path, std::ios::trunc};
        out << content;
    }

private:
    [[nodiscard]] static int counter()
    {
        static int next = 0;
        return ++next;
    }

    std::string m_path;
};

} // namespace

TEST(TraceImportTests, DetectsFormatByExtensionAndContents)
{
    EXPECT_TRUE(TraceImporter::isExternalLog("test.asc"));
    EXPECT_TRUE(TraceImporter::isExternalLog("C:/logs/capture.ASC"));
    EXPECT_TRUE(TraceImporter::isExternalLog("pcan_log.trc"));
    EXPECT_TRUE(TraceImporter::isExternalLog("bus.TRC"));
    EXPECT_FALSE(TraceImporter::isExternalLog("measurement.tblog"));
    EXPECT_FALSE(TraceImporter::isExternalLog("database.dbc"));

    EXPECT_EQ(TraceImporter::detectFormat("test.asc"), ExternalTraceFormat::VectorAsc);
    EXPECT_EQ(TraceImporter::detectFormat("test.trc"), ExternalTraceFormat::PcanTrc);
}

TEST(TraceImportTests, ParseVectorAscStandard11BitFrame)
{
    const std::string line = "   0.012345 1  123             Rx   d 8 01 02 03 04 05 06 07 08";
    CanFrame frame{};
    std::uint64_t wallClockUs = 0;

    ASSERT_TRUE(TraceImporter::parseAscLine(line, frame, wallClockUs));
    EXPECT_EQ(frame.timestampNs, 12'345'000ULL);
    EXPECT_EQ(frame.channel, 0); // 1-based channel 1 -> 0
    EXPECT_EQ(frame.identifier, 0x123U);
    EXPECT_FALSE(frame.isExtended());
    EXPECT_EQ(frame.direction, CanDirection::Rx);
    EXPECT_FALSE(frame.rtr);
    EXPECT_FALSE(frame.error);
    EXPECT_EQ(frame.length, 8U);
    EXPECT_EQ(frame.dlc, 8U);
    EXPECT_EQ(frame.data[0], 0x01);
    EXPECT_EQ(frame.data[7], 0x08);
}

TEST(TraceImportTests, ParseVectorAscExtended29BitFrame)
{
    const std::string line = "   1.500000 2  18FEE500x       Tx   d 4 AA BB CC DD";
    CanFrame frame{};
    std::uint64_t wallClockUs = 0;

    ASSERT_TRUE(TraceImporter::parseAscLine(line, frame, wallClockUs));
    EXPECT_EQ(frame.timestampNs, 1'500'000'000ULL);
    EXPECT_EQ(frame.channel, 1); // 1-based channel 2 -> 1
    EXPECT_EQ(frame.identifier, 0x18FEE500U);
    EXPECT_TRUE(frame.isExtended());
    EXPECT_EQ(frame.direction, CanDirection::Tx);
    EXPECT_EQ(frame.length, 4U);
    EXPECT_EQ(frame.data[0], 0xAA);
    EXPECT_EQ(frame.data[3], 0xDD);
}

TEST(TraceImportTests, ParseVectorAscErrorAndRemoteFrames)
{
    std::uint64_t wallClockUs = 0;

    {
        const std::string rtrLine = "   2.100000 1  7DF             Rx   r 0";
        CanFrame frame{};
        ASSERT_TRUE(TraceImporter::parseAscLine(rtrLine, frame, wallClockUs));
        EXPECT_EQ(frame.identifier, 0x7DFU);
        EXPECT_TRUE(frame.rtr);
        EXPECT_EQ(frame.length, 0U);
    }

    {
        const std::string errLine = "   3.000000 1  ErrorFrame";
        CanFrame frame{};
        ASSERT_TRUE(TraceImporter::parseAscLine(errLine, frame, wallClockUs));
        EXPECT_TRUE(frame.error);
        EXPECT_EQ(frame.timestampNs, 3'000'000'000ULL);
    }
}

TEST(TraceImportTests, ParseVectorAscCanFdFrame)
{
    const std::string line = "   0.005000 CANFD   1 Rx        7E0                                 "
                             "0 0  8  8 02 10 01 00 00 00 00 00";
    CanFrame frame{};
    std::uint64_t wallClockUs = 0;

    ASSERT_TRUE(TraceImporter::parseAscLine(line, frame, wallClockUs));
    EXPECT_EQ(frame.identifier, 0x7E0U);
    EXPECT_TRUE(frame.fd);
    EXPECT_EQ(frame.direction, CanDirection::Rx);
    EXPECT_EQ(frame.length, 8U);
    EXPECT_EQ(frame.data[0], 0x02);
    EXPECT_EQ(frame.data[1], 0x10);
}

TEST(TraceImportTests, ParseVectorAscHeaderDate)
{
    const std::string dateLine = "date Tue Oct 27 12:00:00.000 UTC 2026";
    CanFrame frame{};
    std::uint64_t wallClockUs = 0;

    EXPECT_FALSE(TraceImporter::parseAscLine(dateLine, frame, wallClockUs));
    EXPECT_GT(wallClockUs, 0ULL);
}

TEST(TraceImportTests, ParsePcanTrcV11Format)
{
    int trcVersion = 11;
    std::uint64_t wallClockUs = 0;

    const std::string line1 = "     1)       0.000    DT        0100 8  01 02 03 04 05 06 07 08";
    CanFrame frame1{};
    ASSERT_TRUE(TraceImporter::parseTrcLine(line1, frame1, trcVersion, wallClockUs));
    EXPECT_EQ(frame1.timestampNs, 0ULL);
    EXPECT_EQ(frame1.identifier, 0x100U);
    EXPECT_FALSE(frame1.isExtended());
    EXPECT_EQ(frame1.length, 8U);
    EXPECT_EQ(frame1.data[0], 0x01);
    EXPECT_EQ(frame1.data[7], 0x08);

    const std::string line2 = "     2)       5.500    DT    18FEE500 8  FF FF FF FF FF FF FF FF";
    CanFrame frame2{};
    ASSERT_TRUE(TraceImporter::parseTrcLine(line2, frame2, trcVersion, wallClockUs));
    EXPECT_EQ(frame2.timestampNs, 5'500'000ULL); // 5.5 ms -> 5,500,000 ns
    EXPECT_EQ(frame2.identifier, 0x18FEE500U);
    EXPECT_TRUE(frame2.isExtended());
    EXPECT_EQ(frame2.length, 8U);
}

TEST(TraceImportTests, ParsePcanTrcV21WithBusAndCanFd)
{
    int trcVersion = 21;
    std::uint64_t wallClockUs = 0;

    const std::string startLine = ";$STARTTIME=44500.5";
    CanFrame dummy{};
    EXPECT_FALSE(TraceImporter::parseTrcLine(startLine, dummy, trcVersion, wallClockUs));
    EXPECT_GT(wallClockUs, 0ULL);

    const std::string lineFd =
        "     1)      12.345    FD        2 07E8  -   8   06 50 01 00 00 00 00 00";
    CanFrame frame{};
    ASSERT_TRUE(TraceImporter::parseTrcLine(lineFd, frame, trcVersion, wallClockUs));
    EXPECT_EQ(frame.timestampNs, 12'345'000ULL);
    EXPECT_EQ(frame.channel, 1); // Bus 2 -> channel 1
    EXPECT_EQ(frame.identifier, 0x7E8U);
    EXPECT_TRUE(frame.fd);
    EXPECT_EQ(frame.length, 8U);
    EXPECT_EQ(frame.data[0], 0x06);
    EXPECT_EQ(frame.data[1], 0x50);
}

TEST(TraceImportTests, ImportVectorAscFileToTblogAndVerify)
{
    ScopedTestFile ascFile{"asc"};
    const std::string ascContent =
        "date Tue Oct 27 12:00:00.000 UTC 2026\n"
        "base hex  timestamps absolute\n"
        "internal events logged\n"
        "// TorqueBus test recording\n"
        "Begin Triggerblock Tue Oct 27 12:00:00.000 UTC 2026\n"
        "   0.000000 1  100             Rx   d 8 11 22 33 44 55 66 77 88\n"
        "   0.010000 1  200             Tx   d 2 AA BB\n"
        "   0.020000 1  18FEE500x       Rx   d 8 FF EE DD CC BB AA 99 88\n"
        "End TriggerBlock\n";

    ascFile.writeText(ascContent);

    ScopedTestFile tblogFile{"tblog"};
    ImportSummary summary{};

    const Result res = TraceImporter::importToLog(ascFile.path(), tblogFile.path(), &summary);
    ASSERT_TRUE(res.succeeded()) << res.message();
    EXPECT_EQ(summary.frames, 3U);
    EXPECT_EQ(summary.format, ExternalTraceFormat::VectorAsc);
    EXPECT_EQ(summary.durationNs, 20'000'000ULL);

    // Verify reading with native TraceLogReader
    TraceLogReader reader;
    ASSERT_TRUE(reader.open(tblogFile.path()).succeeded());

    std::vector<CanFrame> readFrames(10);
    const std::size_t count = reader.read(readFrames);
    ASSERT_EQ(count, 3U);

    EXPECT_EQ(readFrames[0].identifier, 0x100U);
    EXPECT_FALSE(readFrames[0].isExtended());
    EXPECT_EQ(readFrames[0].data[0], 0x11);

    EXPECT_EQ(readFrames[1].identifier, 0x200U);
    EXPECT_EQ(readFrames[1].direction, CanDirection::Tx);
    EXPECT_EQ(readFrames[1].length, 2U);

    EXPECT_EQ(readFrames[2].identifier, 0x18FEE500U);
    EXPECT_TRUE(readFrames[2].isExtended());
}

TEST(TraceImportTests, ImportPcanTrcFileToTblogAndVerify)
{
    ScopedTestFile trcFile{"trc"};
    const std::string trcContent =
        ";$FILEVERSION=2.1\n"
        ";$STARTTIME=44500.5\n"
        ";-------------------------------------------------------------------\n"
        "     1)       0.000    DT        1 0123  -   4   01 02 03 04\n"
        "     2)      10.000    DT        1 0456  -   2   AA BB\n";

    trcFile.writeText(trcContent);

    ScopedTestFile tblogFile{"tblog"};
    ImportSummary summary{};

    const Result res = TraceImporter::importToLog(trcFile.path(), tblogFile.path(), &summary);
    ASSERT_TRUE(res.succeeded()) << res.message();
    EXPECT_EQ(summary.frames, 2U);
    EXPECT_EQ(summary.format, ExternalTraceFormat::PcanTrc);
    EXPECT_EQ(summary.durationNs, 10'000'000ULL);

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(tblogFile.path()).succeeded());
    std::vector<CanFrame> frames(5);
    ASSERT_EQ(reader.read(frames), 2U);
    EXPECT_EQ(frames[0].identifier, 0x123U);
    EXPECT_EQ(frames[1].identifier, 0x456U);
}

TEST(TraceImportTests, RoundTripWithTraceExportAndImport)
{
    ScopedTestFile exportedAsc{"asc"};
    {
        TraceExporter exporter;
        ASSERT_TRUE(
            exporter.open(exportedAsc.path(), TraceExporter::Format::Asc, 1700000000000000ULL)
                .succeeded());

        std::vector<CanFrame> originalFrames;
        for (std::uint32_t i = 1; i <= 5; ++i) {
            CanFrame f{};
            f.identifier = 0x100 + i;
            f.timestampNs = i * 1'000'000ULL;
            f.length = 4;
            f.dlc = 4;
            f.data[0] = static_cast<std::uint8_t>(i);
            f.data[1] = static_cast<std::uint8_t>(i * 2);
            f.data[2] = 0xAA;
            f.data[3] = 0xBB;
            originalFrames.push_back(f);
        }

        ASSERT_TRUE(exporter.write(originalFrames).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    std::vector<CanFrame> importedFrames;
    ImportSummary summary{};
    ASSERT_TRUE(
        TraceImporter::importFrames(exportedAsc.path(), importedFrames, 0, &summary).succeeded());

    ASSERT_EQ(importedFrames.size(), 5U);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(importedFrames[i].identifier, 0x101 + i);
        EXPECT_EQ(importedFrames[i].timestampNs, (i + 1) * 1'000'000ULL);
        EXPECT_EQ(importedFrames[i].length, 4U);
        EXPECT_EQ(importedFrames[i].data[0], i + 1);
        EXPECT_EQ(importedFrames[i].data[1], (i + 1) * 2);
    }
}

TEST(TraceImportTests, AutoImportEnablesLogSourceReplayFromAsc)
{
    ScopedTestFile ascFile{"asc"};
    const std::string ascContent =
        "date Tue Oct 27 12:00:00.000 UTC 2026\n"
        "base hex  timestamps absolute\n"
        "Begin Triggerblock Tue Oct 27 12:00:00.000 UTC 2026\n"
        "   0.000000 1  7E0             Rx   d 8 02 10 01 00 00 00 00 00\n"
        "   0.050000 1  7E8             Tx   d 8 06 50 01 00 00 00 00 00\n"
        "End TriggerBlock\n";
    ascFile.writeText(ascContent);

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeParameters params;
    params.set("path", ParameterValue::fromText(ascFile.path()));
    params.set("speed", ParameterValue::fromReal(1.0));

    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> node;

    const Result res = catalog.create("log.source", params, context, "replay_asc", node);
    ASSERT_TRUE(res.succeeded()) << res.message();
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->typeName(), "log.source");
}
