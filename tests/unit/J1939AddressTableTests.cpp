// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Who is on the bus, built from what went past.
//
// The cases that matter are the ones where a plausible wrong answer would be
// accepted without question: an ECU listed at an address it lost, a phantom
// sitting at 254, or a seat at 255 - which is every seat.

#include "core/j1939/J1939AddressTable.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace torquebus;

namespace {

constexpr std::uint8_t kEngine = 0x00U;
constexpr std::uint8_t kGearbox = 0x03U;

/// A NAME whose identity number is `identity`. Lower identity means a lower
/// NAME, and so a winner - every other field is held equal.
[[nodiscard]] J1939Name nameWith(std::uint32_t identity)
{
    J1939Name name;
    name.industryGroup = 1U;
    name.function = 3U;
    name.manufacturerCode = 33U;
    name.identityNumber = identity;

    return name;
}

[[nodiscard]] CanFrame
claimFrame(const J1939Name& name, std::uint8_t source, std::uint8_t length = kJ1939NameBytes)
{
    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnAddressClaimed, source);
    frame.format = CanFrameFormat::Extended;
    frame.length = length;
    frame.dlc = length;

    const std::uint64_t bits = name.value();
    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        frame.data[index] = static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xFFU);
    }

    return frame;
}

/// Any ordinary broadcast from `source`.
[[nodiscard]] CanFrame trafficFrame(std::uint8_t source)
{
    CanFrame frame;
    frame.identifier = j1939Identifier(0x0'FEE5U, source);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    return frame;
}

} // namespace

TEST(J1939AddressTableTests, AClaimSeatsAnECUAtAnAddress)
{
    J1939AddressTable table;
    EXPECT_TRUE(table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U));

    ASSERT_TRUE(table.nodes().size() == 1U);

    const J1939NetworkNode& node = table.nodes()[0];
    EXPECT_TRUE(node.address == kEngine);
    EXPECT_TRUE(node.claimSeen);
    ASSERT_TRUE(node.name.has_value());
    EXPECT_TRUE(node.name->identityNumber == 100U);
    EXPECT_TRUE(node.firstSeenNs == 1000U);

    ASSERT_TRUE(table.events().size() == 1U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::AddressClaimed);
}

TEST(J1939AddressTableTests, TheSameECUAnnouncingItselfAgainIsNotAContest)
{
    // ECUs re-announce on request and after a contest. Reporting each one as a
    // dispute would fill the panel with events that say nothing happened.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    EXPECT_TRUE(table.events().empty());
    EXPECT_TRUE(table.nodes().size() == 1U);
    EXPECT_TRUE(table.nodes()[0].lastSeenNs == 2000U);
}

TEST(J1939AddressTableTests, TheLowerNAMETakesTheAddress)
{
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(200U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    ASSERT_TRUE(table.events().size() == 2U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::AddressContested);
    EXPECT_TRUE(table.events()[1].kind == J1939NetworkEvent::Kind::AddressTaken);

    // Both sides of the contest are carried, because "who lost it" is the
    // question somebody is actually asking.
    ASSERT_TRUE(table.events()[1].name.has_value());
    ASSERT_TRUE(table.events()[1].previousName.has_value());
    EXPECT_TRUE(table.events()[1].name->identityNumber == 100U);
    EXPECT_TRUE(table.events()[1].previousName->identityNumber == 200U);

    ASSERT_TRUE(table.nodes()[0].name.has_value());
    EXPECT_TRUE(table.nodes()[0].name->identityNumber == 100U);
}

TEST(J1939AddressTableTests, AHigherNAMELosesAndTheSeatDoesNotChange)
{
    // The loser is required to stop using the address. Whether it actually does
    // is exactly what somebody is watching this table to find out - so the
    // contest is reported even though nothing moved.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(200U), kEngine), 2000U);

    ASSERT_TRUE(table.events().size() == 1U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::AddressContested);

    ASSERT_TRUE(table.nodes()[0].name.has_value());
    EXPECT_TRUE(table.nodes()[0].name->identityNumber == 100U);
}

TEST(J1939AddressTableTests, AnECUWithNoAddressIsNotSeatedAt254)
{
    // 254 is not a seat. Listing a defeated ECU as its occupant would put a
    // phantom at an address that does not exist, and the next person to read
    // the table would try to talk to it.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kJ1939NullAddress), 1000U);

    EXPECT_TRUE(table.nodes().empty());
    EXPECT_TRUE(table.find(kJ1939NullAddress) == nullptr);

    ASSERT_TRUE(table.defeated().size() == 1U);
    EXPECT_TRUE(table.defeated()[0].name.identityNumber == 100U);
    EXPECT_TRUE(table.defeated()[0].announcements == 1U);

    ASSERT_TRUE(table.events().size() == 1U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::CannotClaim);

    // Announcing defeat again counts, without a second event.
    table.clearEvents();
    table.onFrame(claimFrame(nameWith(100U), kJ1939NullAddress), 2000U);

    EXPECT_TRUE(table.events().empty());
    ASSERT_TRUE(table.defeated().size() == 1U);
    EXPECT_TRUE(table.defeated()[0].announcements == 2U);
    EXPECT_TRUE(table.defeated()[0].lastSeenNs == 2000U);
}

TEST(J1939AddressTableTests, ThereIsNoSeatAtTheGlobalAddress)
{
    // A claim from 255 is malformed. Recording it would create an occupant of
    // every address at once.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kJ1939GlobalAddress), 1000U);

    EXPECT_TRUE(table.nodes().empty());
    EXPECT_TRUE(table.defeated().empty());
    EXPECT_TRUE(table.events().empty());
}

TEST(J1939AddressTableTests, TrafficFromAnAddressWithNoClaimIsReportedOnce)
{
    J1939AddressTable table;
    table.onFrame(trafficFrame(kGearbox), 1000U);
    table.onFrame(trafficFrame(kGearbox), 2000U);
    table.onFrame(trafficFrame(kGearbox), 3000U);

    ASSERT_TRUE(table.events().size() == 1U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::UnclaimedTraffic);
    EXPECT_TRUE(table.events()[0].address == kGearbox);

    // The event is about not knowing a NAME, so it does not carry one.
    EXPECT_FALSE(table.events()[0].name.has_value());

    ASSERT_TRUE(table.nodes().size() == 1U);
    EXPECT_TRUE(table.nodes()[0].trafficSeen);
    EXPECT_FALSE(table.nodes()[0].claimSeen);
    EXPECT_TRUE(table.nodes()[0].framesSeen == 3U);
}

TEST(J1939AddressTableTests, TrafficFromAnECUThatAlreadyClaimedIsNotCalledUnclaimed)
{
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(trafficFrame(kEngine), 2000U);

    EXPECT_TRUE(table.events().empty());
    EXPECT_TRUE(table.nodes()[0].claimSeen);
    EXPECT_TRUE(table.nodes()[0].trafficSeen);
}

TEST(J1939AddressTableTests, AClaimArrivingAfterTheTrafficFillsInTheNAME)
{
    // The ordinary shape of a measurement started before an ECU was reset: the
    // traffic comes first and the claim explains it afterwards.
    J1939AddressTable table;
    table.onFrame(trafficFrame(kEngine), 1000U);
    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    ASSERT_TRUE(table.events().size() == 2U);
    EXPECT_TRUE(table.events()[0].kind == J1939NetworkEvent::Kind::UnclaimedTraffic);
    EXPECT_TRUE(table.events()[1].kind == J1939NetworkEvent::Kind::AddressClaimed);

    ASSERT_TRUE(table.nodes().size() == 1U);
    EXPECT_TRUE(table.nodes()[0].claimSeen);
    ASSERT_TRUE(table.nodes()[0].name.has_value());
    EXPECT_TRUE(table.nodes()[0].name->identityNumber == 100U);

    // And the first sighting is still the traffic, not the claim.
    EXPECT_TRUE(table.nodes()[0].firstSeenNs == 1000U);
}

TEST(J1939AddressTableTests, AddressesComeOutInOrderWhateverOrderTheyArrivedIn)
{
    J1939AddressTable table;
    for (const std::uint8_t address :
         {std::uint8_t{0x30U}, std::uint8_t{0x03U}, std::uint8_t{0xF0U}, std::uint8_t{0x00U}}) {
        table.onFrame(trafficFrame(address), 1000U);
    }

    ASSERT_TRUE(table.nodes().size() == 4U);
    EXPECT_TRUE(table.nodes()[0].address == 0x00U);
    EXPECT_TRUE(table.nodes()[1].address == 0x03U);
    EXPECT_TRUE(table.nodes()[2].address == 0x30U);
    EXPECT_TRUE(table.nodes()[3].address == 0xF0U);

    EXPECT_TRUE(table.find(0x30U) != nullptr);
    EXPECT_TRUE(table.find(0x31U) == nullptr);
}

TEST(J1939AddressTableTests, AClaimTooShortToHoldANAMERecordsNothing)
{
    // The NAME layer refuses to invent the missing bytes, and a claim without a
    // NAME names nobody - so there is no ECU to seat.
    J1939AddressTable table;
    EXPECT_TRUE(table.onFrame(claimFrame(nameWith(100U), kEngine, 7U), 1000U));

    EXPECT_TRUE(table.nodes().empty());
    EXPECT_TRUE(table.events().empty());
}

TEST(J1939AddressTableTests, An11BitFrameTeachesTheTableNothing)
{
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    J1939AddressTable table;
    EXPECT_FALSE(table.onFrame(standard, 1000U));
    EXPECT_TRUE(table.nodes().empty());
}

TEST(J1939AddressTableTests, StartingAMeasurementForgetsTheBus)
{
    // The membership of a bus is a fact about the run, not about the tool.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.onFrame(claimFrame(nameWith(200U), kJ1939NullAddress), 1000U);

    table.reset();

    EXPECT_TRUE(table.nodes().empty());
    EXPECT_TRUE(table.defeated().empty());
    EXPECT_TRUE(table.events().empty());
}
