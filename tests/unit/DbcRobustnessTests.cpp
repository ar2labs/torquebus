// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Databases as they arrive, rather than as they were meant to be.
//
// A .dbc is written by a tool, edited by hand, merged from two others and mailed
// around, and the parser is the first thing to read it. The cases here are the ones a
// file actually brings: the container message every CANdb++ export has, a CAN FD
// database, a number somebody mistyped. For each, the rule is the same one the rest of
// the parser keeps - read what can be read, refuse what cannot with the line and the
// word in the message, and never hand a downstream node a frame it cannot trust.

#include "core/FilePath.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/simulation/RestBusNode.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>

using namespace torquebus;

namespace {

[[nodiscard]] Result parse(const std::string& text, CanDatabase& database)
{
    return DbcParser::parse(text, database);
}

[[nodiscard]] bool mentions(const Result& result, std::string_view text)
{
    return std::string{result.message()}.find(text) != std::string::npos;
}

} // namespace

TEST(DbcRobustnessTests, TheContainerForUnassignedSignalsDoesNotRefuseTheFile)
{
    // CANdb++ and CANoe write this into a very large share of the databases they export:
    // the message numbered 0xC0000000, whose identifier does not fit 29 bits, holding the
    // signals that belong to no frame. The parser took it for a message and refused the
    // whole file over the one entry in it that is not one.
    CanDatabase database;
    const Result result = parse(R"(
VERSION ""
BU_: ECU
BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX
 SG_ OrphanA : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ OrphanB : 8|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 256 Real: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
)",
                                database);

    ASSERT_TRUE(result.succeeded()) << result.message();

    // The container is not a message and its signals are not on any frame; what
    // follows it is read as if it had not been there.
    ASSERT_EQ(database.messageCount(), 1U);
    ASSERT_NE(database.findByName("Real"), nullptr);
    EXPECT_EQ(database.findByName("Real")->signalList.size(), 1U);
    EXPECT_EQ(database.findByName("VECTOR__INDEPENDENT_SIG_MSG"), nullptr);
}

TEST(DbcRobustnessTests, ASignalInTheLastByteOfACanFdMessageIsAccepted)
{
    // 64 bytes is 512 bits, and a signal in the last byte starts at bit 504. The limit
    // used to be a classic frame's 63, and a database that describes a CAN FD message
    // was refused at its first signal past the eighth byte.
    CanDatabase database;
    const Result result = parse(R"(
BO_ 300 BigFrame: 64 ECU
 SG_ Early : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ Late : 504|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ Motorola : 263|16@0+ (1,0) [0|65535] "" Vector__XXX
)",
                                database);

    ASSERT_TRUE(result.succeeded()) << result.message();

    const CanMessage* message = database.findByName("BigFrame");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->length, 64);

    // And they decode where they are.
    std::uint8_t payload[64] = {};
    payload[63] = 0xAB;
    EXPECT_EQ(message->findSignal("Late")->rawValue(payload, 64), 0xAB);
    EXPECT_EQ(message->findSignal("Early")->rawValue(payload, 64), 0);

    // A frame that does not carry the byte is truncated, not read past its end.
    EXPECT_FALSE(message->findSignal("Late")->fitsIn(8));
    EXPECT_EQ(message->findSignal("Late")->rawValue(payload, 8), 0);
}

TEST(DbcRobustnessTests, AStartBitBeyondTheLargestFrameIsStillRefused)
{
    CanDatabase database;
    const Result result = parse(R"(
BO_ 300 TooFar: 64 ECU
 SG_ Nowhere : 512|8@1+ (1,0) [0|255] "" Vector__XXX
)",
                                database);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::ParseError);
    EXPECT_TRUE(mentions(result, "line 3")) << result.message();
    EXPECT_TRUE(mentions(result, "512|8")) << result.message();
}

TEST(DbcRobustnessTests, ANegativePayloadLengthIsRefusedNotNarrowedTo255)
{
    // It was narrowed. A message of length 255 makes a frame that claims four times the
    // payload there is room for, and everything that trusts a frame's length - the Lua
    // bindings, the trace, the decoder - then reads past the end of its array.
    CanDatabase database;
    const Result result = parse("BO_ 256 Broken: -1 ECU\n", database);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::ParseError);
    EXPECT_TRUE(mentions(result, "Broken")) << result.message();
    EXPECT_TRUE(mentions(result, "-1")) << result.message();
    EXPECT_TRUE(mentions(result, "0 to 64")) << result.message();
}

TEST(DbcRobustnessTests, APayloadLengthAboveSixtyFourIsRefusedNotQuietlyClamped)
{
    CanDatabase database;
    const Result result = parse("BO_ 256 Huge: 200 ECU\n", database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "200")) << result.message();
}

TEST(DbcRobustnessTests, AnIdentifierPastThirtyTwoBitsIsRefusedNotTruncated)
{
    // 4294967297 is 2^32 + 1. Narrowed, it was identifier 1: some other message's.
    CanDatabase database;
    const Result result = parse("BO_ 4294967297 Wrapped: 8 ECU\n", database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "Wrapped")) << result.message();
    EXPECT_TRUE(mentions(result, "32-bit")) << result.message();

    CanDatabase negative;
    EXPECT_TRUE(parse("BO_ -5 Below: 8 ECU\n", negative).failed());
}

TEST(DbcRobustnessTests, ANumberThatIsNotAWholeNumberIsRefusedWithItsLine)
{
    // "1.2.3" used to read as 1.2: a factor the file never said.
    CanDatabase database;
    const Result result = parse(R"(
BO_ 256 M: 8 ECU
 SG_ Value : 0|8@1+ (1.2.3,0) [0|255] "" Vector__XXX
)",
                                database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "line 3")) << result.message();
    EXPECT_TRUE(mentions(result, "factor")) << result.message();
}

TEST(DbcRobustnessTests, ANumberWithAPlusSignAndAnExponentIsRead)
{
    // The other direction: from_chars takes neither a leading plus nor a locale, and a
    // .dbc writes both a plus and an exponent now and then.
    CanDatabase database;
    const Result result = parse(R"(
BO_ 256 M: 8 ECU
 SG_ Value : 0|16@1+ (+2.5E-1,-1e2) [0|1e3] "" Vector__XXX
)",
                                database);

    ASSERT_TRUE(result.succeeded()) << result.message();

    const CanSignal* signal = database.findByName("M")->findSignal("Value");
    ASSERT_NE(signal, nullptr);
    EXPECT_DOUBLE_EQ(signal->factor, 0.25);
    EXPECT_DOUBLE_EQ(signal->offset, -100.0);
    EXPECT_DOUBLE_EQ(signal->maximum, 1000.0);
}

TEST(DbcRobustnessTests, ANumberTooBigForADoubleIsRefusedNotReadAsInfinity)
{
    CanDatabase database;
    const Result result = parse(R"(
BO_ 256 M: 8 ECU
 SG_ Value : 0|8@1+ (1e999,0) [0|255] "" Vector__XXX
)",
                                database);

    EXPECT_TRUE(result.failed());
}

TEST(DbcRobustnessTests, AFolderWhereADatabaseWasNamedIsSaidToBeAFolder)
{
    // On some platforms a folder opens as a stream and reads as nothing, which parses as
    // a database with no messages: a decoder that decodes nothing and says nothing.
    const std::filesystem::path folder = std::filesystem::temp_directory_path();

    CanDatabase database;
    const Result result = DbcParser::parseFile(utf8FromPath(folder), database);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(mentions(result, "folder")) << result.message();
}

TEST(DbcRobustnessTests, AFileInAFolderWithAccentsInItsNameIsFound)
{
    // "Área de Trabalho", "Usuários", a project folder called "Simulação": the window
    // opens them through Qt, which converts properly, and the core used to read the same
    // UTF-8 bytes as Windows-1252 and report the file missing from a folder the person
    // was looking at.
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path()
        / pathFromUtf8(std::format("torquebus-dbc-Simulação-ñ-{:08x}", std::random_device{}()));
    std::filesystem::create_directories(folder);

    const std::filesystem::path file = folder / pathFromUtf8("rede-ção.dbc");
    {
        std::ofstream out{file, std::ios::binary};
        out << "BO_ 256 Accented: 8 ECU\n SG_ Value : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n";
    }

    CanDatabase database;
    const Result result = DbcParser::parseFile(utf8FromPath(file), database);

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_EQ(database.messageCount(), 1U);
    EXPECT_EQ(database.sourcePath, utf8FromPath(file));
}

// ---------------------------------------------------------------------------
// What a database makes when it is used to send
// ---------------------------------------------------------------------------

TEST(DbcRobustnessTests, AMessageOfMoreThanEightBytesMakesACanFdFrame)
{
    // It used to make a classic frame with a length of twelve and a code of eight, which
    // exists nowhere: a driver refuses it and a receiver cannot read it.
    CanMessage message;
    message.identifier = 0x123;
    message.length = 12;

    const CanFrame frame = message.makeFrame();

    EXPECT_TRUE(frame.fd);
    EXPECT_EQ(frame.length, 12);
    EXPECT_EQ(frame.dlc, 9) << "9 is the code for a twelve-byte payload";
    EXPECT_EQ(payloadLengthFromDlc(frame.dlc, frame.fd), frame.length);
}

TEST(DbcRobustnessTests, ALengthBetweenTwoFdStepsIsCarriedByTheNextOneUp)
{
    CanMessage message;
    message.length = 9;

    const CanFrame frame = message.makeFrame();

    EXPECT_TRUE(frame.fd);
    EXPECT_EQ(frame.length, 12) << "FD payloads come in steps; the spare bytes are zero";
    EXPECT_EQ(frame.data[11], 0);
}

TEST(DbcRobustnessTests, AClassicMessageStaysAClassicFrame)
{
    CanMessage message;
    message.length = 8;

    const CanFrame frame = message.makeFrame();

    EXPECT_FALSE(frame.fd);
    EXPECT_EQ(frame.length, 8);
    EXPECT_EQ(frame.dlc, 8);
}

TEST(DbcRobustnessTests, ALengthPastTheArrayCannotMakeAFrameThatReadsPastIt)
{
    // A database built in code has not been through the parser's check. 255 here used to
    // be a frame with length 255 and room for 64.
    CanMessage message;
    message.length = 255;

    const CanFrame frame = message.makeFrame();

    EXPECT_LE(frame.length, kMaxCanPayload);
    EXPECT_EQ(frame.length, 64);
}

TEST(DbcRobustnessTests, ARestBusFromACanFdDatabaseSendsFdFrames)
{
    auto database = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parse(R"(
BU_: ECU
BO_ 300 BigFrame: 24 ECU
 SG_ Late : 184|8@1+ (1,0) [0|255] "" Vector__XXX
BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 300 10;
)",
                                 *database)
                    .succeeded());

    RestBusNode node;
    node.setDatabase(database);
    ASSERT_TRUE(node.prepare(16).succeeded());

    std::array<PortBatch, 1> outputs{};
    NodeContext context{{}, outputs};

    // Due on the first pass at or after its staggered start; a few passes cover it.
    for (int pass = 0; pass < 40 && outputs[0].as<CanFrame>().empty(); ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        node.process(context);
    }

    ASSERT_FALSE(outputs[0].as<CanFrame>().empty());

    const CanFrame sent = outputs[0].as<CanFrame>().front();
    EXPECT_TRUE(sent.fd);
    EXPECT_EQ(sent.length, 24);
    EXPECT_EQ(sent.identifier, 300U);
}

// ---------------------------------------------------------------------------
// Encodings
// ---------------------------------------------------------------------------

TEST(DbcRobustnessTests, ADatabaseSavedInWindows1252HasItsUnitsAndAccentsRead)
{
    // CANdb++ and most of what writes these files on Windows save in the ANSI code page: the
    // degree sign is the single byte B0, and the cedilla in a Portuguese comment is E7. Read
    // as UTF-8 they were replacement characters - in the unit of every temperature signal, and
    // in every comment with an accent in it.
    const std::string text = std::string{"BO_ 256 Engine: 8 ECU\n"
                                         " SG_ Coolant : 0|8@1+ (1,-40) [-40|215] \"\xB0"}
                             + "C\" Vector__XXX\n"
                               "CM_ SG_ 256 Coolant \"Temperatura do l\xED"
                             + "quido de arrefecimento, a\xE7\xE3o do term\xF3" + "stato\";\n";

    CanDatabase database;
    ASSERT_TRUE(parse(text, database).succeeded());

    const CanSignal* signal = database.findByName("Engine")->findSignal("Coolant");
    ASSERT_NE(signal, nullptr);

    EXPECT_EQ(signal->unit,
              "\xC2\xB0"
              "C")
        << "the degree sign did not come out as UTF-8";
    EXPECT_EQ(signal->comment,
              "Temperatura do l\xC3\xAD"
              "quido de arrefecimento, a\xC3\xA7\xC3\xA3o do term\xC3\xB3"
              "stato");
}

TEST(DbcRobustnessTests, ADatabaseThatIsAlreadyUtf8IsLeftAlone)
{
    // The other half: converting what needed no conversion would turn "°" into "Â°".
    const std::string text = "BO_ 256 Engine: 8 ECU\n"
                             " SG_ Coolant : 0|8@1+ (1,-40) [-40|215] \"\xC2\xB0"
                             "C\" Vector__XXX\n";

    CanDatabase database;
    ASSERT_TRUE(parse(text, database).succeeded());

    EXPECT_EQ(database.findByName("Engine")->findSignal("Coolant")->unit,
              "\xC2\xB0"
              "C");
}

TEST(DbcRobustnessTests, AByteOrderMarkDoesNotCostTheFirstStatement)
{
    // Notepad puts one there when it saves as UTF-8. It made the first keyword unreadable, and
    // a statement the parser cannot read is one it skips - so the first line was not read, and
    // nothing said so.
    const std::string text = "\xEF\xBB\xBF"
                             "BO_ 256 First: 8 ECU\n"
                             " SG_ Value : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n";

    CanDatabase database;
    ASSERT_TRUE(parse(text, database).succeeded());

    EXPECT_NE(database.findByName("First"), nullptr);
}

TEST(DbcRobustnessTests, TextThatIsNotAnyEncodingStillParsesWithoutReadingPastItsEnd)
{
    // A UTF-8 lead byte as the last byte of the file, and a continuation byte on its own.
    CanDatabase database;

    EXPECT_NO_THROW(static_cast<void>(parse("BO_ 256 M: 8 ECU\n CM_ BO_ 256 \"x\xE2", database)));
    EXPECT_NO_THROW(static_cast<void>(parse("\x80\x80\xC3", database)));
    EXPECT_NO_THROW(static_cast<void>(parse("\xF0\x9F", database)));
}

TEST(DbcRobustnessTests, AVeryLargeDatabaseLoadsInReasonableTime)
{
    // Not a benchmark: a floor. A whole vehicle's database is a few thousand messages and
    // tens of thousands of signals; this is twice that, in a debug build. Anything in the
    // parser that is quadratic in the number of messages or signals - a search where a hash
    // belongs - would turn this from a second into minutes, and the person opening the
    // project would think the window had hung.
    constexpr int kMessages = 4000;
    constexpr int kSignalsPerMessage = 24;

    // Extended identifiers, so that four thousand of them fit: the flag is bit 31 of the number.
    constexpr std::uint64_t kExtendedFlag = 0x8000'0000ULL;

    std::string text = "VERSION \"big\"\nBU_: A B C\n";
    text.reserve(static_cast<std::size_t>(kMessages) * kSignalsPerMessage * 90U);

    for (int message = 0; message < kMessages; ++message) {
        text += "BO_ " + std::to_string(kExtendedFlag + static_cast<std::uint64_t>(message))
                + " Message_" + std::to_string(message) + ": 64 A\n";

        for (int signal = 0; signal < kSignalsPerMessage; ++signal) {
            text += " SG_ Signal_" + std::to_string(signal) + " : " + std::to_string(signal * 16)
                    + "|16@1+ (0.5,0) [0|100] \"unit\" B,C\n";
        }
    }

    for (int message = 0; message < kMessages; ++message) {
        const std::string id = std::to_string(kExtendedFlag + static_cast<std::uint64_t>(message));

        text += "VAL_ " + id + " Signal_3 0 \"off\" 1 \"on\" ;\n";
        text += "CM_ SG_ " + id + " Signal_5 \"a comment\";\n";
        text += "BA_ \"GenMsgCycleTime\" BO_ " + id + " 100;\n";
    }

    const auto started = std::chrono::steady_clock::now();

    CanDatabase database;
    const Result result = parse(text, database);

    const auto took = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_EQ(database.messageCount(), static_cast<std::size_t>(kMessages));
    EXPECT_EQ(database.signalCount(), static_cast<std::size_t>(kMessages) * kSignalsPerMessage);
    EXPECT_LT(took, std::chrono::seconds{30})
        << "parsing " << text.size() / 1024 << " KiB took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms";
}
