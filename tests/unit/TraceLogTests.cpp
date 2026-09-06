// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A log is read back on a different day, by a different build, from a file that
// may have stopped being written mid-sentence. Every case here is about one of
// those three.

#include "core/can/CanFrame.h"
#include "core/log/TraceLog.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// A file that deletes itself, so a failing test does not leave litter behind
/// for the next run to trip over.
class ScopedLogFile final {
public:
    ScopedLogFile()
        : m_path{(std::filesystem::temp_directory_path()
                  / std::filesystem::path{"torquebus_test_" + std::to_string(counter()) + ".tblog"})
                     .string()}
    {
        std::filesystem::remove(m_path);
    }

    ~ScopedLogFile()
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    ScopedLogFile(const ScopedLogFile&) = delete;
    ScopedLogFile& operator=(const ScopedLogFile&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    [[nodiscard]] static int counter()
    {
        static int next = 0;
        return ++next;
    }

    std::string m_path;
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier,
                             std::uint64_t timestampNs,
                             std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = identifier;
    result.timestampNs = timestampNs;
    result.length = length;
    result.dlc = length;
    result.channel = 1;

    for (std::uint8_t index = 0; index < length; ++index) {
        result.data[index] = static_cast<std::uint8_t>(0xA0 + index);
    }

    return result;
}

/// Cuts `bytes` off the end of a file, which is what a killed process leaves.
void truncateBy(const std::string& path, std::uintmax_t bytes)
{
    const std::uintmax_t size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size > bytes ? size - bytes : 0);
}

} // namespace

TEST_CASE("A frame survives being written down and read back", "[log]")
{
    const ScopedLogFile file;

    CanFrame original = frame(0x18FEF100, 1'234'567'890, 8);
    original.format = CanFrameFormat::Extended;
    original.direction = CanDirection::Tx;
    original.fd = true;
    original.brs = true;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
        REQUIRE(writer.append(std::array{original}).succeeded());
        REQUIRE(writer.flush().succeeded());
        CHECK(writer.framesWritten() == 1);
    }

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 4> read{};
    REQUIRE(reader.read(read) == 1);

    const CanFrame& copy = read[0];

    // Every field that was set, and the flags one at a time - a flag byte is
    // exactly the sort of thing where one wrong bit position reads as five
    // fields all being fine.
    CHECK(copy.identifier == original.identifier);
    CHECK(copy.timestampNs == original.timestampNs);
    CHECK(copy.channel == original.channel);
    CHECK(copy.dlc == original.dlc);
    CHECK(copy.length == original.length);
    CHECK(copy.isExtended());
    CHECK_FALSE(copy.isRx());
    CHECK(copy.fd);
    CHECK(copy.brs);
    CHECK_FALSE(copy.esi);
    CHECK_FALSE(copy.rtr);
    CHECK_FALSE(copy.error);

    for (std::size_t index = 0; index < original.length; ++index) {
        CHECK(copy.data[index] == original.data[index]);
    }
}

TEST_CASE("Frames come back in the order they were written", "[log]")
{
    const ScopedLogFile file;

    std::vector<CanFrame> written;
    for (std::uint32_t index = 0; index < 500; ++index) {
        written.push_back(frame(0x100 + index, index * 1000ULL,
                                static_cast<std::uint8_t>(index % 9)));
    }

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
        REQUIRE(writer.append(written).succeeded());
    }

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::vector<CanFrame> read(written.size());
    const std::size_t got = reader.read(read);

    REQUIRE(got == written.size());
    CHECK(reader.truncatedBytes() == 0);

    for (std::size_t index = 0; index < written.size(); ++index) {
        INFO("frame " << index);
        CHECK(read[index].identifier == written[index].identifier);
        CHECK(read[index].timestampNs == written[index].timestampNs);
        CHECK(read[index].length == written[index].length);
    }
}

TEST_CASE("Reading in small batches gives the same frames", "[log]")
{
    // The panel will read a few hundred at a time, not the whole file. A reader
    // that only worked when asked for everything would pass every test above
    // and fail the first time it was used.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());

        for (std::uint32_t index = 0; index < 100; ++index) {
            REQUIRE(writer.append(std::array{frame(index, index)}).succeeded());
        }
    }

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 7> batch{};
    std::uint32_t expected = 0;

    while (const std::size_t got = reader.read(batch)) {
        for (std::size_t index = 0; index < got; ++index) {
            CHECK(batch[index].identifier == expected);
            ++expected;
        }
    }

    CHECK(expected == 100);
    CHECK(reader.framesRead() == 100);
    CHECK(reader.atEnd());
}

TEST_CASE("A recording cut off mid-frame reads up to the last whole one", "[log]")
{
    // The case this format is shaped around. A log is often stopped by a flat
    // battery or a killed process, and the twenty minutes before that are
    // exactly what somebody wanted.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());

        for (std::uint32_t index = 0; index < 50; ++index) {
            REQUIRE(writer.append(std::array{frame(index, index, 8)}).succeeded());
        }
    }

    // Half of the last record: prefix written, payload not.
    truncateBy(file.path(), 12);

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::vector<CanFrame> read(64);
    const std::size_t got = reader.read(read);

    CHECK(got == 49);
    CHECK(reader.atEnd());

    // And it says so, rather than leaving "the log ends at 19:42" and "the log
    // ends at 19:42 and the last write did not finish" looking identical.
    CHECK(reader.truncatedBytes() > 0);
}

TEST_CASE("A file cut off inside its header is refused, not half-read", "[log]")
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
        REQUIRE(writer.append(std::array{frame(0x100, 0)}).succeeded());
    }

    truncateBy(file.path(), std::filesystem::file_size(file.path()) - 10);

    TraceLogReader reader;
    const Result result = reader.open(file.path());

    CHECK(result.failed());
    CHECK(result.code() == ErrorCode::ParseError);
}

TEST_CASE("A file that is not a log says so before reading forty megabytes",
          "[log]")
{
    const ScopedLogFile file;

    {
        std::ofstream other{file.path(), std::ios::binary};
        other << "This is a text file, not a TorqueBus log at all, honestly.";
    }

    TraceLogReader reader;
    const Result result = reader.open(file.path());

    REQUIRE(result.failed());
    CHECK(result.code() == ErrorCode::ParseError);

    // Named in the message, because the commonest way to reach this is picking
    // the wrong file out of a folder.
    CHECK(std::string{result.message()}.find("TBLOG") != std::string::npos);
}

TEST_CASE("A log from a newer TorqueBus is refused by version, not by luck",
          "[log]")
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
        REQUIRE(writer.append(std::array{frame(0x100, 0)}).succeeded());
    }

    // The version field is the two bytes after the magic.
    {
        std::fstream patch{file.path(), std::ios::binary | std::ios::in | std::ios::out};
        patch.seekp(TraceLogHeader::kMagicSize, std::ios::beg);

        const char newer[2] = {static_cast<char>(TraceLogHeader::kCurrentVersion + 1), 0};
        patch.write(newer, 2);
    }

    TraceLogReader reader;
    const Result result = reader.open(file.path());

    CHECK(result.failed());
    CHECK(result.code() == ErrorCode::VersionMismatch);
}

TEST_CASE("The wall clock is carried, and the frame timestamps are not touched",
          "[log]")
{
    // Two clocks on purpose. The frames keep nanoseconds since the measurement
    // began, which is what makes them comparable with the trace; the header
    // carries the one wall-clock reading that turns those into a date.
    const ScopedLogFile file;

    constexpr std::uint64_t kStarted = 1'757'000'000'000'000ULL;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path(), kStarted).succeeded());
        REQUIRE(writer.append(std::array{frame(0x100, 42)}).succeeded());
    }

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    CHECK(reader.header().startWallClockUs == kStarted);
    CHECK(reader.header().version == TraceLogHeader::kCurrentVersion);

    std::array<CanFrame, 1> read{};
    REQUIRE(reader.read(read) == 1);
    CHECK(read[0].timestampNs == 42);
}

TEST_CASE("An empty log is a valid log", "[log]")
{
    // Press record, press stop. Nothing arrived. That is a file with a header
    // and no records, and it must open rather than look like a truncated one.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
    }

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 4> read{};
    CHECK(reader.read(read) == 0);
    CHECK(reader.atEnd());
    CHECK(reader.truncatedBytes() == 0);
}

TEST_CASE("A record is sixteen bytes plus its payload", "[log]")
{
    // Stated as a test because it is the claim the format makes about size: a
    // million typical frames is 24 MB rather than the 88 a CanFrame dump would
    // cost. A field added to the record silently triples a lab's disk usage.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());

        for (int index = 0; index < 100; ++index) {
            REQUIRE(writer.append(std::array{frame(0x100, 0, 8)}).succeeded());
        }
        REQUIRE(writer.flush().succeeded());
    }

    const std::uintmax_t size = std::filesystem::file_size(file.path());

    CHECK(size == TraceLogHeader::kSize + 100 * (16 + 8));
}

TEST_CASE("A zero-length frame costs no payload bytes", "[log]")
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        REQUIRE(writer.open(file.path()).succeeded());
        REQUIRE(writer.append(std::array{frame(0x100, 0, 0)}).succeeded());
        REQUIRE(writer.flush().succeeded());
    }

    CHECK(std::filesystem::file_size(file.path()) == TraceLogHeader::kSize + 16);

    TraceLogReader reader;
    REQUIRE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 1> read{};
    REQUIRE(reader.read(read) == 1);
    CHECK(read[0].length == 0);
}
