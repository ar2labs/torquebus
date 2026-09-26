// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A log is read back on a different day, by a different build, from a file that
// may have stopped being written mid-sentence. Every case here is about one of
// those three.

#include "core/can/CanFrame.h"
#include "core/log/LogNodes.h"
#include "core/log/ReplayControl.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/trace/TraceStore.h"

#include "UniqueTempPath.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using torquebus::tests::uniqueTempPath;

namespace {

/// A file that deletes itself, so a failing test does not leave litter behind
/// for the next run to trip over.
class ScopedLogFile final {
public:
    ScopedLogFile()
        : m_path{uniqueTempPath("torquebus_test", counter(), ".tblog").string()}
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

[[nodiscard]] CanFrame
frame(std::uint32_t identifier, std::uint64_t timestampNs, std::uint8_t length = 8)
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

/// Builds `path` -> trace, with `control` driving the replay.
///
/// Everything is owned by the caller: a PipelineGraph holds the nodes, and a
/// helper that owned it would be handing back references into a temporary.
[[nodiscard]] Result buildReplay(const std::string& path,
                                 const NodeCatalog& catalog,
                                 TraceStore& trace,
                                 ReplayControl& control,
                                 NodeBuildContext& context,
                                 GraphDescription& description,
                                 PipelineGraph& graph,
                                 double speed = 1.0)
{
    context.traceStore = &trace;
    context.replayControl = &control;

    description.addNode(
        NodeDescription{.id = "replay",
                        .typeName = "log.source",
                        .parameters = {{"path", ParameterValue::fromText(path)},
                                       {"speed", ParameterValue::fromReal(speed)}}});
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"replay", 0, "trace", 0});

    if (Result result = description.build(catalog, context, graph); result.failed()) {
        return result;
    }

    return graph.compile();
}

/// Cuts `bytes` off the end of a file, which is what a killed process leaves.
void truncateBy(const std::string& path, std::uintmax_t bytes)
{
    const std::uintmax_t size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size > bytes ? size - bytes : 0);
}

} // namespace

TEST(TraceLogTests, AFrameSurvivesBeingWrittenDownAndReadBack)
{
    const ScopedLogFile file;

    CanFrame original = frame(0x18FEF100, 1'234'567'890, 8);
    original.format = CanFrameFormat::Extended;
    original.direction = CanDirection::Tx;
    original.fd = true;
    original.brs = true;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{original}).succeeded());
        ASSERT_TRUE(writer.flush().succeeded());
        EXPECT_TRUE(writer.framesWritten() == 1);
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 4> read{};
    ASSERT_TRUE(reader.read(read) == 1);

    const CanFrame& copy = read[0];

    // Every field that was set, and the flags one at a time - a flag byte is
    // exactly the sort of thing where one wrong bit position reads as five
    // fields all being fine.
    EXPECT_TRUE(copy.identifier == original.identifier);
    EXPECT_TRUE(copy.timestampNs == original.timestampNs);
    EXPECT_TRUE(copy.channel == original.channel);
    EXPECT_TRUE(copy.dlc == original.dlc);
    EXPECT_TRUE(copy.length == original.length);
    EXPECT_TRUE(copy.isExtended());
    EXPECT_FALSE(copy.isRx());
    EXPECT_TRUE(copy.fd);
    EXPECT_TRUE(copy.brs);
    EXPECT_FALSE(copy.esi);
    EXPECT_FALSE(copy.rtr);
    EXPECT_FALSE(copy.error);

    for (std::size_t index = 0; index < original.length; ++index) {
        EXPECT_TRUE(copy.data[index] == original.data[index]);
    }
}

TEST(TraceLogTests, FramesComeBackInTheOrderTheyWereWritten)
{
    const ScopedLogFile file;

    std::vector<CanFrame> written;
    for (std::uint32_t index = 0; index < 500; ++index) {
        written.push_back(
            frame(0x100 + index, index * 1000ULL, static_cast<std::uint8_t>(index % 9)));
    }

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(written).succeeded());
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::vector<CanFrame> read(written.size());
    const std::size_t got = reader.read(read);

    ASSERT_TRUE(got == written.size());
    EXPECT_TRUE(reader.truncatedBytes() == 0);

    for (std::size_t index = 0; index < written.size(); ++index) {
        SCOPED_TRACE(::testing::Message() << "frame " << index);
        EXPECT_TRUE(read[index].identifier == written[index].identifier);
        EXPECT_TRUE(read[index].timestampNs == written[index].timestampNs);
        EXPECT_TRUE(read[index].length == written[index].length);
    }
}

TEST(TraceLogTests, ReadingInSmallBatchesGivesTheSameFrames)
{
    // The panel will read a few hundred at a time, not the whole file. A reader
    // that only worked when asked for everything would pass every test above
    // and fail the first time it was used.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());

        for (std::uint32_t index = 0; index < 100; ++index) {
            ASSERT_TRUE(writer.append(std::array{frame(index, index)}).succeeded());
        }
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 7> batch{};
    std::uint32_t expected = 0;

    while (const std::size_t got = reader.read(batch)) {
        for (std::size_t index = 0; index < got; ++index) {
            EXPECT_TRUE(batch[index].identifier == expected);
            ++expected;
        }
    }

    EXPECT_TRUE(expected == 100);
    EXPECT_TRUE(reader.framesRead() == 100);
    EXPECT_TRUE(reader.atEnd());
}

TEST(TraceLogTests, ARecordingCutOffMidFrameReadsUpToTheLastWholeOne)
{
    // The case this format is shaped around. A log is often stopped by a flat
    // battery or a killed process, and the twenty minutes before that are
    // exactly what somebody wanted.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());

        for (std::uint32_t index = 0; index < 50; ++index) {
            ASSERT_TRUE(writer.append(std::array{frame(index, index, 8)}).succeeded());
        }
    }

    // Half of the last record: prefix written, payload not.
    truncateBy(file.path(), 12);

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::vector<CanFrame> read(64);
    const std::size_t got = reader.read(read);

    EXPECT_TRUE(got == 49);
    EXPECT_TRUE(reader.atEnd());

    // And it says so, rather than leaving "the log ends at 19:42" and "the log
    // ends at 19:42 and the last write did not finish" looking identical.
    EXPECT_TRUE(reader.truncatedBytes() > 0);
}

TEST(TraceLogTests, AFileCutOffInsideItsHeaderIsRefusedNotHalfRead)
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 0)}).succeeded());
    }

    truncateBy(file.path(), std::filesystem::file_size(file.path()) - 10);

    TraceLogReader reader;
    const Result result = reader.open(file.path());

    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::ParseError);
}

TEST(TraceLogTests, AFileThatIsNotALogSaysSoBeforeReadingFortyMegabytes)
{
    const ScopedLogFile file;

    {
        std::ofstream other{file.path(), std::ios::binary};
        other << "This is a text file, not a TorqueBus log at all, honestly.";
    }

    TraceLogReader reader;
    const Result result = reader.open(file.path());

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::ParseError);

    // Named in the message, because the commonest way to reach this is picking
    // the wrong file out of a folder.
    EXPECT_TRUE(std::string{result.message()}.find("TBLOG") != std::string::npos);
}

TEST(TraceLogTests, ALogFromANewerTorqueBusIsRefusedByVersionNotByLuck)
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 0)}).succeeded());
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

    EXPECT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::VersionMismatch);
}

TEST(TraceLogTests, TheWallClockIsCarriedAndTheFrameTimestampsAreNotTouched)
{
    // Two clocks on purpose. The frames keep nanoseconds since the measurement
    // began, which is what makes them comparable with the trace; the header
    // carries the one wall-clock reading that turns those into a date.
    const ScopedLogFile file;

    constexpr std::uint64_t kStarted = 1'757'000'000'000'000ULL;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path(), kStarted).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 42)}).succeeded());
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    EXPECT_TRUE(reader.header().startWallClockUs == kStarted);
    EXPECT_TRUE(reader.header().version == TraceLogHeader::kCurrentVersion);

    std::array<CanFrame, 1> read{};
    ASSERT_TRUE(reader.read(read) == 1);
    EXPECT_TRUE(read[0].timestampNs == 42);
}

TEST(TraceLogTests, AnEmptyLogIsAValidLog)
{
    // Press record, press stop. Nothing arrived. That is a file with a header
    // and no records, and it must open rather than look like a truncated one.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 4> read{};
    EXPECT_TRUE(reader.read(read) == 0);
    EXPECT_TRUE(reader.atEnd());
    EXPECT_TRUE(reader.truncatedBytes() == 0);
}

TEST(TraceLogTests, ARecordIsSixteenBytesPlusItsPayload)
{
    // Stated as a test because it is the claim the format makes about size: a
    // million typical frames is 24 MB rather than the 88 a CanFrame dump would
    // cost. A field added to the record silently triples a lab's disk usage.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());

        for (int index = 0; index < 100; ++index) {
            ASSERT_TRUE(writer.append(std::array{frame(0x100, 0, 8)}).succeeded());
        }
        ASSERT_TRUE(writer.flush().succeeded());
    }

    const std::uintmax_t size = std::filesystem::file_size(file.path());

    EXPECT_TRUE(size == TraceLogHeader::kSize + 100 * (16 + 8));
}

TEST(TraceLogTests, AZeroLengthFrameCostsNoPayloadBytes)
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 0, 0)}).succeeded());
        ASSERT_TRUE(writer.flush().succeeded());
    }

    EXPECT_TRUE(std::filesystem::file_size(file.path()) == TraceLogHeader::kSize + 16);

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 1> read{};
    ASSERT_TRUE(reader.read(read) == 1);
    EXPECT_TRUE(read[0].length == 0);
}

// ---------------------------------------------------------------------------
// Recording and replaying as blocks
// ---------------------------------------------------------------------------

TEST(TraceLogTests, ALogReplaysThroughThePipelineAsThoughItWereABus)
{
    // The claim rule #11 makes, tested on the last port it had not been tested
    // on: a file is a source like any other, so everything downstream works on
    // a recording without knowing it is one.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());

        for (std::uint32_t index = 0; index < 20; ++index) {
            // All at time zero, so the replay's clock releases every one of
            // them on the first pass and the test does not have to sleep.
            ASSERT_TRUE(writer.append(std::array{frame(0x200 + index, 0)}).succeeded());
        }
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    NodeBuildContext context;
    context.traceStore = &trace;

    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "replay",
                        .typeName = "log.source",
                        .parameters = {{"path", ParameterValue::fromText(file.path())}}});
    description.addNode(NodeDescription{.id = "trace", .typeName = "trace.sink"});
    description.addEdge(EdgeDescription{"replay", 0, "trace", 0});

    ASSERT_TRUE(description.validate(catalog).succeeded());

    PipelineGraph graph;
    ASSERT_TRUE(description.build(catalog, context, graph).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    EXPECT_TRUE(trace.size() == 20);
    EXPECT_TRUE(trace.row(0).frame.identifier == 0x200);
    EXPECT_TRUE(trace.row(19).frame.identifier == 0x213);
}

TEST(TraceLogTests, AReplayBlockWithNoFileIsRefusedWhileItIsOnScreen)
{
    // Required, unlike every other path in the catalog, and for a reason worth
    // stating: a decoder with no database decodes nothing and is a block you
    // have not finished configuring, while a replay with no file is a source
    // that will never produce a frame - a measurement that runs and does
    // nothing, with no error anywhere.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(NodeDescription{.id = "replay", .typeName = "log.source"});

    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("Log file") != std::string::npos);
}

TEST(TraceLogTests, ALoggerBlockWithoutAnOpenLogSaysWhichButtonToPress)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(NodeDescription{.id = "logger", .typeName = "can.log"});

    PipelineGraph graph;
    const Result result = description.build(catalog, NodeBuildContext{}, graph);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("Record") != std::string::npos);
}

TEST(TraceLogTests, WhatALoggerRecordsIsWhatReachedItNotWhatReachedTheBus)
{
    // A sink and not a side effect bolted onto the channel, which is what lets
    // a filter in front of it turn a 4 GB recording into a 40 MB one.
    const ScopedLogFile source;
    const ScopedLogFile recorded;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(source.path()).succeeded());

        for (std::uint32_t index = 0; index < 10; ++index) {
            ASSERT_TRUE(writer.append(std::array{frame(0x100 + index, 0)}).succeeded());
        }
    }

    TraceLogWriter output;
    ASSERT_TRUE(output.open(recorded.path()).succeeded());

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeBuildContext context;
    context.logWriter = &output;

    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "replay",
                        .typeName = "log.source",
                        .parameters = {{"path", ParameterValue::fromText(source.path())}}});
    description.addNode(
        NodeDescription{.id = "filter",
                        .typeName = "can.filter",
                        .parameters = {{"from", ParameterValue::fromInteger(0x105)},
                                       {"to", ParameterValue::fromInteger(0x109)}}});
    description.addNode(NodeDescription{.id = "logger", .typeName = "can.log"});
    description.addEdge(EdgeDescription{"replay", 0, "filter", 0});
    description.addEdge(EdgeDescription{"filter", 0, "logger", 0});

    PipelineGraph graph;
    ASSERT_TRUE(description.build(catalog, context, graph).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();
    ASSERT_TRUE(output.flush().succeeded());
    output.close();

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(recorded.path()).succeeded());

    std::vector<CanFrame> read(32);
    const std::size_t got = reader.read(read);

    // Five of the ten: 0x105 through 0x109.
    EXPECT_TRUE(got == 5);
    if (got == 5) {
        EXPECT_TRUE(read[0].identifier == 0x105);
        EXPECT_TRUE(read[4].identifier == 0x109);
    }
}

// ---------------------------------------------------------------------------
// The transport
// ---------------------------------------------------------------------------

TEST(TraceLogTests, ALogSaysHowLongItIsAtTheCostOfAPassOverIt)
{
    // The format carries no duration - a header patched on close is a header
    // that is wrong whenever the recording was stopped by a flat battery. So
    // the number the timeline is drawn from has to be walked out of the file.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path(), 1'757'000'000'000'000ULL).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 500'000'000)}).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x101, 1'500'000'000)}).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x102, 3'000'000'000)}).succeeded());
    }

    TraceLogSummary summary;
    ASSERT_TRUE(summarize(file.path(), summary).succeeded());

    EXPECT_TRUE(summary.frames == 3);
    EXPECT_TRUE(summary.firstTimestampNs == 500'000'000);
    EXPECT_TRUE(summary.lastTimestampNs == 3'000'000'000);

    // From the first frame to the last, not from zero: a recording that begins
    // at half a second is two and a half seconds long, and a bar drawn from
    // zero would leave a dead half-second nobody can play.
    EXPECT_TRUE(summary.durationNs == 2'500'000'000);
    EXPECT_TRUE(summary.startWallClockUs == 1'757'000'000'000'000ULL);
    EXPECT_TRUE(summary.truncatedBytes == 0);
}

TEST(TraceLogTests, ARecordingThatWasCutShortStillSaysHowLongItIs)
{
    // The case the whole format is shaped around. A summary that refused here
    // would mean no timeline for exactly the recordings people care most about.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        for (std::uint32_t index = 0; index < 100; ++index) {
            ASSERT_TRUE(
                writer.append(std::array{frame(0x300 + index, index * 10'000'000ULL)}).succeeded());
        }
    }

    truncateBy(file.path(), 9);

    TraceLogSummary summary;
    ASSERT_TRUE(summarize(file.path(), summary).succeeded());

    EXPECT_TRUE(summary.frames == 99);
    EXPECT_TRUE(summary.truncatedBytes > 0);
    EXPECT_TRUE(summary.durationNs == 98 * 10'000'000ULL);
}

TEST(TraceLogTests, AReaderCanGoBackToTheBeginning)
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x111, 0), frame(0x222, 1000)}).succeeded());
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(file.path()).succeeded());

    std::array<CanFrame, 8> frames{};
    ASSERT_TRUE(reader.read(std::span{frames}) == 2);
    EXPECT_TRUE(reader.atEnd());

    // Rewound from the end, which is where a replay always is when somebody
    // drags the handle backwards - and a stream sitting on eofbit ignores
    // seekg unless it is cleared first.
    ASSERT_TRUE(reader.restart().succeeded());
    EXPECT_FALSE(reader.atEnd());

    ASSERT_TRUE(reader.read(std::span{frames}) == 2);
    EXPECT_TRUE(frames[0].identifier == 0x111);
    EXPECT_TRUE(reader.framesRead() == 2);
}

TEST(TraceLogTests, PauseStopsTheReplayWhereItIs)
{
    // The point of the transport. Everything else is a convenience; a player
    // that cannot be stopped at the interesting second is a file being poured
    // through a pipe.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x100, 0)}).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x101, 100'000'000)}).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x102, 200'000'000)}).succeeded());
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    ReplayControl control;
    NodeBuildContext context;
    GraphDescription description;
    PipelineGraph graph;

    ASSERT_TRUE(
        buildReplay(file.path(), catalog, trace, control, context, description, graph).succeeded());

    control.setPaused(true);
    graph.execute();

    // The frame at time zero is due at position zero, paused or not: pausing
    // stops the clock, it does not un-play what the clock has already reached.
    EXPECT_TRUE(trace.size() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    graph.execute();

    // And this is the claim: three hundred milliseconds of wall clock went by
    // and the recording did not advance.
    EXPECT_TRUE(trace.size() == 1);
    EXPECT_TRUE(control.positionNs() == 0);

    control.setPaused(false);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    graph.execute();

    EXPECT_TRUE(trace.size() == 3);
    EXPECT_TRUE(control.positionNs() > 0);
}

TEST(TraceLogTests, SeekingForwardPassesOverWhatItSkips)
{
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        for (std::uint32_t index = 0; index < 4; ++index) {
            ASSERT_TRUE(writer.append(std::array{frame(0x200 + index, index * 1'000'000'000ULL)})
                            .succeeded());
        }
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    ReplayControl control;
    NodeBuildContext context;
    GraphDescription description;
    PipelineGraph graph;

    ASSERT_TRUE(
        buildReplay(file.path(), catalog, trace, control, context, description, graph).succeeded());

    // Asked for before anything has played, so nothing before the target can
    // reach the trace by accident and the count below means what it says.
    control.requestSeek(2'000'000'000);

    graph.execute(); // Services the seek. Seeking is not playing.
    EXPECT_TRUE(trace.empty());

    graph.execute(); // Now at 2 s, where a frame is waiting.

    ASSERT_TRUE(trace.size() == 1);
    EXPECT_TRUE(trace.row(0).frame.identifier == 0x202);
    EXPECT_TRUE(control.positionNs() >= 2'000'000'000);

    // Found by asking rather than by position: nodeIds() is in insertion
    // order today, and a test that quietly depends on that breaks for a reason
    // that has nothing to do with what it is checking.
    const LogSourceNode* replay = nullptr;
    for (const NodeId id : graph.nodeIds()) {
        if (const auto* candidate = graph.nodeAs<LogSourceNode>(id); candidate != nullptr) {
            replay = candidate;
        }
    }

    ASSERT_TRUE(replay != nullptr);
    EXPECT_TRUE(replay->positionNs() >= 2'000'000'000);
}

TEST(TraceLogTests, SeekingBackwardsReadsTheFileAgain)
{
    // There is no index to jump with, so going back ten seconds means going
    // back to the top and reading forward. This is the test that says the
    // rewind actually happens rather than the replay quietly staying put.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        for (std::uint32_t index = 0; index < 10; ++index) {
            // All at time zero, so one pass plays the whole file and the test
            // needs no clock.
            ASSERT_TRUE(writer.append(std::array{frame(0x400 + index, 0)}).succeeded());
        }
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    ReplayControl control;
    NodeBuildContext context;
    GraphDescription description;
    PipelineGraph graph;

    ASSERT_TRUE(
        buildReplay(file.path(), catalog, trace, control, context, description, graph).succeeded());

    graph.execute();
    ASSERT_TRUE(trace.size() == 10);

    control.requestSeek(0);
    graph.execute(); // Rewinds.
    graph.execute(); // Plays it again.

    EXPECT_TRUE(trace.size() == 20);
    EXPECT_TRUE(trace.row(10).frame.identifier == 0x400);
}

TEST(TraceLogTests, AtMaximumSpeedTheFileIsBoundedByThePipelineNotTheClock)
{
    // "Maximum" on a playback menu means "as fast as it will go", and it is a
    // speed rather than a mode so that nothing on the frame path has to branch
    // on it.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        for (std::uint32_t index = 0; index < 30; ++index) {
            // Spread over half a minute of recording, which at 1x would take
            // half a minute to play.
            ASSERT_TRUE(writer.append(std::array{frame(0x500 + index, index * 1'000'000'000ULL)})
                            .succeeded());
        }
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    ReplayControl control;
    NodeBuildContext context;
    GraphDescription description;
    PipelineGraph graph;

    ASSERT_TRUE(
        buildReplay(file.path(), catalog, trace, control, context, description, graph).succeeded());

    control.setSpeed(ReplayControl::kUnlimitedSpeed);

    // A handful of passes, not one: the first pass measures a slice of the
    // clock that may be zero on a machine whose steady_clock is coarse, and
    // the read-ahead is topped up between passes by design.
    for (int pass = 0; pass < 8 && trace.size() < 30; ++pass) {
        graph.execute();
    }

    EXPECT_TRUE(trace.size() == 30);
}

TEST(TraceLogTests, TheBlockSSpeedParameterIsWhereTheTransportStarts)
{
    // A project that says 0.5x should open playing at 0.5x. After that the
    // panel owns the speed - the parameter is a starting position, not a rival.
    const ScopedLogFile file;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(file.path()).succeeded());
        ASSERT_TRUE(writer.append(std::array{frame(0x600, 0)}).succeeded());
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    TraceStore trace{256};
    ReplayControl control;
    NodeBuildContext context;
    GraphDescription description;
    PipelineGraph graph;

    ASSERT_TRUE(buildReplay(file.path(), catalog, trace, control, context, description, graph, 0.5)
                    .succeeded());

    EXPECT_TRUE(control.speed() == 0.5);

    // And the duration was scanned at build time, so the timeline has a total
    // before the first frame is played.
    EXPECT_TRUE(control.durationNs() == 0); // One frame: no duration, honestly.
    EXPECT_TRUE(control.isActive());
}

TEST(TraceLogTests, ASpeedOutsideWhatAPlayerOffersIsBroughtBackIn)
{
    ReplayControl control;

    control.setSpeed(0.0);
    EXPECT_TRUE(control.speed() == ReplayControl::kMinimumSpeed);

    control.setSpeed(-4.0);
    EXPECT_TRUE(control.speed() == ReplayControl::kMinimumSpeed);

    control.setSpeed(1.0e30);
    EXPECT_TRUE(control.speed() == ReplayControl::kUnlimitedSpeed);
}

TEST(TraceLogTests, ASeekRequestIsTakenOnce)
{
    // The replay reads this every pass; a request that came back twice would
    // rewind the file again on the pass after it finished seeking.
    ReplayControl control;

    std::uint64_t target = 0;
    EXPECT_FALSE(control.takeSeekRequest(target));

    control.requestSeek(1234);
    ASSERT_TRUE(control.takeSeekRequest(target));
    EXPECT_TRUE(target == 1234);

    EXPECT_FALSE(control.takeSeekRequest(target));

    // A request made while an earlier one is still being served replaces it:
    // dragging a timeline produces a stream of these, and serving every
    // intermediate position would be slower than serving where it stopped.
    control.requestSeek(10);
    control.requestSeek(20);
    ASSERT_TRUE(control.takeSeekRequest(target));
    EXPECT_TRUE(target == 20);
    EXPECT_FALSE(control.takeSeekRequest(target));
}
