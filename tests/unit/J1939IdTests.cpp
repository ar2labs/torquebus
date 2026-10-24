// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Taking a 29-bit J1939 identifier apart.
//
// Nearly every case here is about the PDU1/PDU2 boundary, because that is the
// one decision this file makes and getting it wrong is silent: the PGN comes
// out plausible, matches nothing in the database, and the tool reports a
// message it does not recognise rather than a message it decoded wrongly.

#include "core/j1939/J1939Id.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

using namespace torquebus;

namespace {

/// An extended frame carrying `identifier`, with a payload nothing here reads.
[[nodiscard]] CanFrame extendedFrame(std::uint32_t identifier)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    return frame;
}

} // namespace

TEST(J1939IdTests, EveryFieldOfA29BitIdentifierLandsWhereItBelongs)
{
    // 0x18FEE500 - Engine Hours, broadcast by the engine at address 0.
    const J1939Id id = j1939Decompose(0x18FEE500U);

    EXPECT_TRUE(id.priority == 6U);
    EXPECT_FALSE(id.extendedDataPage);
    EXPECT_FALSE(id.dataPage);
    EXPECT_TRUE(id.pduFormat == 0xFEU);
    EXPECT_TRUE(id.pduSpecific == 0xE5U);
    EXPECT_TRUE(id.sourceAddress == 0x00U);
}

TEST(J1939IdTests, APDU2GroupExtensionIsPartOfThePGN)
{
    // PF 0xFE is above the threshold, so 0xE5 is a group extension and the PGN
    // is 65253. Dropping the low byte here is the commonest way to build a PGN
    // that exists in no database.
    const J1939Id id = j1939Decompose(0x18FEE500U);

    ASSERT_TRUE(id.isPdu2());
    EXPECT_TRUE(id.pgn() == 0xFEE5U);
    EXPECT_TRUE(id.pgn() == 65253U);
}

TEST(J1939IdTests, APDU1DestinationIsNotPartOfThePGN)
{
    // 0x18EF0A0B - Proprietary A, from 0x0B to 0x0A. PF 0xEF is 239, one below
    // the threshold, so 0x0A is an address and the PGN is 0xEF00 flat.
    const J1939Id id = j1939Decompose(0x18EF0A0BU);

    ASSERT_TRUE(id.isPdu1());
    EXPECT_TRUE(id.pgn() == 0xEF00U);
    EXPECT_TRUE(id.destinationAddress() == 0x0AU);
    EXPECT_TRUE(id.sourceAddress == 0x0BU);
    EXPECT_FALSE(id.isBroadcast());
}

TEST(J1939IdTests, ThePDU1AndPDU2BoundaryFallsBetween239And240)
{
    // The whole file turns on this one comparison, so both sides of it are
    // pinned rather than assumed.
    const J1939Id last1 = j1939Decompose(j1939Identifier(0xEF00U, 0x0BU, 0x0AU));
    const J1939Id first2 = j1939Decompose(0x18F00A0BU);

    EXPECT_TRUE(last1.pduFormat == 239U);
    EXPECT_TRUE(last1.isPdu1());
    EXPECT_FALSE(last1.isPdu2());

    EXPECT_TRUE(first2.pduFormat == 240U);
    EXPECT_TRUE(first2.isPdu2());
    EXPECT_FALSE(first2.isPdu1());

    // And the consequence: the same low byte is an address on one side and part
    // of the group number on the other.
    EXPECT_TRUE(last1.pgn() == 0xEF00U);
    EXPECT_TRUE(first2.pgn() == 0xF00AU);
}

TEST(J1939IdTests, ABroadcastPDU2IsAddressedToEveryoneNotToNobody)
{
    const J1939Id id = j1939Decompose(0x18FECA00U);

    ASSERT_TRUE(id.isPdu2());
    EXPECT_TRUE(id.destinationAddress() == kJ1939GlobalAddress);
    EXPECT_TRUE(id.isBroadcast());
}

TEST(J1939IdTests, APDU1SentToTheGlobalAddressIsABroadcastToo)
{
    // 0x18EAFF00 - a request, to everybody. PDU1 with 0xFF in the destination
    // is how one asks the whole bus at once, and calling it "addressed" because
    // its PF is low would hide the one message a bus scan is built on.
    const J1939Id id = j1939Decompose(0x18EAFF00U);

    ASSERT_TRUE(id.isPdu1());
    EXPECT_TRUE(id.pgn() == kPgnRequest);
    EXPECT_TRUE(id.destinationAddress() == kJ1939GlobalAddress);
    EXPECT_TRUE(id.isBroadcast());
}

TEST(J1939IdTests, TheProtocolPGNsDecodeToTheNumbersTheStandardGivesThem)
{
    // Named constants are only worth having if they are the right numbers.
    EXPECT_TRUE(kPgnRequest == 59904U);
    EXPECT_TRUE(kPgnTransportData == 60160U);
    EXPECT_TRUE(kPgnTransportConnection == 60416U);
    EXPECT_TRUE(kPgnAddressClaimed == 60928U);
    EXPECT_TRUE(kPgnDm1 == 65226U);
    EXPECT_TRUE(kPgnDm2 == 65227U);

    // The three that carry the protocol itself are PDU1, so their low byte is a
    // destination - which is why a BAM goes to 0xFF and a CTS to one address.
    EXPECT_TRUE(j1939Decompose(0x1CECFF00U).pgn() == kPgnTransportConnection);
    EXPECT_TRUE(j1939Decompose(0x1CEB0A0BU).pgn() == kPgnTransportData);
    EXPECT_TRUE(j1939Decompose(0x18EEFF80U).pgn() == kPgnAddressClaimed);

    // DM1 is PDU2 and keeps its low byte.
    EXPECT_TRUE(j1939Decompose(0x18FECA00U).pgn() == kPgnDm1);
}

TEST(J1939IdTests, AnIdentifierSurvivesBeingTakenApartAndPutBack)
{
    for (const std::uint32_t original :
         {0x18FEE500U, 0x18EF0A0BU, 0x1CECFF00U, 0x18EEFF80U, 0x0CF00400U, 0x1FFFFFFFU}) {
        EXPECT_TRUE(j1939Decompose(original).identifier() == original);
    }
}

TEST(J1939IdTests, BuildingAnIdentifierPutsTheDestinationWhereThePDUAllows)
{
    // PDU1: the destination goes into the low byte.
    EXPECT_TRUE(j1939Identifier(kPgnRequest, 0x0BU, 0x0AU, 6U) == 0x18EA0A0BU);

    // PDU2: there is nowhere to put one, and the group extension stays.
    EXPECT_TRUE(j1939Identifier(kPgnDm1, 0x00U, 0x0AU, 6U) == 0x18FECA00U);
}

TEST(J1939IdTests, APDU1PGNCarryingAStrayLowByteDoesNotSteerTheMessage)
{
    // 0xEA0A is not a PGN - the low byte of a PDU1 group is a destination, so a
    // PGN built with one set describes no group at all. The destination given
    // by the caller is what the message is addressed with; honouring the stray
    // byte would send it to whichever ECU that value happened to name.
    EXPECT_TRUE(j1939Identifier(0xEA0AU, 0x0BU, 0xFFU)
                == j1939Identifier(kPgnRequest, 0x0BU, 0xFFU));
}

TEST(J1939IdTests, PriorityIsThreeBitsAndAWiderOneCannotReachThePageBits)
{
    // Priority arrives from a parameter form somebody can type into. Eight is
    // one past the top, and letting it carry into EDP would silently move the
    // message to another page - a different PGN, still transmitted, never
    // matched.
    const std::uint32_t built = j1939Identifier(kPgnDm1, 0x00U, kJ1939GlobalAddress, 8U);

    EXPECT_TRUE(j1939Decompose(built).priority == 0U);
    EXPECT_TRUE(j1939Decompose(built).pgn() == kPgnDm1);
}

TEST(J1939IdTests, TheDataPageBitsWidenThePGNBeyondTwoBytes)
{
    // 0x1DFF0100 - DP set, so the PGN is 0x1FF01 and not 0xFF01. A decoder that
    // reads only PF and PS collides two different groups onto one number.
    const J1939Id id = j1939Decompose(0x1DFF0100U);

    EXPECT_TRUE(id.dataPage);
    EXPECT_FALSE(id.extendedDataPage);
    EXPECT_TRUE(id.pgn() == 0x1FF01U);
    EXPECT_TRUE(id.pgn() <= kJ1939MaxPgn);
}

TEST(J1939IdTests, AStandardFrameOnAJ1939BusIsNotAJ1939Message)
{
    // An 11-bit frame shares the wire with J1939 on plenty of machines. Reading
    // its identifier as though the top byte were a PDU format invents a PGN out
    // of nothing, and the message it names belongs to nobody.
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    EXPECT_FALSE(j1939Decompose(standard).has_value());

    const std::optional<J1939Id> extended = j1939Decompose(extendedFrame(0x18FEE500U));
    ASSERT_TRUE(extended.has_value());
    EXPECT_TRUE(extended->pgn() == 0xFEE5U);
}

TEST(J1939IdTests, TheNullAddressIsDecodedNotFilteredAway)
{
    // 254 means the ECU lost an address contest and has none. It is on the bus,
    // it is transmitting, and it cannot be addressed - which is a diagnosis,
    // and only reaches somebody if the address is shown as it arrived.
    const J1939Id id = j1939Decompose(0x18FEE5FEU);

    EXPECT_TRUE(id.sourceAddress == kJ1939NullAddress);
}

TEST(J1939IdTests, ARequestForAClaimIsThreeBytesFromTheNullAddressToEveryone)
{
    // What a tool without an address sends to ask the bus who is on it: priority 6, PGN 59904 to
    // the global address, from 254, and the PGN asked for - 60928 - least significant byte first.
    const CanFrame request = j1939RequestFrame(kPgnAddressClaimed);

    EXPECT_EQ(request.identifier, 0x18EAFFFEU);
    EXPECT_TRUE(request.isExtended());
    EXPECT_EQ(request.direction, CanDirection::Tx);
    EXPECT_EQ(request.length, 3U);
    EXPECT_EQ(request.dlc, 3U);
    EXPECT_EQ(request.data[0], 0x00U);
    EXPECT_EQ(request.data[1], 0xEEU);
    EXPECT_EQ(request.data[2], 0x00U);
}

TEST(J1939IdTests, ARequestCanGoToOneECUFromOneAndOnAChannel)
{
    // The DM1 of the ECU at 0x21, asked for by the tool at 0xF9, on the third channel.
    const CanFrame request = j1939RequestFrame(kPgnDm1, 0x21U, 0xF9U, 2U);

    EXPECT_EQ(request.identifier, 0x18EA21F9U);
    EXPECT_EQ(request.channel, 2U);
    EXPECT_EQ(request.data[0], 0xCAU);
    EXPECT_EQ(request.data[1], 0xFEU);
    EXPECT_EQ(request.data[2], 0x00U);

    const J1939Id id = j1939Decompose(request.identifier);
    EXPECT_EQ(id.pgn(), kPgnRequest);
    EXPECT_EQ(id.destinationAddress(), 0x21U);
    EXPECT_EQ(id.sourceAddress, 0xF9U);
}

TEST(J1939IdTests, APgnBeyondTwoBytesSurvivesTheRequestAndAnythingBeyondThePgnIsDropped)
{
    // The data page is the third byte: a request for 0x1FF01 that lost it would ask for 0xFF01.
    EXPECT_EQ(j1939RequestFrame(0x1FF01U).data[2], 0x01U);
    EXPECT_EQ(j1939RequestedPgn(j1939RequestFrame(0x1FF01U)), 0x1FF01U);
    EXPECT_EQ(j1939RequestedPgn(j1939RequestFrame(kJ1939MaxPgn)), kJ1939MaxPgn);
    EXPECT_EQ(j1939RequestedPgn(j1939RequestFrame(0U)), 0U);

    // Above the 18 bits of a PGN there is nothing to ask for, and nothing is invented from it.
    EXPECT_EQ(j1939RequestFrame(0xFFFF'FFFFU).data[2], 0x03U);
}

TEST(J1939IdTests, TheSixBitsAboveAPgnInARequestAreNotPartOfIt)
{
    // Reserved, and whatever a sender left in them: a request for 60928 with them set is still one.
    CanFrame request = j1939RequestFrame(kPgnAddressClaimed);
    request.data[2] = 0xFCU;

    EXPECT_EQ(j1939RequestedPgn(request), kPgnAddressClaimed);
}

TEST(J1939IdTests, OnlyARequestLongEnoughToNameAPgnIsOne)
{
    // Not a request: another PGN carrying the same three bytes.
    CanFrame notARequest = j1939RequestFrame(kPgnAddressClaimed);
    notARequest.identifier = 0x18FECA00U;
    EXPECT_FALSE(j1939RequestedPgn(notARequest).has_value());

    // Two bytes name no PGN, and the third is not read off the end of what the frame carries.
    CanFrame tooShort = j1939RequestFrame(kPgnAddressClaimed);
    tooShort.length = 2U;
    tooShort.dlc = 2U;
    tooShort.data[2] = 0x00U;
    EXPECT_FALSE(j1939RequestedPgn(tooShort).has_value());

    // An 11-bit frame is not J1939: its identifier would be read as a PF it does not have.
    CanFrame standard = j1939RequestFrame(kPgnAddressClaimed);
    standard.format = CanFrameFormat::Standard;
    standard.identifier = 0x7E0U;
    EXPECT_FALSE(j1939RequestedPgn(standard).has_value());
}

TEST(J1939IdTests, AnAcknowledgementIsEightBytesWithTheAddressAndThePgnAtTheEnd)
{
    // What the ECU at 0x21 says to the tool at 0xF9 that asked it for a DM2 it has not got: control
    // negative, the group function not used, two bytes not used, the tool's address, and the PGN
    // asked for - 65227 - least significant byte first.
    const CanFrame refusal = j1939AcknowledgementFrame(
        J1939Acknowledgement{kJ1939AckNegative, 0xFFU, 0xF9U, kPgnDm2}, 0x21U, 0xF9U, 2U);

    EXPECT_EQ(refusal.identifier, 0x18E8F921U);
    EXPECT_TRUE(refusal.isExtended());
    EXPECT_EQ(refusal.direction, CanDirection::Tx);
    EXPECT_EQ(refusal.channel, 2U);
    EXPECT_EQ(refusal.length, 8U);
    EXPECT_EQ(refusal.dlc, 8U);

    const std::array<std::uint8_t, 8> expected{
        0x01U, 0xFFU, 0xFFU, 0xFFU, 0xF9U, 0xCBU, 0xFEU, 0x00U};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), refusal.data.begin()));

    // To everyone by default, which is where one goes when the asker has no address to be told at.
    EXPECT_EQ(j1939AcknowledgementFrame(J1939Acknowledgement{}, 0x21U).identifier, 0x18E8FF21U);

    // A PGN has eighteen bits, and what is above them is not put on the wire.
    const CanFrame cut = j1939AcknowledgementFrame(
        J1939Acknowledgement{kJ1939AckNegative, 0xFFU, 0xF9U, 0xFFFFFFFFU}, 0x21U);
    EXPECT_EQ(cut.data[5], 0xFFU);
    EXPECT_EQ(cut.data[6], 0xFFU);
    EXPECT_EQ(cut.data[7], 0x03U);
}

TEST(J1939IdTests, AnAcknowledgementIsReadBackFromItsFrameAndOnlyFromThat)
{
    const J1939Acknowledgement sent{kJ1939AckAccessDenied, 0x05U, 0x33U, 0x1FF01U};
    const CanFrame frame = j1939AcknowledgementFrame(sent, 0x21U, 0x33U);

    // The data page is in the third byte of the PGN, and the control byte is kept as it was sent.
    EXPECT_EQ(frame.data[7], 0x01U);
    EXPECT_EQ(j1939Acknowledgement(frame), std::optional<J1939Acknowledgement>{sent});

    // The six bits above a PGN are nobody's.
    CanFrame reserved = frame;
    reserved.data[7] = 0xFDU;
    ASSERT_TRUE(j1939Acknowledgement(reserved).has_value());
    EXPECT_EQ(j1939Acknowledgement(reserved)->pgn, 0x1FF01U);

    // Not one: another PGN, one too short, and an 11-bit frame.
    CanFrame other = frame;
    other.identifier = 0x18FECA21U;
    EXPECT_FALSE(j1939Acknowledgement(other).has_value());

    CanFrame shortOne = frame;
    shortOne.length = 7U;
    shortOne.dlc = 7U;
    EXPECT_FALSE(j1939Acknowledgement(shortOne).has_value());

    CanFrame standard = frame;
    standard.format = CanFrameFormat::Standard;
    standard.identifier = 0x7E0U;
    EXPECT_FALSE(j1939Acknowledgement(standard).has_value());
}
