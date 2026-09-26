// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The databases below are embedded rather than read from disk. Two reasons,
// and the second is the real one:
//
//   - the test suite runs from wherever CTest is invoked, and a test that
//     depends on a relative path is a test that passes on one machine;
//   - a fixture you can see next to the assertion is a fixture you can check.
//     Half of these cases exist because a .dbc says something subtle, and the
//     subtlety has to be visible.
//
// `kMotohawk` is the example database cantools ships, reproduced exactly,
// including the NS_ block that this parser failed on the first time it ran
// against a real file.

#include <gtest/gtest.h>

#include "core/database/DbcParser.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace torquebus;

namespace {

/// The cantools example database, verbatim.
constexpr const char* kMotohawk = R"(VERSION "1.0"

NS_ :
	NS_DESC_
	CM_
	BA_DEF_
	BA_
	VAL_
	CAT_DEF_
	BA_DEF_DEF_
	SG_MUL_VAL_

BS_:

BU_: PCM1 FOO

BO_ 496 ExampleMessage: 8 PCM1
 SG_ Temperature : 0|12@0- (0.01,250) [229.52|270.47] "degK"  PCM1,FOO
 SG_ AverageRadius : 6|6@0+ (0.1,0) [0|5] "m" Vector__XXX
 SG_ Enable : 7|1@0+ (1,0) [0|0] "-" Vector__XXX

CM_ BO_ 496 "Example message used as template in MotoHawk models.";
BA_DEF_ BO_  "GenMsgCycleTime" INT 0 65535;
BA_DEF_DEF_  "GenMsgCycleTime" 0;

VAL_ 496 Enable 0 "Disabled" 1 "Enabled" ;
)";

[[nodiscard]] CanDatabase parsed(const char* text)
{
    CanDatabase database;
    const Result result = DbcParser::parse(text, database);
    EXPECT_TRUE(result.succeeded());
    return database;
}

[[nodiscard]] bool near(double actual, double expected)
{
    return (actual - expected) < 1e-9 && (expected - actual) < 1e-9;
}

} // namespace

TEST(DbcParserTests, TheKeywordListInNSIsNotASetOfSections)
{
    // A file that declares it may use CM_ contains a line that is exactly
    // `CM_`. Reading that as a comment section is how this parser failed on
    // every real database the first time it ran - all three fixtures, at line
    // 7, with an error about a missing quote.
    const CanDatabase database = parsed(kMotohawk);

    EXPECT_TRUE(database.messageCount() == 1);
    EXPECT_TRUE(database.version == "1.0");
}

TEST(DbcParserTests, AMessageAndItsSignalsComeBackAsTheFileDescribesThem)
{
    const CanDatabase database = parsed(kMotohawk);

    const CanMessage* message = database.find(0x1F0, CanFrameFormat::Standard);
    ASSERT_TRUE(message != nullptr);

    EXPECT_TRUE(message->name == "ExampleMessage");
    EXPECT_TRUE(message->length == 8);
    EXPECT_TRUE(message->transmitter == "PCM1");
    EXPECT_TRUE(message->signalList.size() == 3);
    EXPECT_TRUE(message->comment == "Example message used as template in MotoHawk models.");

    const CanSignal* temperature = message->findSignal("Temperature");
    ASSERT_TRUE(temperature != nullptr);
    EXPECT_TRUE(temperature->startBit == 0);
    EXPECT_TRUE(temperature->bitLength == 12);
    EXPECT_TRUE(temperature->byteOrder == ByteOrder::Motorola);
    EXPECT_TRUE(temperature->isSigned);
    EXPECT_TRUE(near(temperature->factor, 0.01));
    EXPECT_TRUE(near(temperature->offset, 250.0));
    EXPECT_TRUE(temperature->unit == "degK");

    // Vector__XXX means "unspecified", so it is dropped rather than recorded as
    // a node named Vector__XXX.
    EXPECT_TRUE((temperature->receivers == std::vector<std::string>{"PCM1", "FOO"}));
    EXPECT_TRUE(message->findSignal("AverageRadius")->receivers.empty());
}

TEST(DbcParserTests, AParsedDatabaseDecodesTheVectorTheFileWasPublishedWith)
{
    // The end-to-end check: file text in, physical values out. Every part of
    // the parse has to be right for this to pass, which is why it is worth
    // having on top of the field-by-field assertions above.
    const CanDatabase database = parsed(kMotohawk);
    const std::uint8_t payload[8] = {0xC0, 0x06, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};

    const CanMessage* message = database.find(0x1F0, CanFrameFormat::Standard);
    ASSERT_TRUE(message != nullptr);

    EXPECT_TRUE(near(message->findSignal("Temperature")->decode(payload, 8), 250.55));
    EXPECT_TRUE(near(message->findSignal("AverageRadius")->decode(payload, 8), 3.2));
    EXPECT_TRUE(near(message->findSignal("Enable")->decode(payload, 8), 1.0));

    EXPECT_TRUE(message->findSignal("Enable")->nameForValue(1) == "Enabled");
}

TEST(DbcParserTests, Bit31OfTheIdentifierIsTheFrameFormatNotTheIdentifier)
{
    // 2566840064 is 0x98FEDF00: the extended flag in bit 31, plus the J1939
    // identifier 0x18FEDF00. Read as a plain identifier it is larger than any
    // CAN identifier can be, so a parser that does not split the flag out
    // produces a message no frame can ever match - and matches nothing,
    // silently.
    const CanDatabase database = parsed(R"(
BO_ 2566840064 EEC1: 8 ECU
 SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] "rpm" Vector__XXX
BO_ 256 Standard: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
)");

    const CanMessage* extended = database.find(0x18FEDF00, CanFrameFormat::Extended);
    ASSERT_TRUE(extended != nullptr);
    EXPECT_TRUE(extended->name == "EEC1");

    // And the format is part of the key, not decoration: the same number in the
    // other format is a different message.
    EXPECT_TRUE(database.find(0x18FEDF00, CanFrameFormat::Standard) == nullptr);
    EXPECT_TRUE(database.find(0x100, CanFrameFormat::Standard) != nullptr);
    EXPECT_TRUE(database.find(0x100, CanFrameFormat::Extended) == nullptr);
}

TEST(DbcParserTests, MultiplexedSignalsAreReportedOnlyForTheFramesThatCarryThem)
{
    const CanDatabase database = parsed(R"(
BO_ 100 Multiplexed: 8 ECU
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ WhenZero m0 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ WhenOne m1 : 8|16@1+ (0.5,0) [0|32767] "" Vector__XXX
 SG_ Always : 24|8@1+ (1,0) [0|255] "" Vector__XXX
)");

    const CanMessage* message = database.findByName("Multiplexed");
    ASSERT_TRUE(message != nullptr);
    ASSERT_TRUE(message->multiplexerSwitch() != nullptr);
    EXPECT_TRUE(message->multiplexerSwitch()->name == "Selector");

    // Selector = 0. WhenZero rides along; WhenOne is not in this frame at all,
    // and the same bits mean something else. Bytes 1 and 2 are 34 12, which
    // Intel order reads as 0x1234 - least significant byte first, which is the
    // step this test got wrong when it was written.
    const std::uint8_t zero[8] = {0x00, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    const std::vector<const CanSignal*> inZero = message->signalsIn(zero, 8);
    ASSERT_TRUE(inZero.size() == 3);
    EXPECT_TRUE(inZero[0]->name == "Selector");
    EXPECT_TRUE(inZero[1]->name == "WhenZero");
    EXPECT_TRUE(inZero[2]->name == "Always");
    EXPECT_TRUE(near(inZero[1]->decode(zero, 8), 4660.0));

    const std::uint8_t one[8] = {0x01, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    const std::vector<const CanSignal*> inOne = message->signalsIn(one, 8);
    ASSERT_TRUE(inOne.size() == 3);
    EXPECT_TRUE(inOne[1]->name == "WhenOne");
    // Same bits, half the value: the whole point of a multiplexer.
    EXPECT_TRUE(near(inOne[1]->decode(one, 8), 2330.0));

    // A selector value no signal is declared for carries only the plain ones.
    // Not an error: a database rarely describes every value a switch can hold.
    const std::uint8_t unknown[8] = {0x07, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    EXPECT_TRUE(message->signalsIn(unknown, 8).size() == 2);
}

TEST(DbcParserTests, BlockCommentsThatAreNotPartOfTheFormatAreTolerated)
{
    // Not DBC syntax. Both hand-written databases this project is tested
    // against are full of them, and every other tool reads those files - so a
    // parser that rejects them is the one that is wrong.
    const CanDatabase database = parsed(R"(
/* -----------------------------------------
   Speed, in the shape a human wrote it
   ----------------------------------------- */
BO_ 257 VehicleSpeed: 8 ECU
 SG_ SpeedKmh : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECM
)");

    ASSERT_TRUE(database.messageCount() == 1);
    EXPECT_TRUE(database.findByName("VehicleSpeed") != nullptr);
}

TEST(DbcParserTests, ASectionThisParserDoesNotKnowIsSkippedNotRejected)
{
    // The format grows, and files are passed between tools. Refusing a file
    // because of a section nothing downstream reads would make this parser
    // useless for exactly the databases that come from somewhere else.
    const CanDatabase database = parsed(R"(
BO_ 256 Known: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
SIG_GROUP_ 256 Group1 1 : Value;
BA_DEF_ SG_ "SigType" ENUM "Default","Range";
EV_ Brightness: 0 [0|100] "%" 50 1 DUMMY_NODE_VECTOR0 Vector__XXX;
)");

    EXPECT_TRUE(database.messageCount() == 1);
    EXPECT_TRUE(database.findByName("Known") != nullptr);
}

TEST(DbcParserTests, AMalformedSignalFailsTheLoadAndNamesTheLine)
{
    // The other half of the rule above. An unknown section is skipped because
    // nothing reads it; a broken SG_ line is not, because a signal that
    // silently goes missing is a panel that shows nothing with no explanation.
    CanDatabase database;
    const Result result = DbcParser::parse(R"(
BO_ 256 Broken: 8 ECU
 SG_ Value : 0|8@2+ (1,0) [0|255] "" Vector__XXX
)",
                                           database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::ParseError);

    const std::string message{result.message()};
    EXPECT_TRUE(message.find("line 3") != std::string::npos);
    EXPECT_TRUE(message.find("@0") != std::string::npos);
}

TEST(DbcParserTests, AFailedParseLeavesThePreviousDatabaseIntact)
{
    // Reloading a .dbc that someone is editing is exactly when a parse fails,
    // and losing the database that was working would turn a typo into a
    // restart.
    CanDatabase database = parsed(kMotohawk);
    ASSERT_TRUE(database.messageCount() == 1);

    const Result result = DbcParser::parse("BO_ 256 Broken: 8 ECU\n SG_ x : bad\n", database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(database.messageCount() == 1);
    EXPECT_TRUE(database.findByName("ExampleMessage") != nullptr);
}

TEST(DbcParserTests, ACommentSpanningSeveralLinesDoesNotSwallowTheFile)
{
    const CanDatabase database = parsed(R"(
BO_ 256 Known: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
CM_ BO_ 256 "First line.
Second line.";
BO_ 257 After: 8 ECU
 SG_ Other : 0|8@1+ (1,0) [0|255] "" Vector__XXX
)");

    ASSERT_TRUE(database.messageCount() == 2);
    EXPECT_TRUE(database.findByName("Known")->comment == "First line.\nSecond line.");
    EXPECT_TRUE(database.findByName("After") != nullptr);
}

TEST(DbcParserTests, GenMsgCycleTimeIsReadWhereTheFileStatesOne)
{
    const CanDatabase database = parsed(R"(
BO_ 256 Periodic: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 257 Sporadic: 8 ECU
 SG_ Other : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 256 100;
)");

    EXPECT_TRUE(database.findByName("Periodic")->cycleTimeMs == 100);

    // Zero means "the file does not say", not "sent once". Plenty of periodic
    // messages carry no attribute.
    EXPECT_TRUE(database.findByName("Sporadic")->cycleTimeMs == 0);
}

TEST(DbcParserTests, ASecondDefinitionOfTheSameMessageReplacesTheFirst)
{
    // Merged databases contain duplicates. The last definition is the one the
    // tools that produced the file act on.
    const CanDatabase database = parsed(R"(
BO_ 256 Old: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 256 New: 8 ECU
 SG_ Value : 0|16@1+ (1,0) [0|65535] "" Vector__XXX
)");

    ASSERT_TRUE(database.messageCount() == 1);
    EXPECT_TRUE(database.find(0x100, CanFrameFormat::Standard)->name == "New");
    EXPECT_TRUE(database.find(0x100, CanFrameFormat::Standard)->signalList[0].bitLength == 16);
}

TEST(DbcParserTests, AMissingFileIsReportedAsMissingNotAsAParseError)
{
    CanDatabase database;
    const Result result = DbcParser::parseFile("no-such-database.dbc", database);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::FileNotFound);
}

TEST(DbcParserTests, ParseFileRecordsWhereTheDatabaseCameFrom)
{
    // The path is what an error message from a running measurement has to
    // name: "signal not found" is not actionable when three databases are
    // loaded.
    const std::string path = "torquebus-dbc-test.dbc";
    {
        std::ofstream file{path};
        file << kMotohawk;
    }

    CanDatabase database;
    const Result result = DbcParser::parseFile(path, database);
    std::remove(path.c_str());

    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(database.sourcePath == path);
    EXPECT_TRUE(database.messageCount() == 1);
}
