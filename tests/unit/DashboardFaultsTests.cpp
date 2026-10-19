// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a Faults widget lists, before it is words: which code is worse, which ECU reported it and
// what that ECU is called.
//
// These are the choices that make the rows, and they are made in the core without Qt precisely so
// that they can be asked here, with no screen. A technician reading the list is trusting three
// things they cannot check - that the worst code is on top, that the colour of a row is what its
// ECU said about it and not something the widget inferred, and that a name is a name the tables
// know and not a guess - and each is a case below.

#include "core/dashboard/DashboardFaults.h"
#include "core/dashboard/cluster/ClusterProfile.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/dashboard/cluster/ClusterRole.h"
#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr J1939LampState kOff = J1939LampState::Off;
constexpr J1939LampState kOn = J1939LampState::On;

[[nodiscard]] J1939Lamps lamps(J1939LampState stop,
                               J1939LampState warning = kOff,
                               J1939LampState malfunction = kOff,
                               J1939LampState protect = kOff)
{
    J1939Lamps result;
    result.redStop = stop;
    result.amberWarning = warning;
    result.malfunction = malfunction;
    result.protect = protect;
    return result;
}

[[nodiscard]] J1939ActiveFault
fault(std::uint8_t source, std::uint32_t spn, std::uint8_t fmi, const J1939Lamps& under)
{
    J1939ActiveFault result;
    result.sourceAddress = source;
    result.code.spn = spn;
    result.code.spnAssembled = true;
    result.code.fmi = fmi;
    result.code.occurrenceCount = 1U;
    result.lamps = under;
    return result;
}

/// An ECU seen at `address`; one that claimed its address with a NAME whose function is `function`,
/// or none that did when `function` is negative.
[[nodiscard]] J1939NetworkNode nodeAt(std::uint8_t address, int function)
{
    J1939NetworkNode node;
    node.address = address;
    node.trafficSeen = true;

    if (function >= 0) {
        J1939Name name;
        name.function = static_cast<std::uint8_t>(function);
        node.name = name;
        node.claimSeen = true;
    }

    return node;
}

constexpr int kNoClaim = -1;

/// What one J1939 block hands over: the one bus it reads.
[[nodiscard]] J1939NetworkSnapshot oneBus(J1939BusView bus)
{
    J1939NetworkSnapshot snapshot;
    snapshot.published = true;
    snapshot.buses.push_back(std::move(bus));
    return snapshot;
}

} // namespace

TEST(DashboardFaultsTests, TheStopLampMakesItsCodesDangerAndAnyOtherLitLampAttention)
{
    // Red stop is the vehicle to be stopped, whatever else is lit with it.
    EXPECT_EQ(toneOfLamps(lamps(kOn)), DashboardTone::Danger);
    EXPECT_EQ(toneOfLamps(lamps(kOn, kOn, kOn, kOn)), DashboardTone::Danger);

    // Every other lamp is attention: the amber warning, the malfunction indicator and the protect
    // lamp.
    EXPECT_EQ(toneOfLamps(lamps(kOff, kOn)), DashboardTone::Warning);
    EXPECT_EQ(toneOfLamps(lamps(kOff, kOff, kOn)), DashboardTone::Warning);
    EXPECT_EQ(toneOfLamps(lamps(kOff, kOff, kOff, kOn)), DashboardTone::Warning);
}

TEST(DashboardFaultsTests, ACodeUnderDarkLampsHasNoOpinionAndIsNotDrawnAsAlarm)
{
    // Real ECUs report a code with every lamp dark: logged, and not yet worth telling the driver.
    EXPECT_EQ(toneOfLamps(lamps(kOff)), DashboardTone::Neutral);

    // The default of a lamp is "not available" - an ECU that does not drive it - and that is not
    // lit.
    EXPECT_EQ(toneOfLamps(J1939Lamps{}), DashboardTone::Neutral);

    // The value the standard reserves is not "on" either; to the block it is a statement it will
    // not act on, and so it is to a list.
    EXPECT_EQ(toneOfLamps(lamps(J1939LampState::Reserved, J1939LampState::Reserved)),
              DashboardTone::Neutral);
}

TEST(DashboardFaultsTests, TheWorstCodesComeFirstAndTheRestKeepTheOrderTheBusGaveThem)
{
    J1939BusView bus;

    // As the block hands them over: address ascending.
    bus.activeFaults = {
        fault(0x00U, 1U, 1U, lamps(kOff, kOn)),
        fault(0x03U, 2U, 2U, lamps(kOff)),
        fault(0x0BU, 3U, 3U, lamps(kOn)),
        fault(0x15U, 4U, 4U, lamps(kOff, kOff, kOn)),
        fault(0x17U, 5U, 5U, lamps(kOn)),
        fault(0x21U, 6U, 6U, J1939Lamps{}),
    };

    const std::vector<DashboardFault> faults = dashboardFaultsOf(oneBus(bus), nullptr);

    ASSERT_EQ(faults.size(), 6U);

    // The stop lamp's, in the order they came; then the other lamps', in the order they came; then
    // the rest.
    const std::vector<std::uint32_t> spns{3U, 5U, 1U, 4U, 2U, 6U};

    for (std::size_t row = 0; row < spns.size(); ++row) {
        EXPECT_EQ(faults[row].code.spn, spns[row]) << "row " << row;
    }

    EXPECT_EQ(faults[0].tone, DashboardTone::Danger);
    EXPECT_EQ(faults[1].tone, DashboardTone::Danger);
    EXPECT_EQ(faults[2].tone, DashboardTone::Warning);
    EXPECT_EQ(faults[3].tone, DashboardTone::Warning);
    EXPECT_EQ(faults[4].tone, DashboardTone::Neutral);
    EXPECT_EQ(faults[5].tone, DashboardTone::Neutral);

    // Where each came from goes with it - and with one bus, which bus is not said.
    EXPECT_EQ(faults[0].sourceAddress, 0x0BU);
    EXPECT_EQ(faults[1].sourceAddress, 0x17U);
    EXPECT_FALSE(faults[0].channel.has_value());
}

TEST(DashboardFaultsTests, TwoCodesOfOneMessageAreAsBadAsEachOtherBecauseALampBelongsToTheMessage)
{
    // The standard has no way to say that one of the two is the reason the lamp is lit.
    J1939BusView bus;
    bus.activeFaults = {fault(0x00U, 100U, 1U, lamps(kOn)), fault(0x00U, 110U, 0U, lamps(kOn))};

    const std::vector<DashboardFault> faults = dashboardFaultsOf(oneBus(bus), nullptr);

    ASSERT_EQ(faults.size(), 2U);
    EXPECT_EQ(faults[0].tone, faults[1].tone);
    EXPECT_EQ(faults[0].code.spn, 100U);
    EXPECT_EQ(faults[1].code.spn, 110U);
}

TEST(DashboardFaultsTests, AnEcuIsNamedByTheFunctionOfTheNameItClaimedWhenTheTablesKnowIt)
{
    J1939NameTables tables;
    ASSERT_TRUE(tables.load("function,0,Engine\nfunction,3,Transmission\n").succeeded());

    J1939BusView bus;

    // The engine and the gearbox claimed their addresses and the tables know what they are. The
    // brakes claimed one with a function the tables do not list, and the body controller did not
    // claim at all - it is an address, and an address is all that is known.
    bus.nodes = {nodeAt(0x00U, 0U), nodeAt(0x03U, 3U), nodeAt(0x0BU, 99U), nodeAt(0x21U, kNoClaim)};

    bus.activeFaults = {fault(0x00U, 1U, 1U, lamps(kOn)),
                        fault(0x03U, 2U, 1U, lamps(kOn)),
                        fault(0x0BU, 3U, 1U, lamps(kOn)),
                        fault(0x21U, 4U, 1U, lamps(kOn)),

                        // An address no node was seen at at all.
                        fault(0x37U, 5U, 1U, lamps(kOn))};

    const J1939NetworkSnapshot snapshot = oneBus(bus);
    const std::vector<DashboardFault> faults = dashboardFaultsOf(snapshot, &tables);

    ASSERT_EQ(faults.size(), 5U);

    ASSERT_TRUE(faults[0].ecu.has_value());
    EXPECT_EQ(*faults[0].ecu, "Engine");
    ASSERT_TRUE(faults[1].ecu.has_value());
    EXPECT_EQ(*faults[1].ecu, "Transmission");

    EXPECT_FALSE(faults[2].ecu.has_value());
    EXPECT_FALSE(faults[3].ecu.has_value());
    EXPECT_FALSE(faults[4].ecu.has_value());

    // The address is on every row, named or not.
    EXPECT_EQ(faults[2].sourceAddress, 0x0BU);
    EXPECT_EQ(faults[4].sourceAddress, 0x37U);

    // A machine with no tables at all names nobody, and loses nothing else.
    const std::vector<DashboardFault> plain = dashboardFaultsOf(snapshot, nullptr);

    ASSERT_EQ(plain.size(), 5U);

    for (const DashboardFault& row : plain) {
        EXPECT_FALSE(row.ecu.has_value());
    }

    const J1939NameTables empty;
    EXPECT_FALSE(dashboardFaultsOf(snapshot, &empty).front().ecu.has_value());
}

TEST(DashboardFaultsTests, TheDiagnosticsAnEcuLastSaidAreNotTheCodesTheBusIsReportingNow)
{
    // The snapshot keeps what each ECU last said, for the panel that reads them afterwards. The
    // list is built from what is being reported now, which is not the same thing: a stopped
    // measurement has the first and not the second.
    J1939BusView bus;
    bus.nodes = {nodeAt(0x00U, 0U)};

    J1939Diagnostic said;
    said.active = true;
    said.sourceAddress = 0x00U;
    said.lamps = lamps(kOn);
    said.faults = {fault(0x00U, 100U, 1U, lamps(kOn)).code};
    bus.diagnostics = {said};

    EXPECT_TRUE(dashboardFaultsOf(oneBus(bus), nullptr).empty());
    EXPECT_TRUE(dashboardFaultsOf(J1939NetworkSnapshot{}, nullptr).empty());
}

TEST(DashboardFaultsTests, WithTwoBusesTheListIsBothAndEachRowSaysWhichBusItIsOn)
{
    // A truck's powertrain on CAN 1 and its body on CAN 2, a J1939 block on each. Address 0 is the
    // engine on one and something else entirely on the other: an address is a seat on one bus, and
    // each ECU is named from the claims of its own.
    J1939NameTables tables;
    ASSERT_TRUE(tables.load("function,0,Engine\nfunction,26,Body Control\n").succeeded());

    J1939BusView powertrain;
    powertrain.channel = 0U;
    powertrain.nodes = {nodeAt(0x00U, 0U)};
    powertrain.activeFaults = {fault(0x00U, 100U, 1U, lamps(kOff, kOn))};

    J1939BusView body;
    body.channel = 1U;
    body.nodes = {nodeAt(0x00U, 26U), nodeAt(0x21U, kNoClaim)};
    body.activeFaults = {fault(0x00U, 3509U, 4U, lamps(kOff)), fault(0x21U, 168U, 1U, lamps(kOn))};

    J1939NetworkSnapshot snapshot;
    snapshot.published = true;
    snapshot.buses = {powertrain, body};

    const std::vector<DashboardFault> faults = dashboardFaultsOf(snapshot, &tables);
    ASSERT_EQ(faults.size(), 3U);

    // Worst first across both buses: the body's red stop lamp, the engine's amber, the body's dark.
    EXPECT_EQ(faults[0].code.spn, 168U);
    ASSERT_TRUE(faults[0].channel.has_value());
    EXPECT_EQ(*faults[0].channel, 1U);
    EXPECT_FALSE(faults[0].ecu.has_value());

    EXPECT_EQ(faults[1].code.spn, 100U);
    ASSERT_TRUE(faults[1].channel.has_value());
    EXPECT_EQ(*faults[1].channel, 0U);
    ASSERT_TRUE(faults[1].ecu.has_value());
    EXPECT_EQ(*faults[1].ecu, "Engine");

    // The same address on the other bus, and the name its own bus gave it.
    EXPECT_EQ(faults[2].code.spn, 3509U);
    EXPECT_EQ(faults[2].sourceAddress, 0x00U);
    ASSERT_TRUE(faults[2].channel.has_value());
    EXPECT_EQ(*faults[2].channel, 1U);
    ASSERT_TRUE(faults[2].ecu.has_value());
    EXPECT_EQ(*faults[2].ecu, "Body Control");
}

TEST(DashboardFaultsTests, CodesOfOneRankKeepTheOrderOfTheBusesTheyCameFrom)
{
    // Bus by bus in channel order, and within a bus in address order: the list does not shuffle.
    J1939BusView first;
    first.channel = 0U;
    first.activeFaults = {fault(0x05U, 1U, 1U, lamps(kOn)), fault(0x07U, 2U, 1U, lamps(kOn))};

    J1939BusView second;
    second.channel = 2U;
    second.activeFaults = {fault(0x01U, 3U, 1U, lamps(kOn))};

    J1939NetworkSnapshot snapshot;
    snapshot.buses = {first, second};

    const std::vector<DashboardFault> faults = dashboardFaultsOf(snapshot, nullptr);
    ASSERT_EQ(faults.size(), 3U);
    EXPECT_EQ(faults[0].code.spn, 1U);
    EXPECT_EQ(faults[1].code.spn, 2U);
    EXPECT_EQ(faults[2].code.spn, 3U);
    ASSERT_TRUE(faults[2].channel.has_value());
    EXPECT_EQ(*faults[2].channel, 2U);
}

TEST(DashboardFaultsTests, TheLampsOfTheWidgetAreTheVariablesTheClusterReadsInTheOrderItDrawsThem)
{
    // The lamp on the cluster and the lamp on the list cannot disagree if they are one variable.
    EXPECT_EQ(kFaultLampVariables[0], kJ1939LampStopVariable);
    EXPECT_EQ(kFaultLampVariables[1], kJ1939LampWarningVariable);
    EXPECT_EQ(kFaultLampVariables[2], kJ1939LampMilVariable);
    EXPECT_EQ(kFaultLampVariables[3], kJ1939LampProtectVariable);

    const ClusterProfile* profile = ClusterProfiles::instance().find(kJ1939CommercialProfile);
    ASSERT_NE(profile, nullptr);

    const auto variableOf = [profile](ClusterRole role) -> std::string {
        for (const ClusterSource& source : profile->sources) {
            if (source.role == role) {
                return source.binding.variable;
            }
        }

        return {};
    };

    EXPECT_EQ(variableOf(ClusterRole::DmStop), std::string{kFaultLampVariables[0]});
    EXPECT_EQ(variableOf(ClusterRole::DmWarn), std::string{kFaultLampVariables[1]});
    EXPECT_EQ(variableOf(ClusterRole::DmMil), std::string{kFaultLampVariables[2]});
    EXPECT_EQ(variableOf(ClusterRole::DmProtect), std::string{kFaultLampVariables[3]});
}
