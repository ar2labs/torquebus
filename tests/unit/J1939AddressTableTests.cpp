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

#include <catch2/catch_test_macros.hpp>

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

[[nodiscard]] CanFrame claimFrame(const J1939Name& name,
                                  std::uint8_t source,
                                  std::uint8_t length = kJ1939NameBytes)
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

TEST_CASE("A claim seats an ECU at an address", "[j1939][network]")
{
    J1939AddressTable table;
    CHECK(table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U));

    REQUIRE(table.nodes().size() == 1U);

    const J1939Node& node = table.nodes()[0];
    CHECK(node.address == kEngine);
    CHECK(node.claimSeen);
    REQUIRE(node.name.has_value());
    CHECK(node.name->identityNumber == 100U);
    CHECK(node.firstSeenNs == 1000U);

    REQUIRE(table.events().size() == 1U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::AddressClaimed);
}

TEST_CASE("The same ECU announcing itself again is not a contest",
          "[j1939][network]")
{
    // ECUs re-announce on request and after a contest. Reporting each one as a
    // dispute would fill the panel with events that say nothing happened.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    CHECK(table.events().empty());
    CHECK(table.nodes().size() == 1U);
    CHECK(table.nodes()[0].lastSeenNs == 2000U);
}

TEST_CASE("The lower NAME takes the address", "[j1939][network]")
{
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(200U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    REQUIRE(table.events().size() == 2U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::AddressContested);
    CHECK(table.events()[1].kind == J1939NetworkEvent::Kind::AddressTaken);

    // Both sides of the contest are carried, because "who lost it" is the
    // question somebody is actually asking.
    REQUIRE(table.events()[1].name.has_value());
    REQUIRE(table.events()[1].previousName.has_value());
    CHECK(table.events()[1].name->identityNumber == 100U);
    CHECK(table.events()[1].previousName->identityNumber == 200U);

    REQUIRE(table.nodes()[0].name.has_value());
    CHECK(table.nodes()[0].name->identityNumber == 100U);
}

TEST_CASE("A higher NAME loses, and the seat does not change",
          "[j1939][network]")
{
    // The loser is required to stop using the address. Whether it actually does
    // is exactly what somebody is watching this table to find out - so the
    // contest is reported even though nothing moved.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(claimFrame(nameWith(200U), kEngine), 2000U);

    REQUIRE(table.events().size() == 1U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::AddressContested);

    REQUIRE(table.nodes()[0].name.has_value());
    CHECK(table.nodes()[0].name->identityNumber == 100U);
}

TEST_CASE("An ECU with no address is not seated at 254", "[j1939][network]")
{
    // 254 is not a seat. Listing a defeated ECU as its occupant would put a
    // phantom at an address that does not exist, and the next person to read
    // the table would try to talk to it.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kJ1939NullAddress), 1000U);

    CHECK(table.nodes().empty());
    CHECK(table.find(kJ1939NullAddress) == nullptr);

    REQUIRE(table.defeated().size() == 1U);
    CHECK(table.defeated()[0].name.identityNumber == 100U);
    CHECK(table.defeated()[0].announcements == 1U);

    REQUIRE(table.events().size() == 1U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::CannotClaim);

    // Announcing defeat again counts, without a second event.
    table.clearEvents();
    table.onFrame(claimFrame(nameWith(100U), kJ1939NullAddress), 2000U);

    CHECK(table.events().empty());
    REQUIRE(table.defeated().size() == 1U);
    CHECK(table.defeated()[0].announcements == 2U);
    CHECK(table.defeated()[0].lastSeenNs == 2000U);
}

TEST_CASE("There is no seat at the global address", "[j1939][network]")
{
    // A claim from 255 is malformed. Recording it would create an occupant of
    // every address at once.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kJ1939GlobalAddress), 1000U);

    CHECK(table.nodes().empty());
    CHECK(table.defeated().empty());
    CHECK(table.events().empty());
}

TEST_CASE("Traffic from an address with no claim is reported once",
          "[j1939][network]")
{
    J1939AddressTable table;
    table.onFrame(trafficFrame(kGearbox), 1000U);
    table.onFrame(trafficFrame(kGearbox), 2000U);
    table.onFrame(trafficFrame(kGearbox), 3000U);

    REQUIRE(table.events().size() == 1U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::UnclaimedTraffic);
    CHECK(table.events()[0].address == kGearbox);

    // The event is about not knowing a NAME, so it does not carry one.
    CHECK_FALSE(table.events()[0].name.has_value());

    REQUIRE(table.nodes().size() == 1U);
    CHECK(table.nodes()[0].trafficSeen);
    CHECK_FALSE(table.nodes()[0].claimSeen);
    CHECK(table.nodes()[0].framesSeen == 3U);
}

TEST_CASE("Traffic from an ECU that already claimed is not called unclaimed",
          "[j1939][network]")
{
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.clearEvents();

    table.onFrame(trafficFrame(kEngine), 2000U);

    CHECK(table.events().empty());
    CHECK(table.nodes()[0].claimSeen);
    CHECK(table.nodes()[0].trafficSeen);
}

TEST_CASE("A claim arriving after the traffic fills in the NAME",
          "[j1939][network]")
{
    // The ordinary shape of a measurement started before an ECU was reset: the
    // traffic comes first and the claim explains it afterwards.
    J1939AddressTable table;
    table.onFrame(trafficFrame(kEngine), 1000U);
    table.onFrame(claimFrame(nameWith(100U), kEngine), 2000U);

    REQUIRE(table.events().size() == 2U);
    CHECK(table.events()[0].kind == J1939NetworkEvent::Kind::UnclaimedTraffic);
    CHECK(table.events()[1].kind == J1939NetworkEvent::Kind::AddressClaimed);

    REQUIRE(table.nodes().size() == 1U);
    CHECK(table.nodes()[0].claimSeen);
    REQUIRE(table.nodes()[0].name.has_value());
    CHECK(table.nodes()[0].name->identityNumber == 100U);

    // And the first sighting is still the traffic, not the claim.
    CHECK(table.nodes()[0].firstSeenNs == 1000U);
}

TEST_CASE("Addresses come out in order, whatever order they arrived in",
          "[j1939][network]")
{
    J1939AddressTable table;
    for (const std::uint8_t address : {std::uint8_t{0x30U}, std::uint8_t{0x03U},
                                       std::uint8_t{0xF0U}, std::uint8_t{0x00U}}) {
        table.onFrame(trafficFrame(address), 1000U);
    }

    REQUIRE(table.nodes().size() == 4U);
    CHECK(table.nodes()[0].address == 0x00U);
    CHECK(table.nodes()[1].address == 0x03U);
    CHECK(table.nodes()[2].address == 0x30U);
    CHECK(table.nodes()[3].address == 0xF0U);

    CHECK(table.find(0x30U) != nullptr);
    CHECK(table.find(0x31U) == nullptr);
}

TEST_CASE("A claim too short to hold a NAME records nothing",
          "[j1939][network]")
{
    // The NAME layer refuses to invent the missing bytes, and a claim without a
    // NAME names nobody - so there is no ECU to seat.
    J1939AddressTable table;
    CHECK(table.onFrame(claimFrame(nameWith(100U), kEngine, 7U), 1000U));

    CHECK(table.nodes().empty());
    CHECK(table.events().empty());
}

TEST_CASE("An 11-bit frame teaches the table nothing", "[j1939][network]")
{
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    J1939AddressTable table;
    CHECK_FALSE(table.onFrame(standard, 1000U));
    CHECK(table.nodes().empty());
}

TEST_CASE("Starting a measurement forgets the bus", "[j1939][network]")
{
    // The membership of a bus is a fact about the run, not about the tool.
    J1939AddressTable table;
    table.onFrame(claimFrame(nameWith(100U), kEngine), 1000U);
    table.onFrame(claimFrame(nameWith(200U), kJ1939NullAddress), 1000U);

    table.reset();

    CHECK(table.nodes().empty());
    CHECK(table.defeated().empty());
    CHECK(table.events().empty());
}
