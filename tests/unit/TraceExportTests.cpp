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

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

class ScopedFile final {
public:
    explicit ScopedFile(const char* extension)
        : m_path{(std::filesystem::temp_directory_path()
                  / std::filesystem::path{"torquebus_export_" + std::to_string(counter()) + "."
                                          + extension})
                     .string()}
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
[[nodiscard]] bool anyLineContains(const std::vector<std::string>& lines,
                                   const std::string& needle)
{
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("An ASC file has the preamble every reader looks for", "[export][asc]")
{
    const ScopedFile file{"asc"};

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.write(std::array{frame(0x100, 0)}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();
    REQUIRE(lines.size() >= 6);

    CHECK(lines[0].starts_with("date "));

    // Both of these describe every line below, and a reader that does not find
    // them guesses - usually at decimal, which turns 0x100 into 256.
    CHECK(anyLineContains(lines, "base hex"));
    CHECK(anyLineContains(lines, "timestamps absolute"));
    CHECK(anyLineContains(lines, "Begin Triggerblock"));

    // Without this a reader treats the file as truncated.
    CHECK(lines.back() == "End TriggerBlock");
}

TEST_CASE("An extended identifier carries its x", "[export][asc]")
{
    // The commonest way an exported file decodes wrongly at the other end:
    // 0x100 standard and 0x100 extended are two different messages on one bus,
    // and the `x` is the only thing that tells them apart.
    const ScopedFile file{"asc"};

    CanFrame extended = frame(0x18FEF100, 1'000'000);
    extended.format = CanFrameFormat::Extended;

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.write(std::array{extended, frame(0x7AB, 2'000'000)}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    CHECK(anyLineContains(lines, "18FEF100x"));

    // And the standard one does not get one by accident. Checked against an
    // identifier that is not a suffix of the extended one - the first version
    // of this looked for "100x " and found it inside "18FEF100x", which is a
    // test that passes for the wrong reason in one direction and fails for the
    // wrong reason in the other.
    CHECK(anyLineContains(lines, "7AB "));
    CHECK_FALSE(anyLineContains(lines, "7ABx"));
}

TEST_CASE("A frame line carries the time, channel, direction and bytes",
          "[export][asc]")
{
    const ScopedFile file{"asc"};

    CanFrame sent = frame(0x123, 1'500'000);   // 0.0015 s
    sent.direction = CanDirection::Tx;
    sent.channel = 1;                          // CAN 2, in the numbering people read

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.write(std::array{sent}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    CHECK(anyLineContains(lines, "0.001500"));
    CHECK(anyLineContains(lines, "123"));
    CHECK(anyLineContains(lines, "Tx"));

    // Uppercase hex, space separated, which is what every ASC file uses.
    CHECK(anyLineContains(lines, "0A BC FF"));
}

TEST_CASE("An error frame is written as one, not as data", "[export][asc]")
{
    // Otherwise it is counted as traffic at the other end - and an error frame
    // being read as a message is the sort of thing that sends somebody looking
    // for a message that does not exist.
    const ScopedFile file{"asc"};

    CanFrame bad = frame(0x100, 0);
    bad.error = true;

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.write(std::array{bad}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    CHECK(anyLineContains(lines, "ErrorFrame"));
    CHECK_FALSE(anyLineContains(lines, "0A BC FF"));
}

TEST_CASE("A remote request has a length and no bytes", "[export][asc]")
{
    const ScopedFile file{"asc"};

    CanFrame remote = frame(0x200, 0);
    remote.rtr = true;

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.write(std::array{remote}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    CHECK(anyLineContains(lines, " r "));
    CHECK_FALSE(anyLineContains(lines, "0A BC FF"));
}

TEST_CASE("A CSV names its columns and never moves them", "[export][csv]")
{
    // A script that read column six last year has to still be reading the same
    // thing, which is the whole reason the header row exists.
    const ScopedFile file{"csv"};

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Csv).succeeded());
        REQUIRE(exporter.write(std::array{frame(0x100, 0)}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();
    REQUIRE(lines.size() == 2);

    CHECK(lines[0] == "timestamp_s,channel,direction,id,extended,dlc,length,flags,data");
    CHECK(lines[1] == "0.000000,1,Rx,100,0,3,3,,0ABCFF");
}

TEST_CASE("CSV flags are words rather than a number", "[export][csv]")
{
    // A column reading "FD BRS" is one somebody can act on; 0x06 needs the
    // source of this file open beside it.
    const ScopedFile file{"csv"};

    CanFrame fd = frame(0x300, 0);
    fd.fd = true;
    fd.brs = true;

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Csv).succeeded());
        REQUIRE(exporter.write(std::array{fd}).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    CHECK(file.lines()[1].find("FD BRS") != std::string::npos);
}

TEST_CASE("An export with no frames is still a valid file", "[export]")
{
    // Filter everything out, then export. That is an empty result, not a
    // failure, and an ASC file without its End TriggerBlock would read as
    // truncated rather than as empty.
    const ScopedFile asc{"asc"};

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(asc.path(), TraceExporter::Format::Asc).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    CHECK(asc.lines().back() == "End TriggerBlock");

    const ScopedFile csv{"csv"};

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(csv.path(), TraceExporter::Format::Csv).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    CHECK(csv.lines().size() == 1);
}

TEST_CASE("Batches and single frames produce the same file", "[export]")
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
        REQUIRE(exporter.open(all.path(), TraceExporter::Format::Asc, 0).succeeded());
        REQUIRE(exporter.write(frames).succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(piecemeal.path(), TraceExporter::Format::Asc, 0).succeeded());
        for (const CanFrame& one : frames) {
            REQUIRE(exporter.write(std::array{one}).succeeded());
        }
        REQUIRE(exporter.close().succeeded());
    }

    CHECK(all.text() == piecemeal.text());
}

TEST_CASE("The date line says UTC", "[export][asc]")
{
    // A log travels between a vehicle in one timezone and an office in another.
    // A bare local time on it is a number two people read differently.
    const ScopedFile file{"asc"};

    {
        TraceExporter exporter;
        REQUIRE(exporter.open(file.path(), TraceExporter::Format::Asc,
                              1'757'000'000'000'000ULL)
                    .succeeded());
        REQUIRE(exporter.close().succeeded());
    }

    const std::vector<std::string> lines = file.lines();

    CHECK(lines[0].find("UTC") != std::string::npos);
    CHECK(lines[0].find("2025") != std::string::npos);
}

TEST_CASE("Each format knows its own extension", "[export]")
{
    CHECK(extensionFor(TraceExporter::Format::Asc) == "asc");
    CHECK(extensionFor(TraceExporter::Format::Csv) == "csv");
}
