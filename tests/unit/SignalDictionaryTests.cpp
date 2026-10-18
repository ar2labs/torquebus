// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the loaded databases tell the Dashboard, and what it does with that.
//
// Most of these are about rules, and a rule is only worth having if it can be read off a test:
// which widget a signal wants, what an unnamed typo is taken for, what ranks above what when a
// person types three letters into a search box. Each is pinned with a case small enough to see why,
// and the last group runs every signal of the databases that ship through the whole path, because
// the way a suggestion fails is quietly - a widget that arrives, validates, and means the wrong
// thing.

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace torquebus;

namespace {

/// A small database of the shapes the rules care about: a scaled engine speed with a comment and a
/// cycle time, a percentage, a temperature with an offset, a two-state lamp whose names say
/// nothing, a state with five names, a counter, a door whose names say something, a gear whose
/// table is in raw numbers that are not the physical ones, and a signal with a unit and no declared
/// range.
constexpr std::string_view kDatabase = R"(VERSION "test"

BU_: EMS BCM

BO_ 2364539904 EEC1: 8 EMS
 SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] "rpm" Vector__XXX
 SG_ DriverDemandTorque : 8|8@1+ (1,-125) [-125|125] "%" Vector__XXX

BO_ 2566843904 ET1: 8 EMS
 SG_ EngineCoolantTemperature : 0|8@1+ (1,-40) [-40|210] "degC" Vector__XXX

BO_ 2566844160 EFL_P1: 8 EMS
 SG_ EngineOilLevel : 16|8@1+ (0.4,0) [0|100] "%" Vector__XXX
 SG_ EngineOilPressure : 24|8@1+ (4,0) [0|1000] "kPa" Vector__XXX

BO_ 2566834688 DM1: 8 EMS
 SG_ AmberWarningLamp : 2|2@1+ (1,0) [0|3] "" Vector__XXX
 SG_ SPNLow : 16|16@1+ (1,0) [0|65535] "" Vector__XXX

BO_ 2566848640 Model: 8 BCM
 SG_ RegimeClass : 29|3@1+ (1,0) [0|4] "class" Vector__XXX
 SG_ TotalHours : 0|32@1+ (0.05,0) [0|210554060] "hr" Vector__XXX

BO_ 300 Doors: 8 BCM
 SG_ DoorState : 0|1@1+ (1,0) [0|1] "" Vector__XXX
 SG_ Flag : 8|1@1+ (1,0) [0|1] "" Vector__XXX
 SG_ DoorLatch : 16|2@1+ (1,0) [0|1] "" Vector__XXX

BO_ 301 Drive: 8 BCM
 SG_ Gear : 0|8@1+ (1,-125) [-125|125] "" Vector__XXX
 SG_ Mystery : 8|8@1+ (1,0) [0|0] "V" Vector__XXX

BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 2364539904 20;
BA_ "GenMsgCycleTime" BO_ 2566844160 500;

CM_ SG_ 2364539904 EngineSpeed "Crankshaft speed";
VAL_ 2566834688 AmberWarningLamp 0 "Off" 1 "On" ;
VAL_ 2566848640 RegimeClass 0 "Idle" 1 "Cruise" 2 "HighLoad" 3 "ThermalStress" 4 "Anomaly" ;
VAL_ 300 DoorState 0 "Closed" 1 "Open" ;
VAL_ 301 Gear 125 "Neutral" 126 "First" 124 "Reverse" ;
)";

[[nodiscard]] std::shared_ptr<const CanDatabase>
parsedDatabase(std::string_view text, const char* path = "C:/dbc/test.dbc")
{
    auto database = std::make_shared<CanDatabase>();

    const Result result = DbcParser::parse(text, *database);
    EXPECT_TRUE(result.succeeded()) << std::string{result.message()};

    database->sourcePath = path;
    return database;
}

[[nodiscard]] SignalDictionary dictionaryOf(std::string_view text = kDatabase)
{
    const std::vector<std::shared_ptr<const CanDatabase>> databases{parsedDatabase(text)};
    return SignalDictionary{databases};
}

/// The names of the entries a search returned, in the order it returned them.
[[nodiscard]] std::vector<std::string> namesOf(const std::vector<const DictionaryEntry*>& found)
{
    std::vector<std::string> names;

    for (const DictionaryEntry* entry : found) {
        names.push_back(entry->qualified());
    }

    return names;
}

[[nodiscard]] SignalDictionary shippedJ1939()
{
    auto database = std::make_shared<CanDatabase>();

    const Result result =
        DbcParser::parseFile(std::string{TORQUEBUS_EXAMPLE_DATABASE_DIR} + "/j1939.dbc", *database);
    EXPECT_TRUE(result.succeeded()) << std::string{result.message()};

    const std::vector<std::shared_ptr<const CanDatabase>> databases{database};
    return SignalDictionary{databases};
}

} // namespace

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, EverySignalOfTheLoadedDatabasesIsListedWithWhatTheDatabaseSaysOfIt)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(dictionary.size(), 14U);

    const DictionaryEntry* speed = dictionary.find("EEC1", "EngineSpeed");
    ASSERT_NE(speed, nullptr);

    EXPECT_EQ(speed->qualified(), "EEC1.EngineSpeed");
    EXPECT_EQ(speed->unit, "rpm");
    EXPECT_EQ(speed->comment, "Crankshaft speed");
    EXPECT_EQ(speed->transmitter, "EMS");
    EXPECT_EQ(speed->cycleTimeMs, 20U);
    EXPECT_EQ(speed->database, "test.dbc");
    EXPECT_EQ(speed->bitLength, 16);
    EXPECT_TRUE(speed->declaredRange);
    EXPECT_DOUBLE_EQ(speed->minimum, 0.0);
    EXPECT_DOUBLE_EQ(speed->maximum, 8031.875);

    // Not stated, so not claimed.
    const DictionaryEntry* torque = dictionary.find("EEC1", "DriverDemandTorque");
    ASSERT_NE(torque, nullptr);
    EXPECT_EQ(torque->cycleTimeMs, 20U);
    EXPECT_EQ(dictionary.find("ET1", "EngineCoolantTemperature")->cycleTimeMs, 0U);
}

TEST(SignalDictionaryTests, ARangeNobodyWroteIsWorkedOutFromTheBitsAndSaysSo)
{
    const SignalDictionary dictionary = dictionaryOf();

    // [0|0] is what a database says when it has nothing to say, not that the signal is zero.
    const DictionaryEntry* mystery = dictionary.find("Drive", "Mystery");
    ASSERT_NE(mystery, nullptr);

    EXPECT_FALSE(mystery->declaredRange);
    EXPECT_DOUBLE_EQ(mystery->minimum, 0.0);
    EXPECT_DOUBLE_EQ(mystery->maximum, 255.0);

    // With the signal's own arithmetic: 8 bits, factor 1, offset -125.
    const DictionaryEntry* gear = dictionary.find("Drive", "Gear");
    ASSERT_NE(gear, nullptr);
    EXPECT_TRUE(gear->declaredRange);
    EXPECT_DOUBLE_EQ(gear->minimum, -125.0);
    EXPECT_DOUBLE_EQ(gear->maximum, 125.0);
}

TEST(SignalDictionaryTests, ALookupIsExactBecauseThePlotStoreIsExact)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_NE(dictionary.find("EEC1", "EngineSpeed"), nullptr);
    EXPECT_EQ(dictionary.find("eec1", "EngineSpeed"), nullptr);
    EXPECT_EQ(dictionary.find("EEC1", "enginespeed"), nullptr);
    EXPECT_EQ(dictionary.find("ET1", "EngineSpeed"), nullptr);

    EXPECT_TRUE(dictionary.hasMessage("EEC1"));
    EXPECT_FALSE(dictionary.hasMessage("Eec1"));
    EXPECT_FALSE(dictionary.hasMessage(""));
}

TEST(SignalDictionaryTests, AMessageListsItsSignalsInTheDatabasesOrderAndMessagesAreSorted)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.signalsOf("EFL_P1")),
              (std::vector<std::string>{"EFL_P1.EngineOilLevel", "EFL_P1.EngineOilPressure"}));
    EXPECT_TRUE(dictionary.signalsOf("Nothing").empty());

    EXPECT_EQ(
        dictionary.messageNames(),
        (std::vector<std::string>{"DM1", "Doors", "Drive", "EEC1", "EFL_P1", "ET1", "Model"}));
}

TEST(SignalDictionaryTests, AnEmptyDictionaryListsNothingAndFindsNothing)
{
    const SignalDictionary dictionary;

    EXPECT_TRUE(dictionary.empty());
    EXPECT_EQ(dictionary.find("EEC1", "EngineSpeed"), nullptr);
    EXPECT_TRUE(dictionary.search("speed").empty());
    EXPECT_TRUE(dictionary.messageNames().empty());
}

TEST(SignalDictionaryTests, ADatabaseThatIsNullIsSkippedAndTwoAreBothListed)
{
    constexpr std::string_view second = R"(
BO_ 400 Extra: 8 ECU
 SG_ Boost : 0|8@1+ (1,0) [0|255] "kPa" Vector__XXX
)";

    const std::vector<std::shared_ptr<const CanDatabase>> databases{
        nullptr, parsedDatabase(kDatabase), parsedDatabase(second, "C:/other/extra.dbc")};

    const SignalDictionary dictionary{databases};

    EXPECT_EQ(dictionary.size(), 15U);
    ASSERT_NE(dictionary.find("Extra", "Boost"), nullptr);
    EXPECT_EQ(dictionary.find("Extra", "Boost")->database, "extra.dbc");
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, ANameRanksAboveAMessageAboveACommentAndTiesKeepTheDatabasesOrder)
{
    constexpr std::string_view text = R"(
BO_ 500 Speed: 8 ECU
 SG_ Torque : 0|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 501 Other: 8 ECU
 SG_ Actual : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ SpeedLimit : 8|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ Speed : 16|8@1+ (1,0) [0|255] "" Vector__XXX
CM_ SG_ 501 Actual "the speed of the thing";
)";

    const SignalDictionary dictionary = dictionaryOf(text);

    // Exactly the name, then the start of the name, then the start of the message, then a comment.
    EXPECT_EQ(namesOf(dictionary.search("speed")),
              (std::vector<std::string>{
                  "Other.Speed", "Other.SpeedLimit", "Speed.Torque", "Other.Actual"}));
}

TEST(SignalDictionaryTests, AWordMatchesTheStartOfAWordInsideAName)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.search("coolant")),
              (std::vector<std::string>{"ET1.EngineCoolantTemperature"}));
    EXPECT_EQ(namesOf(dictionary.search("temp")),
              (std::vector<std::string>{"ET1.EngineCoolantTemperature"}));

    // In the middle of a word is a weaker match, but it is a match.
    EXPECT_EQ(namesOf(dictionary.search("olant")),
              (std::vector<std::string>{"ET1.EngineCoolantTemperature"}));
}

TEST(SignalDictionaryTests, TheInitialsOfAWordedNameFindIt)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.search("ect")),
              (std::vector<std::string>{"ET1.EngineCoolantTemperature"}));
    EXPECT_EQ(namesOf(dictionary.search("eop")),
              (std::vector<std::string>{"EFL_P1.EngineOilPressure"}));
}

TEST(SignalDictionaryTests, EveryWordHasToMatchAndTheMessageCountsAsPartOfTheName)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.search("eec1 torque")),
              (std::vector<std::string>{"EEC1.DriverDemandTorque"}));
    EXPECT_EQ(namesOf(dictionary.search("EEC1.eng")),
              (std::vector<std::string>{"EEC1.EngineSpeed"}));
    EXPECT_EQ(namesOf(dictionary.search("oil press")),
              (std::vector<std::string>{"EFL_P1.EngineOilPressure"}));
    EXPECT_TRUE(dictionary.search("oil speed").empty());
}

TEST(SignalDictionaryTests, ASearchFindsAUnitAndAComment)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.search("kpa")),
              (std::vector<std::string>{"EFL_P1.EngineOilPressure"}));
    EXPECT_EQ(namesOf(dictionary.search("crankshaft")),
              (std::vector<std::string>{"EEC1.EngineSpeed"}));
    EXPECT_TRUE(dictionary.search("zzz").empty());
}

TEST(SignalDictionaryTests, EmptyTextListsTheFirstSignalsAndTheLimitIsKept)
{
    const SignalDictionary dictionary = dictionaryOf();

    EXPECT_EQ(namesOf(dictionary.search("", 3)),
              (std::vector<std::string>{
                  "EEC1.EngineSpeed", "EEC1.DriverDemandTorque", "ET1.EngineCoolantTemperature"}));
    EXPECT_EQ(dictionary.search("").size(), 14U);
    EXPECT_EQ(dictionary.search("engine", 2).size(), 2U);
    EXPECT_TRUE(dictionary.search("engine", 0).empty());
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, AnIdentifierIsSplitIntoTheWordsAPersonReads)
{
    EXPECT_EQ(prettyName("EngineCoolantTemperature"), "Engine Coolant Temperature");

    // An acronym stays whole, and the word after it starts where the case turns back.
    EXPECT_EQ(prettyName("ABSAmberWarning"), "ABS Amber Warning");
    EXPECT_EQ(prettyName("SPNLow"), "SPN Low");
    EXPECT_EQ(prettyName("DEFLevelLowIndicator"), "DEF Level Low Indicator");

    // A digit belongs to the word before it; a capital after a digit starts one.
    EXPECT_EQ(prettyName("AccelPedalPos1"), "Accel Pedal Pos1");
    EXPECT_EQ(prettyName("EngineIntake1AirTemperature"), "Engine Intake1 Air Temperature");

    EXPECT_EQ(prettyName("engine_speed"), "engine speed");
    EXPECT_EQ(prettyName("RPM"), "RPM");
    EXPECT_EQ(prettyName("TinyML_Proprietary"), "Tiny ML Proprietary");

    // Nothing to split is returned as it was.
    EXPECT_EQ(prettyName(""), "");
    EXPECT_EQ(prettyName("__"), "__");
}

TEST(SignalDictionaryTests, AWidgetIdIsLowercaseWordsAndNeverEmpty)
{
    EXPECT_EQ(idFor("EngineSpeed"), "engine_speed");
    EXPECT_EQ(idFor("ABSAmberWarning"), "abs_amber_warning");
    EXPECT_EQ(idFor("EEC1"), "eec1");
    EXPECT_EQ(idFor("TinyML_Proprietary"), "tiny_ml_proprietary");

    EXPECT_EQ(idFor(""), "signal");
    EXPECT_EQ(idFor("$$$"), "signal");
}

TEST(SignalDictionaryTests, AWordForAValueSuggestsHowItShouldBeTinted)
{
    EXPECT_EQ(inferTone("Error"), DashboardTone::Danger);
    EXPECT_EQ(inferTone("Critical"), DashboardTone::Danger);
    EXPECT_EQ(inferTone("ActiveFault"), DashboardTone::Danger);

    EXPECT_EQ(inferTone("Warning"), DashboardTone::Warning);
    EXPECT_EQ(inferTone("HighLoad"), DashboardTone::Warning);
    EXPECT_EQ(inferTone("Anomaly"), DashboardTone::Warning);

    EXPECT_EQ(inferTone("Moderate"), DashboardTone::Info);

    EXPECT_EQ(inferTone("Running"), DashboardTone::Ok);
    EXPECT_EQ(inferTone("Engaged"), DashboardTone::Ok);

    EXPECT_EQ(inferTone("Off"), DashboardTone::Off);
    EXPECT_EQ(inferTone("Idle"), DashboardTone::Off);
    EXPECT_EQ(inferTone("Waiting"), DashboardTone::Off);

    // "Available" is not good news when it follows "Not".
    EXPECT_EQ(inferTone("NotAvailable"), DashboardTone::Off);
    EXPECT_EQ(inferTone("Not available"), DashboardTone::Off);
    EXPECT_EQ(inferTone("Unavailable"), DashboardTone::Off);

    // No opinion is the default, because a word that means different things on different signals is
    // not listed.
    EXPECT_EQ(inferTone("Reverse"), DashboardTone::Neutral);
    EXPECT_EQ(inferTone("Cruise"), DashboardTone::Neutral);
    EXPECT_EQ(inferTone("ThermalStress"), DashboardTone::Neutral);
    EXPECT_EQ(inferTone(""), DashboardTone::Neutral);
}

TEST(SignalDictionaryTests, DecimalsTellOneStepFromTheNextAndNoMore)
{
    EXPECT_EQ(decimalsFor(1.0), 0);
    EXPECT_EQ(decimalsFor(4.0), 0);
    EXPECT_EQ(decimalsFor(1000.0), 0);

    EXPECT_EQ(decimalsFor(0.9), 1);
    EXPECT_EQ(decimalsFor(0.5), 1);
    EXPECT_EQ(decimalsFor(0.4), 1);
    EXPECT_EQ(decimalsFor(0.125), 1);
    EXPECT_EQ(decimalsFor(0.1), 1);

    EXPECT_EQ(decimalsFor(0.05), 2);
    EXPECT_EQ(decimalsFor(0.03125), 2);

    // Nobody reads an engine speed to the third place.
    EXPECT_EQ(decimalsFor(0.00390625), 2);
    EXPECT_EQ(decimalsFor(0.001953125), 2);

    EXPECT_EQ(decimalsFor(-0.05), 2);
    EXPECT_EQ(decimalsFor(0.0), 0);
    EXPECT_EQ(decimalsFor(std::numeric_limits<double>::quiet_NaN()), 0);
    EXPECT_EQ(decimalsFor(std::numeric_limits<double>::infinity()), 0);
}

TEST(SignalDictionaryTests, ASignalIsStaleAfterThreeOfItsCyclesWithinReason)
{
    EXPECT_EQ(staleAfterMs(0), 0U);

    // Never under half a second: a 20 ms message is not stale at 60, it is a busy bus.
    EXPECT_EQ(staleAfterMs(1), 500U);
    EXPECT_EQ(staleAfterMs(20), 500U);
    EXPECT_EQ(staleAfterMs(166), 500U);
    EXPECT_EQ(staleAfterMs(167), 501U);
    EXPECT_EQ(staleAfterMs(200), 600U);
    EXPECT_EQ(staleAfterMs(1000), 3000U);

    // And never past a minute.
    EXPECT_EQ(staleAfterMs(20'000), 60'000U);
    EXPECT_EQ(staleAfterMs(100'000), 60'000U);
    EXPECT_EQ(staleAfterMs(std::numeric_limits<std::uint32_t>::max()), 60'000U);
}

// ---------------------------------------------------------------------------
// Which widget a signal wants
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, EachRuleForTheKindOfWidgetHasACaseThatPinsIt)
{
    const SignalDictionary dictionary = dictionaryOf();

    struct Case final {
        const char* message;
        const char* signal;
        DashboardWidgetKind kind;
        const char* why;
    };

    const Case cases[] = {
        {"DM1", "AmberWarningLamp", DashboardWidgetKind::Lamp, "a table of exactly 0 and 1"},
        {"Doors", "DoorState", DashboardWidgetKind::Lamp, "a table of exactly 0 and 1"},
        {"Model", "RegimeClass", DashboardWidgetKind::State, "any other table"},
        {"Drive", "Gear", DashboardWidgetKind::State, "any other table, in physical values"},
        {"Doors", "Flag", DashboardWidgetKind::Lamp, "one bit"},
        {"Doors", "DoorLatch", DashboardWidgetKind::Lamp, "two bits with no table"},
        {"EFL_P1", "EngineOilLevel", DashboardWidgetKind::Bar, "a percentage"},
        {"EEC1", "DriverDemandTorque", DashboardWidgetKind::Bar, "a percentage, whatever its name"},
        {"Model", "TotalHours", DashboardWidgetKind::Numeric, "a unit that counts time"},
        {"EEC1", "EngineSpeed", DashboardWidgetKind::Gauge, "a declared range, a unit, a span"},
        {"ET1", "EngineCoolantTemperature", DashboardWidgetKind::Gauge, "the same, with an offset"},
        {"EFL_P1", "EngineOilPressure", DashboardWidgetKind::Gauge, "the same"},
        {"DM1", "SPNLow", DashboardWidgetKind::Numeric, "no unit: a dial of nothing"},
        {"Drive", "Mystery", DashboardWidgetKind::Numeric, "a unit and a range nobody wrote"},
    };

    for (const Case& test : cases) {
        const DictionaryEntry* entry = dictionary.find(test.message, test.signal);

        SCOPED_TRACE(::testing::Message()
                     << test.message << "." << test.signal << ": " << test.why);
        ASSERT_NE(entry, nullptr);

        EXPECT_EQ(suggestKind(*entry), test.kind);
    }
}

TEST(SignalDictionaryTests, ATableOfOnlyZeroIsNotATwoStateLamp)
{
    constexpr std::string_view text = R"(
BO_ 600 Partial: 8 ECU
 SG_ Mode : 0|4@1+ (1,0) [0|15] "" Vector__XXX
 SG_ Pair : 4|4@1+ (1,0) [0|15] "" Vector__XXX
VAL_ 600 Mode 0 "Off" ;
VAL_ 600 Pair 1 "A" 2 "B" ;
)";

    const SignalDictionary dictionary = dictionaryOf(text);

    EXPECT_EQ(suggestKind(*dictionary.find("Partial", "Mode")), DashboardWidgetKind::State);
    EXPECT_EQ(suggestKind(*dictionary.find("Partial", "Pair")), DashboardWidgetKind::State);
}

// ---------------------------------------------------------------------------
// A widget made from a signal
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, AWidgetMadeFromASignalIsBoundAndConfiguredFromTheDatabase)
{
    const SignalDictionary dictionary = dictionaryOf();
    const DictionaryEntry* speed = dictionary.find("EEC1", "EngineSpeed");
    ASSERT_NE(speed, nullptr);

    const DashboardWidget widget = suggestWidget(*speed);

    EXPECT_EQ(widget.kind, DashboardWidgetKind::Gauge);
    EXPECT_EQ(widget.id, "engine_speed");
    EXPECT_EQ(widget.title, "Engine Speed");
    EXPECT_EQ(widget.unit, "rpm");
    EXPECT_EQ(widget.binding.source, DashboardBinding::Source::Signal);
    EXPECT_EQ(widget.binding.message, "EEC1");
    EXPECT_EQ(widget.binding.signal, "EngineSpeed");
    EXPECT_DOUBLE_EQ(widget.minimum, 0.0);
    EXPECT_DOUBLE_EQ(widget.maximum, 8031.875);
    EXPECT_EQ(widget.decimals, 1);

    // A message every 20 ms is called silent at half a second, not at 60 ms.
    EXPECT_EQ(widget.staleMs, 500U);

    // Starts at the size a Gauge from the menu starts at.
    EXPECT_DOUBLE_EQ(widget.width, defaultSizeOf(DashboardWidgetKind::Gauge).width);

    DashboardDescription dashboard;
    dashboard.add(widget);
    SCOPED_TRACE(::testing::Message() << std::string{dashboard.validate().message()});
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(SignalDictionaryTests, ASignalWithNamesBecomesAStateWithThemInPhysicalValuesAndTones)
{
    const SignalDictionary dictionary = dictionaryOf();

    const DashboardWidget regime = suggestWidget(*dictionary.find("Model", "RegimeClass"));

    EXPECT_EQ(regime.kind, DashboardWidgetKind::State);
    ASSERT_EQ(regime.states.size(), 5U);
    EXPECT_EQ(regime.states[0].label, "Idle");
    EXPECT_EQ(regime.states[0].tone, DashboardTone::Off);
    EXPECT_EQ(regime.states[1].label, "Cruise");
    EXPECT_EQ(regime.states[1].tone, DashboardTone::Neutral);
    EXPECT_EQ(regime.states[2].label, "HighLoad");
    EXPECT_EQ(regime.states[2].tone, DashboardTone::Warning);
    EXPECT_EQ(regime.states[4].label, "Anomaly");
    EXPECT_EQ(regime.states[4].tone, DashboardTone::Warning);

    // The table holds raw numbers; the widget reads physical ones. Raw 125 is gear 0 here.
    const DashboardWidget gear = suggestWidget(*dictionary.find("Drive", "Gear"));

    ASSERT_EQ(gear.states.size(), 3U);
    EXPECT_DOUBLE_EQ(gear.states[0].value, -1.0);
    EXPECT_EQ(gear.states[0].label, "Reverse");
    EXPECT_DOUBLE_EQ(gear.states[1].value, 0.0);
    EXPECT_EQ(gear.states[1].label, "Neutral");
    EXPECT_DOUBLE_EQ(gear.states[2].value, 1.0);
    EXPECT_EQ(gear.states[2].label, "First");

    ASSERT_NE(gear.stateOf(0.0), nullptr);
    EXPECT_EQ(gear.stateOf(0.0)->label, "Neutral");
    EXPECT_EQ(gear.stateOf(7.0), nullptr);

    DashboardDescription dashboard;
    dashboard.add(regime);
    dashboard.add(gear);
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(SignalDictionaryTests, ALampIsToldNothingByNamesThatSayOffAndOnButSomethingByOtherNames)
{
    const SignalDictionary dictionary = dictionaryOf();

    // "Off" and "On" under a bulb that is lit or dark says nothing the bulb does not.
    const DashboardWidget amber = suggestWidget(*dictionary.find("DM1", "AmberWarningLamp"));
    EXPECT_EQ(amber.kind, DashboardWidgetKind::Lamp);
    EXPECT_TRUE(amber.states.empty());

    // "Closed" and "Open" do.
    const DashboardWidget door = suggestWidget(*dictionary.find("Doors", "DoorState"));
    EXPECT_EQ(door.kind, DashboardWidgetKind::Lamp);
    ASSERT_EQ(door.states.size(), 2U);
    EXPECT_EQ(door.states[0].label, "Closed");
    EXPECT_EQ(door.states[1].label, "Open");
}

TEST(SignalDictionaryTests, APersonCanAskForAnotherKindButNotForOneThatCannotShowASignal)
{
    const SignalDictionary dictionary = dictionaryOf();
    const DictionaryEntry* speed = dictionary.find("EEC1", "EngineSpeed");
    ASSERT_NE(speed, nullptr);

    const DashboardWidget bar = suggestWidget(*speed, DashboardWidgetKind::Bar);
    EXPECT_EQ(bar.kind, DashboardWidgetKind::Bar);
    EXPECT_DOUBLE_EQ(bar.maximum, 8031.875);

    const DashboardWidget trend = suggestWidget(*speed, DashboardWidgetKind::Trend);
    EXPECT_EQ(trend.kind, DashboardWidgetKind::Trend);
    EXPECT_EQ(trend.unit, "rpm");

    // A slider writes, and a signal cannot be written: the suggestion stands.
    EXPECT_EQ(suggestWidget(*speed, DashboardWidgetKind::Slider).kind, DashboardWidgetKind::Gauge);
    EXPECT_EQ(suggestWidget(*speed, DashboardWidgetKind::Cluster).kind, DashboardWidgetKind::Gauge);
    EXPECT_EQ(suggestWidget(*speed, DashboardWidgetKind::Label).kind, DashboardWidgetKind::Gauge);

    // An image bound to a signal keeps the names, so that each can be given a picture.
    const DashboardWidget image =
        suggestWidget(*dictionary.find("Model", "RegimeClass"), DashboardWidgetKind::Image);
    EXPECT_EQ(image.kind, DashboardWidgetKind::Image);
    EXPECT_EQ(image.states.size(), 5U);
    EXPECT_TRUE(image.image.empty());
}

TEST(SignalDictionaryTests, ARangeThatIsEmptyIsGivenOneRatherThanLeftToBeRefused)
{
    // A signal whose range collapses to a point - a factor of zero is how, and the shipped-database
    // tests catch that - is still one a widget can be made from: validate() would refuse a needle
    // that cannot sweep, and a widget that arrives refused is a bad welcome.
    DictionaryEntry entry;
    entry.message = "Odd";
    entry.signal = "Point";
    entry.minimum = 3.0;
    entry.maximum = 3.0;

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Gauge);
    widget.id = "point";
    applyEntry(widget, entry);

    EXPECT_DOUBLE_EQ(widget.minimum, 3.0);
    EXPECT_DOUBLE_EQ(widget.maximum, 4.0);

    DashboardDescription dashboard;
    dashboard.add(widget);
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(SignalDictionaryTests, BindingAWidgetToAnotherSignalKeepsATitleSomebodyTyped)
{
    const SignalDictionary dictionary = dictionaryOf();
    const DictionaryEntry* speed = dictionary.find("EEC1", "EngineSpeed");
    const DictionaryEntry* oil = dictionary.find("EFL_P1", "EngineOilPressure");
    ASSERT_NE(speed, nullptr);
    ASSERT_NE(oil, nullptr);

    // Made from the first signal, so its title is the one that signal gave it: free to be replaced.
    DashboardWidget widget = suggestWidget(*speed);
    ASSERT_EQ(widget.title, "Engine Speed");

    applyEntry(widget, *oil);
    EXPECT_EQ(widget.title, "Engine Oil Pressure");
    EXPECT_EQ(widget.binding.signal, "EngineOilPressure");
    EXPECT_EQ(widget.unit, "kPa");
    EXPECT_DOUBLE_EQ(widget.maximum, 1000.0);

    // A title typed by a person is theirs.
    widget.title = "Oil";
    applyEntry(widget, *speed);
    EXPECT_EQ(widget.title, "Oil");
    EXPECT_EQ(widget.unit, "rpm");

    // And the things that are not about the signal are not touched.
    widget.width = 333.0;
    widget.image = "pictures/engine.png";
    widget.limits.warningAbove = 6000.0;
    applyEntry(widget, *oil);
    EXPECT_DOUBLE_EQ(widget.width, 333.0);
    EXPECT_EQ(widget.image, "pictures/engine.png");
    ASSERT_TRUE(widget.limits.warningAbove.has_value());
    EXPECT_DOUBLE_EQ(*widget.limits.warningAbove, 6000.0);
}

TEST(SignalDictionaryTests, ATimeoutSomebodyChoseIsNotReplacedByTheDatabasesOne)
{
    const SignalDictionary dictionary = dictionaryOf();
    const DictionaryEntry* speed = dictionary.find("EEC1", "EngineSpeed");
    ASSERT_NE(speed, nullptr);

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Gauge);
    widget.staleMs = 1234;

    applyEntry(widget, *speed);
    EXPECT_EQ(widget.staleMs, 1234U);
}

TEST(SignalDictionaryTests, ATableIsAsTallAsItsMessageUpToAScreenful)
{
    const DashboardWidget small = suggestTable("EEC1", 2);

    EXPECT_EQ(small.kind, DashboardWidgetKind::Table);
    EXPECT_EQ(small.id, "eec1_table");
    EXPECT_EQ(small.binding.source, DashboardBinding::Source::Message);
    EXPECT_EQ(small.binding.message, "EEC1");

    const DashboardWidget big = suggestTable("EEC1", 400);
    EXPECT_GT(big.height, small.height);
    EXPECT_LT(big.height, 400.0);

    DashboardDescription dashboard;
    dashboard.add(small);
    EXPECT_TRUE(dashboard.validate().succeeded());
}

// ---------------------------------------------------------------------------
// Does the binding name something?
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, ABindingThatNamesASignalThatIsThereIsFound)
{
    const SignalDictionary dictionary = dictionaryOf();

    DashboardBinding binding;
    binding.source = DashboardBinding::Source::Signal;
    binding.message = "EEC1";
    binding.signal = "EngineSpeed";

    const BindingFinding finding = dictionary.check(binding);

    EXPECT_EQ(finding.status, BindingFinding::Status::Found);
    ASSERT_NE(finding.entry, nullptr);
    EXPECT_EQ(finding.entry->qualified(), "EEC1.EngineSpeed");
}

TEST(SignalDictionaryTests, ATypoInASignalNameIsAnsweredWithTheNameThatWasMeant)
{
    const SignalDictionary dictionary = dictionaryOf();

    DashboardBinding binding;
    binding.source = DashboardBinding::Source::Signal;
    binding.message = "EEC1";

    // A swapped pair of letters, a missing one and the wrong case are all the same mistake.
    for (const char* typo : {"EngineSped", "EnginSpeed", "enginespeed", "EngineSpede"}) {
        binding.signal = typo;

        SCOPED_TRACE(typo);
        const BindingFinding finding = dictionary.check(binding);

        EXPECT_EQ(finding.status, BindingFinding::Status::UnknownSignal);
        EXPECT_EQ(finding.suggestedMessage, "EEC1");
        EXPECT_EQ(finding.suggestedSignal, "EngineSpeed");
    }

    binding.signal = "Bogus";
    const BindingFinding nothing = dictionary.check(binding);
    EXPECT_EQ(nothing.status, BindingFinding::Status::UnknownSignal);
    EXPECT_TRUE(nothing.suggestedSignal.empty());
}

TEST(SignalDictionaryTests, ASignalInTheWrongMessageIsFoundInTheOneThatHasIt)
{
    const SignalDictionary dictionary = dictionaryOf();

    DashboardBinding binding;
    binding.source = DashboardBinding::Source::Signal;
    binding.signal = "EngineSpeed";

    // The message is not one at all - "Engine" is what a person calls it - but only one message has
    // a signal of that name.
    binding.message = "Engine";
    BindingFinding finding = dictionary.check(binding);

    EXPECT_EQ(finding.status, BindingFinding::Status::UnknownMessage);
    EXPECT_EQ(finding.suggestedMessage, "EEC1");
    EXPECT_EQ(finding.suggestedSignal, "EngineSpeed");

    // The message is one, and it is the wrong one.
    binding.message = "ET1";
    finding = dictionary.check(binding);

    EXPECT_EQ(finding.status, BindingFinding::Status::UnknownSignal);
    EXPECT_EQ(finding.suggestedMessage, "EEC1");
    EXPECT_EQ(finding.suggestedSignal, "EngineSpeed");
}

TEST(SignalDictionaryTests, ATypoInAMessageNameIsAnsweredWithTheMessageThatWasMeant)
{
    const SignalDictionary dictionary = dictionaryOf();

    DashboardBinding binding;
    binding.source = DashboardBinding::Source::Signal;
    binding.signal = "EngineOilPressure";
    binding.message = "EFL_P2";

    const BindingFinding finding = dictionary.check(binding);

    EXPECT_EQ(finding.status, BindingFinding::Status::UnknownMessage);
    EXPECT_EQ(finding.suggestedMessage, "EFL_P1");

    DashboardBinding message;
    message.source = DashboardBinding::Source::Message;
    message.message = "eec1";

    const BindingFinding table = dictionary.check(message);
    EXPECT_EQ(table.status, BindingFinding::Status::UnknownMessage);
    EXPECT_EQ(table.suggestedMessage, "EEC1");

    message.message = "EEC1";
    EXPECT_EQ(dictionary.check(message).status, BindingFinding::Status::Found);
}

TEST(SignalDictionaryTests, WhatCannotBeCheckedIsNotReportedAsWrong)
{
    const SignalDictionary dictionary = dictionaryOf();

    // A variable is not in a database, and "nothing" is not either.
    DashboardBinding variable;
    variable.source = DashboardBinding::Source::Variable;
    variable.variable = "brake_pedal";
    EXPECT_EQ(dictionary.check(variable).status, BindingFinding::Status::NotChecked);
    EXPECT_EQ(dictionary.check(DashboardBinding{}).status, BindingFinding::Status::NotChecked);

    // And with no database loaded nothing can be wrong: the project may simply be open on a machine
    // that does not have it.
    DashboardBinding signal;
    signal.source = DashboardBinding::Source::Signal;
    signal.message = "Whatever";
    signal.signal = "Whatever";
    EXPECT_EQ(SignalDictionary{}.check(signal).status, BindingFinding::Status::NotChecked);
}

TEST(SignalDictionaryTests, AnAuditListsTheWidgetsWhoseBindingIsNotThereAndOnlyThose)
{
    const SignalDictionary dictionary = dictionaryOf();

    DashboardDescription dashboard;

    DashboardWidget good = suggestWidget(*dictionary.find("EEC1", "EngineSpeed"));
    dashboard.add(good);

    DashboardWidget typo = good;
    typo.id = "typo";
    typo.binding.signal = "EngineSped";
    dashboard.add(typo);

    DashboardWidget variable = defaultWidgetOf(DashboardWidgetKind::Slider);
    variable.id = "pedal";
    variable.binding.source = DashboardBinding::Source::Variable;
    variable.binding.variable = "brake_pedal";
    dashboard.add(variable);

    DashboardWidget table = suggestTable("Nope", 3);
    dashboard.add(table);

    const std::vector<WidgetFinding> findings = dictionary.audit(dashboard);

    ASSERT_EQ(findings.size(), 2U);
    EXPECT_EQ(findings[0].widgetId, "typo");
    EXPECT_EQ(findings[0].finding.suggestedSignal, "EngineSpeed");
    EXPECT_EQ(findings[1].widgetId, "nope_table");
    EXPECT_EQ(findings[1].finding.status, BindingFinding::Status::UnknownMessage);

    EXPECT_TRUE(SignalDictionary{}.audit(dashboard).empty());
}

// ---------------------------------------------------------------------------
// The databases that ship
// ---------------------------------------------------------------------------

TEST(SignalDictionaryTests, TheSuggestionsForTheSignalsOfTheJ1939ExampleAreTheOnesAPersonWouldMake)
{
    const SignalDictionary dictionary = shippedJ1939();
    ASSERT_FALSE(dictionary.empty());

    struct Case final {
        const char* message;
        const char* signal;
        DashboardWidgetKind kind;
    };

    const Case cases[] = {
        {"EEC1", "EngineSpeed", DashboardWidgetKind::Gauge},
        {"ET1", "EngineCoolantTemperature", DashboardWidgetKind::Gauge},
        {"CCVS1", "WheelBasedVehicleSpeed", DashboardWidgetKind::Gauge},
        {"AT1T1I1", "DieselExhaustFluidTankLevel", DashboardWidgetKind::Bar},
        {"EEC2", "AccelPedalPos1", DashboardWidgetKind::Bar},
        {"HOURS", "TotalEngineHours", DashboardWidgetKind::Numeric},
        {"VDHR", "TotalVehicleDistance", DashboardWidgetKind::Numeric},
        {"DM1", "RedStopLamp", DashboardWidgetKind::Lamp},
        {"CCVS1", "ParkingBrakeSwitch", DashboardWidgetKind::Lamp},
        {"BAS", "SeatBeltWarningLamp", DashboardWidgetKind::Lamp},
        {"TinyML_Proprietary", "RegimeClass", DashboardWidgetKind::State},
        {"TinyML_Proprietary", "ModelStatus", DashboardWidgetKind::State},
        {"OEL", "LightingCommand", DashboardWidgetKind::State},
        {"ETC2", "TransmissionCurrentGear", DashboardWidgetKind::Numeric},
    };

    for (const Case& test : cases) {
        const DictionaryEntry* entry = dictionary.find(test.message, test.signal);

        SCOPED_TRACE(::testing::Message() << test.message << "." << test.signal);
        ASSERT_NE(entry, nullptr);

        EXPECT_EQ(suggestKind(*entry), test.kind);
    }

    // The details of a few: a speed to two places, a temperature to none, a status with its words.
    const DashboardWidget speed =
        suggestWidget(*dictionary.find("CCVS1", "WheelBasedVehicleSpeed"));
    EXPECT_EQ(speed.unit, "km/h");
    EXPECT_EQ(speed.decimals, 2);

    const DashboardWidget coolant =
        suggestWidget(*dictionary.find("ET1", "EngineCoolantTemperature"));
    EXPECT_EQ(coolant.decimals, 0);
    EXPECT_DOUBLE_EQ(coolant.minimum, -40.0);
    EXPECT_DOUBLE_EQ(coolant.maximum, 210.0);

    const DashboardWidget status =
        suggestWidget(*dictionary.find("TinyML_Proprietary", "ModelStatus"));
    ASSERT_EQ(status.states.size(), 4U);
    EXPECT_EQ(status.states[0].tone, DashboardTone::Off);
    EXPECT_EQ(status.states[1].tone, DashboardTone::Ok);
    EXPECT_EQ(status.states[2].tone, DashboardTone::Danger);
    EXPECT_EQ(status.states[3].tone, DashboardTone::Off);

    // The lamps of the example say Off and On, which a lamp already knows.
    EXPECT_TRUE(suggestWidget(*dictionary.find("DM1", "RedStopLamp")).states.empty());
}

TEST(SignalDictionaryTests, EverySignalOfTheJ1939ExampleMakesAWidgetThatCanBeDrawnAndSaved)
{
    // The way a suggestion fails is quietly: a widget that arrives, is added, and is refused when
    // the project is opened. So every signal of the database that ships is taken through the whole
    // path, as the kind it wants and as every kind a person could ask for instead.
    const SignalDictionary dictionary = shippedJ1939();
    ASSERT_FALSE(dictionary.empty());

    for (const DictionaryEntry& entry : dictionary.entries()) {
        SCOPED_TRACE(::testing::Message() << entry.qualified());

        std::vector<std::optional<DashboardWidgetKind>> asks{std::nullopt};
        for (const DashboardWidgetKind kind : readerKinds()) {
            asks.push_back(kind);
        }

        for (const std::optional<DashboardWidgetKind>& ask : asks) {
            DashboardDescription dashboard;

            DashboardWidget widget = suggestWidget(entry, ask);
            widget.id = dashboard.uniqueId(widget.id);
            dashboard.add(widget);

            const Result result = dashboard.validate();
            EXPECT_TRUE(result.succeeded()) << std::string{result.message()};

            // And it reads what it was made from.
            EXPECT_EQ(dictionary.check(widget.binding).status, BindingFinding::Status::Found);
        }
    }
}

TEST(SignalDictionaryTests, EverySignalOfTheOtherExampleDatabasesMakesAValidWidgetToo)
{
    for (const char* file : {"/ecu.dbc", "/vehicle.dbc"}) {
        auto database = std::make_shared<CanDatabase>();

        ASSERT_TRUE(
            DbcParser::parseFile(std::string{TORQUEBUS_EXAMPLE_DATABASE_DIR} + file, *database)
                .succeeded());

        const std::vector<std::shared_ptr<const CanDatabase>> databases{database};
        const SignalDictionary dictionary{databases};
        ASSERT_FALSE(dictionary.empty());

        DashboardDescription dashboard;

        for (const DictionaryEntry& entry : dictionary.entries()) {
            DashboardWidget widget = suggestWidget(entry);
            widget.id = dashboard.uniqueId(widget.id);
            dashboard.add(std::move(widget));
        }

        SCOPED_TRACE(file);
        const Result result = dashboard.validate();
        EXPECT_TRUE(result.succeeded()) << std::string{result.message()};
    }
}
