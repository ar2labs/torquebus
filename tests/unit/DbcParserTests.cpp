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

#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(result.succeeded());
    return database;
}

[[nodiscard]] bool near(double actual, double expected)
{
    return (actual - expected) < 1e-9 && (expected - actual) < 1e-9;
}

} // namespace

TEST_CASE("The keyword list in NS_ is not a set of sections", "[dbc][parser]")
{
    // A file that declares it may use CM_ contains a line that is exactly
    // `CM_`. Reading that as a comment section is how this parser failed on
    // every real database the first time it ran - all three fixtures, at line
    // 7, with an error about a missing quote.
    const CanDatabase database = parsed(kMotohawk);

    CHECK(database.messageCount() == 1);
    CHECK(database.version == "1.0");
}

TEST_CASE("A message and its signals come back as the file describes them", "[dbc][parser]")
{
    const CanDatabase database = parsed(kMotohawk);

    const CanMessage* message = database.find(0x1F0, CanFrameFormat::Standard);
    REQUIRE(message != nullptr);

    CHECK(message->name == "ExampleMessage");
    CHECK(message->length == 8);
    CHECK(message->transmitter == "PCM1");
    CHECK(message->signalList.size() == 3);
    CHECK(message->comment == "Example message used as template in MotoHawk models.");

    const CanSignal* temperature = message->findSignal("Temperature");
    REQUIRE(temperature != nullptr);
    CHECK(temperature->startBit == 0);
    CHECK(temperature->bitLength == 12);
    CHECK(temperature->byteOrder == ByteOrder::Motorola);
    CHECK(temperature->isSigned);
    CHECK(near(temperature->factor, 0.01));
    CHECK(near(temperature->offset, 250.0));
    CHECK(temperature->unit == "degK");

    // Vector__XXX means "unspecified", so it is dropped rather than recorded as
    // a node named Vector__XXX.
    CHECK(temperature->receivers == std::vector<std::string>{"PCM1", "FOO"});
    CHECK(message->findSignal("AverageRadius")->receivers.empty());
}

TEST_CASE("A parsed database decodes the vector the file was published with", "[dbc][parser]")
{
    // The end-to-end check: file text in, physical values out. Every part of
    // the parse has to be right for this to pass, which is why it is worth
    // having on top of the field-by-field assertions above.
    const CanDatabase database = parsed(kMotohawk);
    const std::uint8_t payload[8] = {0xC0, 0x06, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};

    const CanMessage* message = database.find(0x1F0, CanFrameFormat::Standard);
    REQUIRE(message != nullptr);

    CHECK(near(message->findSignal("Temperature")->decode(payload, 8), 250.55));
    CHECK(near(message->findSignal("AverageRadius")->decode(payload, 8), 3.2));
    CHECK(near(message->findSignal("Enable")->decode(payload, 8), 1.0));

    CHECK(message->findSignal("Enable")->nameForValue(1) == "Enabled");
}

TEST_CASE("Bit 31 of the identifier is the frame format, not the identifier", "[dbc][parser]")
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
    REQUIRE(extended != nullptr);
    CHECK(extended->name == "EEC1");

    // And the format is part of the key, not decoration: the same number in the
    // other format is a different message.
    CHECK(database.find(0x18FEDF00, CanFrameFormat::Standard) == nullptr);
    CHECK(database.find(0x100, CanFrameFormat::Standard) != nullptr);
    CHECK(database.find(0x100, CanFrameFormat::Extended) == nullptr);
}

TEST_CASE("Multiplexed signals are reported only for the frames that carry them", "[dbc][parser]")
{
    const CanDatabase database = parsed(R"(
BO_ 100 Multiplexed: 8 ECU
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ WhenZero m0 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ WhenOne m1 : 8|16@1+ (0.5,0) [0|32767] "" Vector__XXX
 SG_ Always : 24|8@1+ (1,0) [0|255] "" Vector__XXX
)");

    const CanMessage* message = database.findByName("Multiplexed");
    REQUIRE(message != nullptr);
    REQUIRE(message->multiplexerSwitch() != nullptr);
    CHECK(message->multiplexerSwitch()->name == "Selector");

    // Selector = 0. WhenZero rides along; WhenOne is not in this frame at all,
    // and the same bits mean something else. Bytes 1 and 2 are 34 12, which
    // Intel order reads as 0x1234 - least significant byte first, which is the
    // step this test got wrong when it was written.
    const std::uint8_t zero[8] = {0x00, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    const std::vector<const CanSignal*> inZero = message->signalsIn(zero, 8);
    REQUIRE(inZero.size() == 3);
    CHECK(inZero[0]->name == "Selector");
    CHECK(inZero[1]->name == "WhenZero");
    CHECK(inZero[2]->name == "Always");
    CHECK(near(inZero[1]->decode(zero, 8), 4660.0));

    const std::uint8_t one[8] = {0x01, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    const std::vector<const CanSignal*> inOne = message->signalsIn(one, 8);
    REQUIRE(inOne.size() == 3);
    CHECK(inOne[1]->name == "WhenOne");
    // Same bits, half the value: the whole point of a multiplexer.
    CHECK(near(inOne[1]->decode(one, 8), 2330.0));

    // A selector value no signal is declared for carries only the plain ones.
    // Not an error: a database rarely describes every value a switch can hold.
    const std::uint8_t unknown[8] = {0x07, 0x34, 0x12, 0x2A, 0, 0, 0, 0};
    CHECK(message->signalsIn(unknown, 8).size() == 2);
}

TEST_CASE("Block comments that are not part of the format are tolerated", "[dbc][parser]")
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

    REQUIRE(database.messageCount() == 1);
    CHECK(database.findByName("VehicleSpeed") != nullptr);
}

TEST_CASE("A section this parser does not know is skipped, not rejected", "[dbc][parser]")
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

    CHECK(database.messageCount() == 1);
    CHECK(database.findByName("Known") != nullptr);
}

TEST_CASE("A malformed signal fails the load and names the line", "[dbc][parser]")
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

    REQUIRE(result.failed());
    CHECK(result.code() == ErrorCode::ParseError);

    const std::string message{result.message()};
    CHECK(message.find("line 3") != std::string::npos);
    CHECK(message.find("@0") != std::string::npos);
}

TEST_CASE("A failed parse leaves the previous database intact", "[dbc][parser]")
{
    // Reloading a .dbc that someone is editing is exactly when a parse fails,
    // and losing the database that was working would turn a typo into a
    // restart.
    CanDatabase database = parsed(kMotohawk);
    REQUIRE(database.messageCount() == 1);

    const Result result = DbcParser::parse("BO_ 256 Broken: 8 ECU\n SG_ x : bad\n", database);

    REQUIRE(result.failed());
    CHECK(database.messageCount() == 1);
    CHECK(database.findByName("ExampleMessage") != nullptr);
}

TEST_CASE("A comment spanning several lines does not swallow the file", "[dbc][parser]")
{
    const CanDatabase database = parsed(R"(
BO_ 256 Known: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
CM_ BO_ 256 "First line.
Second line.";
BO_ 257 After: 8 ECU
 SG_ Other : 0|8@1+ (1,0) [0|255] "" Vector__XXX
)");

    REQUIRE(database.messageCount() == 2);
    CHECK(database.findByName("Known")->comment == "First line.\nSecond line.");
    CHECK(database.findByName("After") != nullptr);
}

TEST_CASE("GenMsgCycleTime is read where the file states one", "[dbc][parser]")
{
    const CanDatabase database = parsed(R"(
BO_ 256 Periodic: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 257 Sporadic: 8 ECU
 SG_ Other : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 256 100;
)");

    CHECK(database.findByName("Periodic")->cycleTimeMs == 100);

    // Zero means "the file does not say", not "sent once". Plenty of periodic
    // messages carry no attribute.
    CHECK(database.findByName("Sporadic")->cycleTimeMs == 0);
}

TEST_CASE("A second definition of the same message replaces the first", "[dbc][parser]")
{
    // Merged databases contain duplicates. The last definition is the one the
    // tools that produced the file act on.
    const CanDatabase database = parsed(R"(
BO_ 256 Old: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 256 New: 8 ECU
 SG_ Value : 0|16@1+ (1,0) [0|65535] "" Vector__XXX
)");

    REQUIRE(database.messageCount() == 1);
    CHECK(database.find(0x100, CanFrameFormat::Standard)->name == "New");
    CHECK(database.find(0x100, CanFrameFormat::Standard)->signalList[0].bitLength == 16);
}

TEST_CASE("A missing file is reported as missing, not as a parse error", "[dbc][parser]")
{
    CanDatabase database;
    const Result result = DbcParser::parseFile("no-such-database.dbc", database);

    REQUIRE(result.failed());
    CHECK(result.code() == ErrorCode::FileNotFound);
}

TEST_CASE("parseFile records where the database came from", "[dbc][parser]")
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

    REQUIRE(result.succeeded());
    CHECK(database.sourcePath == path);
    CHECK(database.messageCount() == 1);
}
