// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// An exported file is read by something that is not TorqueBus. Every case here
// is about a detail another tool will get wrong if this one writes it wrong -
// which is a class of bug that never shows up in this application at all.

#include "core/can/CanFrame.h"
#include "core/log/TraceExport.h"

#include "UniqueTempPath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace torquebus;
using torquebus::tests::uniqueTempPath;

namespace {

class ScopedFile final {
public:
    explicit ScopedFile(const char* extension)
        : m_path{
              uniqueTempPath("torquebus_export", counter(), std::string{"."} + extension).string()}
    {
        std::filesystem::remove(m_path);
    }

    ~ScopedFile()
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

    [[nodiscard]] std::vector<std::string> lines() const
    {
        std::ifstream file{m_path};
        std::vector<std::string> result;

        for (std::string line; std::getline(file, line);) {
            result.push_back(line);
        }

        return result;
    }

    [[nodiscard]] std::string text() const
    {
        std::ifstream file{m_path};
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

private:
    [[nodiscard]] static int counter()
    {
        static int next = 0;
        return ++next;
    }

    std::string m_path;
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::uint64_t timestampNs)
{
    CanFrame result;
    result.identifier = identifier;
    result.timestampNs = timestampNs;
    result.length = 3;
    result.dlc = 3;
    result.data[0] = 0x0A;
    result.data[1] = 0xBC;
    result.data[2] = 0xFF;
    return result;
}

/// True when any line contains `needle`.
[[nodiscard]] bool anyLineContains(const std::vector<std::string>& lines, const std::string& needle)
{
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(TraceExportTests, AnASCFileHasThePreambleEveryReaderLooksFor)
{
    const ScopedFile file{"asc"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.write(std::array{frame(0x100, 0)}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();
    ASSERT_TRUE(lines.size() >= 6);

    EXPECT_TRUE(lines[0].starts_with("date "));

    // Both of these describe every line below, and a reader that does not find
    // them guesses - usually at decimal, which turns 0x100 into 256.
    EXPECT_TRUE(anyLineContains(lines, "base hex"));
    EXPECT_TRUE(anyLineContains(lines, "timestamps absolute"));
    EXPECT_TRUE(anyLineContains(lines, "Begin Triggerblock"));

    // Without this a reader treats the file as truncated.
    EXPECT_TRUE(lines.back() == "End TriggerBlock");
}

TEST(TraceExportTests, AnExtendedIdentifierCarriesItsX)
{
    // The commonest way an exported file decodes wrongly at the other end:
    // 0x100 standard and 0x100 extended are two different messages on one bus,
    // and the `x` is the only thing that tells them apart.
    const ScopedFile file{"asc"};

    CanFrame extended = frame(0x18FEF100, 1'000'000);
    extended.format = CanFrameFormat::Extended;

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.write(std::array{extended, frame(0x7AB, 2'000'000)}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    EXPECT_TRUE(anyLineContains(lines, "18FEF100x"));

    // And the standard one does not get one by accident. Checked against an
    // identifier that is not a suffix of the extended one - the first version
    // of this looked for "100x " and found it inside "18FEF100x", which is a
    // test that passes for the wrong reason in one direction and fails for the
    // wrong reason in the other.
    EXPECT_TRUE(anyLineContains(lines, "7AB "));
    EXPECT_FALSE(anyLineContains(lines, "7ABx"));
}

TEST(TraceExportTests, AFrameLineCarriesTheTimeChannelDirectionAndBytes)
{
    const ScopedFile file{"asc"};

    CanFrame sent = frame(0x123, 1'500'000); // 0.0015 s
    sent.direction = CanDirection::Tx;
    sent.channel = 1; // CAN 2, in the numbering people read

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.write(std::array{sent}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    EXPECT_TRUE(anyLineContains(lines, "0.001500"));
    EXPECT_TRUE(anyLineContains(lines, "123"));
    EXPECT_TRUE(anyLineContains(lines, "Tx"));

    // Uppercase hex, space separated, which is what every ASC file uses.
    EXPECT_TRUE(anyLineContains(lines, "0A BC FF"));
}

TEST(TraceExportTests, AnErrorFrameIsWrittenAsOneNotAsData)
{
    // Otherwise it is counted as traffic at the other end - and an error frame
    // being read as a message is the sort of thing that sends somebody looking
    // for a message that does not exist.
    const ScopedFile file{"asc"};

    CanFrame bad = frame(0x100, 0);
    bad.error = true;

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.write(std::array{bad}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    EXPECT_TRUE(anyLineContains(lines, "ErrorFrame"));
    EXPECT_FALSE(anyLineContains(lines, "0A BC FF"));
}

TEST(TraceExportTests, ARemoteRequestHasALengthAndNoBytes)
{
    const ScopedFile file{"asc"};

    CanFrame remote = frame(0x200, 0);
    remote.rtr = true;

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.write(std::array{remote}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    EXPECT_TRUE(anyLineContains(lines, " r "));
    EXPECT_FALSE(anyLineContains(lines, "0A BC FF"));
}

TEST(TraceExportTests, ACSVNamesItsColumnsAndNeverMovesThem)
{
    // A script that read column six last year has to still be reading the same
    // thing, which is the whole reason the header row exists.
    const ScopedFile file{"csv"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Csv).succeeded());
        ASSERT_TRUE(exporter.write(std::array{frame(0x100, 0)}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();
    ASSERT_TRUE(lines.size() == 2);

    EXPECT_TRUE(lines[0] == "timestamp_s,channel,direction,id,extended,dlc,length,flags,data");
    EXPECT_TRUE(lines[1] == "0.000000,1,Rx,100,0,3,3,,0ABCFF");
}

TEST(TraceExportTests, CSVFlagsAreWordsRatherThanANumber)
{
    // A column reading "FD BRS" is one somebody can act on; 0x06 needs the
    // source of this file open beside it.
    const ScopedFile file{"csv"};

    CanFrame fd = frame(0x300, 0);
    fd.fd = true;
    fd.brs = true;

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Csv).succeeded());
        ASSERT_TRUE(exporter.write(std::array{fd}).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    EXPECT_TRUE(file.lines()[1].find("FD BRS") != std::string::npos);
}

TEST(TraceExportTests, AnExportWithNoFramesIsStillAValidFile)
{
    // Filter everything out, then export. That is an empty result, not a
    // failure, and an ASC file without its End TriggerBlock would read as
    // truncated rather than as empty.
    const ScopedFile asc{"asc"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(asc.path(), TraceExporter::Format::Asc).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    EXPECT_TRUE(asc.lines().back() == "End TriggerBlock");

    const ScopedFile csv{"csv"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(csv.path(), TraceExporter::Format::Csv).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    EXPECT_TRUE(csv.lines().size() == 1);
}

TEST(TraceExportTests, BatchesAndSingleFramesProduceTheSameFile)
{
    // The exporter is fed in batches so it can serve a log being read back and
    // a live trace being written out. Those two arrive in different shapes and
    // must produce the same bytes.
    std::vector<CanFrame> frames;
    for (std::uint32_t index = 0; index < 50; ++index) {
        frames.push_back(frame(0x100 + index, index * 1000ULL));
    }

    const ScopedFile all{"asc"};
    const ScopedFile piecemeal{"asc"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(all.path(), TraceExporter::Format::Asc, 0).succeeded());
        ASSERT_TRUE(exporter.write(frames).succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(piecemeal.path(), TraceExporter::Format::Asc, 0).succeeded());
        for (const CanFrame& one : frames) {
            ASSERT_TRUE(exporter.write(std::array{one}).succeeded());
        }
        ASSERT_TRUE(exporter.close().succeeded());
    }

    EXPECT_TRUE(all.text() == piecemeal.text());
}

TEST(TraceExportTests, TheDateLineSaysUTC)
{
    // A log travels between a vehicle in one timezone and an office in another.
    // A bare local time on it is a number two people read differently.
    const ScopedFile file{"asc"};

    {
        TraceExporter exporter;
        ASSERT_TRUE(exporter.open(file.path(), TraceExporter::Format::Asc, 1'757'000'000'000'000ULL)
                        .succeeded());
        ASSERT_TRUE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    EXPECT_TRUE(lines[0].find("UTC") != std::string::npos);
    EXPECT_TRUE(lines[0].find("2025") != std::string::npos);
}

TEST(TraceExportTests, EachFormatKnowsItsOwnExtension)
{
    EXPECT_TRUE(extensionFor(TraceExporter::Format::Asc) == "asc");
    EXPECT_TRUE(extensionFor(TraceExporter::Format::Csv) == "csv");
}
