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

#include <gtest/gtest.h>

#include <cstdint>

using namespace torquebus;

namespace {

/// An Address Claimed from `source`, carrying `name` in the usual byte order.
[[nodiscard]] CanFrame
claimFrame(std::uint64_t name, std::uint8_t source, std::uint8_t length = kJ1939NameBytes)
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

TEST(J1939NameTests, ANAMESurvivesBeingPackedAndTakenApart)
{
    const J1939Name original = sampleName();
    const J1939Name round = j1939DecodeName(original.value());

    EXPECT_TRUE(round.arbitraryAddressCapable == original.arbitraryAddressCapable);
    EXPECT_TRUE(round.industryGroup == original.industryGroup);
    EXPECT_TRUE(round.vehicleSystemInstance == original.vehicleSystemInstance);
    EXPECT_TRUE(round.vehicleSystem == original.vehicleSystem);
    EXPECT_TRUE(round.reserved == original.reserved);
    EXPECT_TRUE(round.function == original.function);
    EXPECT_TRUE(round.functionInstance == original.functionInstance);
    EXPECT_TRUE(round.ecuInstance == original.ecuInstance);
    EXPECT_TRUE(round.manufacturerCode == original.manufacturerCode);
    EXPECT_TRUE(round.identityNumber == original.identityNumber);
}

TEST(J1939NameTests, NoFieldOfANAMEBleedsIntoItsNeighbour)
{
    // Each field set to all ones on its own. If a shift or a mask is off by a
    // bit, the value lands in the field next door and this is where it shows.
    J1939Name name;
    name.identityNumber = 0x1F'FFFFU;
    EXPECT_TRUE(j1939DecodeName(name.value()).manufacturerCode == 0U);

    name = J1939Name{};
    name.manufacturerCode = 0x7FFU;
    EXPECT_TRUE(j1939DecodeName(name.value()).identityNumber == 0U);
    EXPECT_TRUE(j1939DecodeName(name.value()).ecuInstance == 0U);

    name = J1939Name{};
    name.function = 0xFFU;
    EXPECT_TRUE(j1939DecodeName(name.value()).functionInstance == 0U);
    EXPECT_TRUE(j1939DecodeName(name.value()).reserved == false);

    name = J1939Name{};
    name.vehicleSystem = 0x7FU;
    EXPECT_TRUE(j1939DecodeName(name.value()).reserved == false);
    EXPECT_TRUE(j1939DecodeName(name.value()).vehicleSystemInstance == 0U);

    name = J1939Name{};
    name.industryGroup = 0x07U;
    EXPECT_TRUE(j1939DecodeName(name.value()).arbitraryAddressCapable == false);
    EXPECT_TRUE(j1939DecodeName(name.value()).vehicleSystemInstance == 0U);
}

TEST(J1939NameTests, TheEightBytesOfAClaimAreReadLeastSignificantFirst)
{
    // Bytes 1..8 in order. A NAME read the other way round is still a plausible
    // NAME - another manufacturer, another serial - and nothing about it looks
    // wrong, which is why the order is pinned with a value that cannot be
    // mistaken for its own reverse.
    CanFrame frame = claimFrame(0U, 0x80U);
    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(index + 1U);
    }

    EXPECT_TRUE(j1939NameBits(frame) == 0x0807'0605'0403'0201ULL);

    const std::optional<J1939Name> name = j1939NameFromClaim(frame);
    ASSERT_TRUE(name.has_value());

    // Derived from that number rather than restated: identity is the low 21
    // bits, and the vehicle system instance the low nibble of the top byte.
    EXPECT_TRUE(name->identityNumber == 0x03'0201U);
    EXPECT_TRUE(name->vehicleSystemInstance == 0x08U);
    EXPECT_FALSE(name->arbitraryAddressCapable);
}

TEST(J1939NameTests, TheLowerNAMEWinsTheAddress)
{
    J1939Name low = sampleName();
    low.identityNumber = 100U;

    J1939Name high = sampleName();
    high.identityNumber = 200U;

    EXPECT_TRUE(low.winsAgainst(high));
    EXPECT_FALSE(high.winsAgainst(low));

    // A NAME does not beat itself, or two ECUs that somehow shipped identical
    // NAMEs would each conclude it had won.
    EXPECT_FALSE(low.winsAgainst(low));
}

TEST(J1939NameTests, AnECUThatCanMoveLosesTheTieItWouldOtherwiseWin)
{
    // Identical in every other field. The arbitrary-address-capable bit is the
    // top bit of the NAME, so setting it can only make the NAME larger - and
    // the one that can go somewhere else is the one that has to.
    J1939Name fixed = sampleName();
    fixed.arbitraryAddressCapable = false;

    J1939Name movable = sampleName();
    movable.arbitraryAddressCapable = true;

    EXPECT_TRUE(fixed.winsAgainst(movable));
    EXPECT_FALSE(movable.winsAgainst(fixed));
}

TEST(J1939NameTests, AClaimShorterThanEightBytesIsRefusedNotPadded)
{
    // Padding would invent an identity number nobody transmitted, and that
    // invented NAME would then win or lose contests and be shown in a panel as
    // an ECU that exists.
    EXPECT_FALSE(j1939NameFromClaim(claimFrame(sampleName().value(), 0x80U, 7U)).has_value());
    EXPECT_TRUE(j1939NameFromClaim(claimFrame(sampleName().value(), 0x80U, 8U)).has_value());
}

TEST(J1939NameTests, AFrameThatIsNotAnAddressClaimedCarriesNoNAME)
{
    CanFrame other;
    other.identifier = j1939Identifier(kPgnDm1, 0x00U);
    other.format = CanFrameFormat::Extended;
    other.length = 8;
    other.dlc = 8;

    EXPECT_FALSE(j1939NameFromClaim(other).has_value());

    // Nor does an 11-bit frame that happens to carry the same low bits.
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    EXPECT_FALSE(j1939NameFromClaim(standard).has_value());
}

TEST(J1939NameTests, CannotClaimAddressIsRecognisedAndIsNotAClaimOn254)
{
    // An ECU that lost announces it from the null address. The NAME is real and
    // worth keeping; the address is not an address. Filing this as an occupant
    // of 254 would put a phantom ECU in the network table, at a seat that does
    // not exist.
    const CanFrame defeat = claimFrame(sampleName().value(), kJ1939NullAddress);

    EXPECT_TRUE(j1939IsCannotClaimAddress(defeat));

    const std::optional<J1939Name> name = j1939NameFromClaim(defeat);
    ASSERT_TRUE(name.has_value());
    EXPECT_TRUE(name->identityNumber == sampleName().identityNumber);

    // An ordinary claim is not one of these.
    EXPECT_FALSE(j1939IsCannotClaimAddress(claimFrame(sampleName().value(), 0x80U)));
}

TEST(J1939NameTests, TheReservedBitIsCarriedNotQuietlyCleared)
{
    // Round-tripping a NAME that normalised a bit would change an ECU identity
    // while claiming to have copied it - and the copy would lose contests the
    // original won.
    J1939Name name = sampleName();
    name.reserved = true;

    const J1939Name round = j1939DecodeName(name.value());

    EXPECT_TRUE(round.reserved);
    EXPECT_TRUE(round.value() == name.value());
}

TEST(J1939NameTests, TheIndustryGroupsAreNamedAndTheReservedOnesSaySo)
{
    EXPECT_TRUE(j1939IndustryGroupName(0U) == "Global");
    EXPECT_TRUE(j1939IndustryGroupName(1U) == "On-Highway Equipment");
    EXPECT_TRUE(j1939IndustryGroupName(2U) == "Agricultural and Forestry Equipment");
    EXPECT_TRUE(j1939IndustryGroupName(3U) == "Construction Equipment");
    EXPECT_TRUE(j1939IndustryGroupName(4U) == "Marine");
    EXPECT_TRUE(j1939IndustryGroupName(5U) == "Industrial-Process Control");

    // Not blank: a blank cell reads as a decoding failure, and this decoded.
    EXPECT_TRUE(j1939IndustryGroupName(6U) == "Reserved");
    EXPECT_TRUE(j1939IndustryGroupName(7U) == "Reserved");
}
