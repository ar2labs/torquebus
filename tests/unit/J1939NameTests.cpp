// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The 64-bit NAME, and the contest it decides.
//
// Two things here would be silent if wrong. The byte order, because a NAME read
// backwards is still a plausible NAME - a different manufacturer, a different
// serial, and nothing in it looks broken. And the arbitration comparison,
// because a bus reaches a stable state either way: the wrong ECU keeps the
// address, answers everything asked of it, and is the wrong ECU.

#include "core/j1939/J1939Name.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace torquebus;

namespace {

/// An Address Claimed from `source`, carrying `name` in the usual byte order.
[[nodiscard]] CanFrame claimFrame(std::uint64_t name,
                                  std::uint8_t source,
                                  std::uint8_t length = kJ1939NameBytes)
{
    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnAddressClaimed, source);
    frame.format = CanFrameFormat::Extended;
    frame.length = length;
    frame.dlc = length;

    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        frame.data[index] = static_cast<std::uint8_t>((name >> (index * 8U)) & 0xFFU);
    }

    return frame;
}

/// A NAME with every field set to something distinguishable.
[[nodiscard]] J1939Name sampleName()
{
    J1939Name name;
    name.arbitraryAddressCapable = true;
    name.industryGroup = 1U;
    name.vehicleSystemInstance = 2U;
    name.vehicleSystem = 5U;
    name.reserved = false;
    name.function = 3U;
    name.functionInstance = 1U;
    name.ecuInstance = 4U;
    name.manufacturerCode = 33U;
    name.identityNumber = 0x1'2345U;

    return name;
}

} // namespace

TEST_CASE("A NAME survives being packed and taken apart", "[j1939][name]")
{
    const J1939Name original = sampleName();
    const J1939Name round = j1939DecodeName(original.value());

    CHECK(round.arbitraryAddressCapable == original.arbitraryAddressCapable);
    CHECK(round.industryGroup == original.industryGroup);
    CHECK(round.vehicleSystemInstance == original.vehicleSystemInstance);
    CHECK(round.vehicleSystem == original.vehicleSystem);
    CHECK(round.reserved == original.reserved);
    CHECK(round.function == original.function);
    CHECK(round.functionInstance == original.functionInstance);
    CHECK(round.ecuInstance == original.ecuInstance);
    CHECK(round.manufacturerCode == original.manufacturerCode);
    CHECK(round.identityNumber == original.identityNumber);
}

TEST_CASE("No field of a NAME bleeds into its neighbour", "[j1939][name]")
{
    // Each field set to all ones on its own. If a shift or a mask is off by a
    // bit, the value lands in the field next door and this is where it shows.
    J1939Name name;
    name.identityNumber = 0x1F'FFFFU;
    CHECK(j1939DecodeName(name.value()).manufacturerCode == 0U);

    name = J1939Name{};
    name.manufacturerCode = 0x7FFU;
    CHECK(j1939DecodeName(name.value()).identityNumber == 0U);
    CHECK(j1939DecodeName(name.value()).ecuInstance == 0U);

    name = J1939Name{};
    name.function = 0xFFU;
    CHECK(j1939DecodeName(name.value()).functionInstance == 0U);
    CHECK(j1939DecodeName(name.value()).reserved == false);

    name = J1939Name{};
    name.vehicleSystem = 0x7FU;
    CHECK(j1939DecodeName(name.value()).reserved == false);
    CHECK(j1939DecodeName(name.value()).vehicleSystemInstance == 0U);

    name = J1939Name{};
    name.industryGroup = 0x07U;
    CHECK(j1939DecodeName(name.value()).arbitraryAddressCapable == false);
    CHECK(j1939DecodeName(name.value()).vehicleSystemInstance == 0U);
}

TEST_CASE("The eight bytes of a claim are read least significant first",
          "[j1939][name]")
{
    // Bytes 1..8 in order. A NAME read the other way round is still a plausible
    // NAME - another manufacturer, another serial - and nothing about it looks
    // wrong, which is why the order is pinned with a value that cannot be
    // mistaken for its own reverse.
    CanFrame frame = claimFrame(0U, 0x80U);
    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(index + 1U);
    }

    CHECK(j1939NameBits(frame) == 0x0807'0605'0403'0201ULL);

    const std::optional<J1939Name> name = j1939NameFromClaim(frame);
    REQUIRE(name.has_value());

    // Derived from that number rather than restated: identity is the low 21
    // bits, and the vehicle system instance the low nibble of the top byte.
    CHECK(name->identityNumber == 0x03'0201U);
    CHECK(name->vehicleSystemInstance == 0x08U);
    CHECK_FALSE(name->arbitraryAddressCapable);
}

TEST_CASE("The lower NAME wins the address", "[j1939][name]")
{
    J1939Name low = sampleName();
    low.identityNumber = 100U;

    J1939Name high = sampleName();
    high.identityNumber = 200U;

    CHECK(low.winsAgainst(high));
    CHECK_FALSE(high.winsAgainst(low));

    // A NAME does not beat itself, or two ECUs that somehow shipped identical
    // NAMEs would each conclude it had won.
    CHECK_FALSE(low.winsAgainst(low));
}

TEST_CASE("An ECU that can move loses the tie it would otherwise win",
          "[j1939][name]")
{
    // Identical in every other field. The arbitrary-address-capable bit is the
    // top bit of the NAME, so setting it can only make the NAME larger - and
    // the one that can go somewhere else is the one that has to.
    J1939Name fixed = sampleName();
    fixed.arbitraryAddressCapable = false;

    J1939Name movable = sampleName();
    movable.arbitraryAddressCapable = true;

    CHECK(fixed.winsAgainst(movable));
    CHECK_FALSE(movable.winsAgainst(fixed));
}

TEST_CASE("A claim shorter than eight bytes is refused, not padded",
          "[j1939][name]")
{
    // Padding would invent an identity number nobody transmitted, and that
    // invented NAME would then win or lose contests and be shown in a panel as
    // an ECU that exists.
    CHECK_FALSE(j1939NameFromClaim(claimFrame(sampleName().value(), 0x80U, 7U)).has_value());
    CHECK(j1939NameFromClaim(claimFrame(sampleName().value(), 0x80U, 8U)).has_value());
}

TEST_CASE("A frame that is not an Address Claimed carries no NAME",
          "[j1939][name]")
{
    CanFrame other;
    other.identifier = j1939Identifier(kPgnDm1, 0x00U);
    other.format = CanFrameFormat::Extended;
    other.length = 8;
    other.dlc = 8;

    CHECK_FALSE(j1939NameFromClaim(other).has_value());

    // Nor does an 11-bit frame that happens to carry the same low bits.
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    CHECK_FALSE(j1939NameFromClaim(standard).has_value());
}

TEST_CASE("Cannot Claim Address is recognised and is not a claim on 254",
          "[j1939][name]")
{
    // An ECU that lost announces it from the null address. The NAME is real and
    // worth keeping; the address is not an address. Filing this as an occupant
    // of 254 would put a phantom ECU in the network table, at a seat that does
    // not exist.
    const CanFrame defeat = claimFrame(sampleName().value(), kJ1939NullAddress);

    CHECK(j1939IsCannotClaimAddress(defeat));

    const std::optional<J1939Name> name = j1939NameFromClaim(defeat);
    REQUIRE(name.has_value());
    CHECK(name->identityNumber == sampleName().identityNumber);

    // An ordinary claim is not one of these.
    CHECK_FALSE(j1939IsCannotClaimAddress(claimFrame(sampleName().value(), 0x80U)));
}

TEST_CASE("The reserved bit is carried, not quietly cleared", "[j1939][name]")
{
    // Round-tripping a NAME that normalised a bit would change an ECU identity
    // while claiming to have copied it - and the copy would lose contests the
    // original won.
    J1939Name name = sampleName();
    name.reserved = true;

    const J1939Name round = j1939DecodeName(name.value());

    CHECK(round.reserved);
    CHECK(round.value() == name.value());
}

TEST_CASE("The industry groups are named, and the reserved ones say so",
          "[j1939][name]")
{
    CHECK(j1939IndustryGroupName(0U) == "Global");
    CHECK(j1939IndustryGroupName(1U) == "On-Highway Equipment");
    CHECK(j1939IndustryGroupName(2U) == "Agricultural and Forestry Equipment");
    CHECK(j1939IndustryGroupName(3U) == "Construction Equipment");
    CHECK(j1939IndustryGroupName(4U) == "Marine");
    CHECK(j1939IndustryGroupName(5U) == "Industrial-Process Control");

    // Not blank: a blank cell reads as a decoding failure, and this decoded.
    CHECK(j1939IndustryGroupName(6U) == "Reserved");
    CHECK(j1939IndustryGroupName(7U) == "Reserved");
}
