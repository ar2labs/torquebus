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

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

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

TEST_CASE("Every field of a 29-bit identifier lands where it belongs", "[j1939]")
{
    // 0x18FEE500 - Engine Hours, broadcast by the engine at address 0.
    const J1939Id id = j1939Decompose(0x18FEE500U);

    CHECK(id.priority == 6U);
    CHECK_FALSE(id.extendedDataPage);
    CHECK_FALSE(id.dataPage);
    CHECK(id.pduFormat == 0xFEU);
    CHECK(id.pduSpecific == 0xE5U);
    CHECK(id.sourceAddress == 0x00U);
}

TEST_CASE("A PDU2 group extension is part of the PGN", "[j1939]")
{
    // PF 0xFE is above the threshold, so 0xE5 is a group extension and the PGN
    // is 65253. Dropping the low byte here is the commonest way to build a PGN
    // that exists in no database.
    const J1939Id id = j1939Decompose(0x18FEE500U);

    REQUIRE(id.isPdu2());
    CHECK(id.pgn() == 0xFEE5U);
    CHECK(id.pgn() == 65253U);
}

TEST_CASE("A PDU1 destination is not part of the PGN", "[j1939]")
{
    // 0x18EF0A0B - Proprietary A, from 0x0B to 0x0A. PF 0xEF is 239, one below
    // the threshold, so 0x0A is an address and the PGN is 0xEF00 flat.
    const J1939Id id = j1939Decompose(0x18EF0A0BU);

    REQUIRE(id.isPdu1());
    CHECK(id.pgn() == 0xEF00U);
    CHECK(id.destinationAddress() == 0x0AU);
    CHECK(id.sourceAddress == 0x0BU);
    CHECK_FALSE(id.isBroadcast());
}

TEST_CASE("The PDU1 and PDU2 boundary falls between 239 and 240", "[j1939]")
{
    // The whole file turns on this one comparison, so both sides of it are
    // pinned rather than assumed.
    const J1939Id last1 = j1939Decompose(j1939Identifier(0xEF00U, 0x0BU, 0x0AU));
    const J1939Id first2 = j1939Decompose(0x18F00A0BU);

    CHECK(last1.pduFormat == 239U);
    CHECK(last1.isPdu1());
    CHECK_FALSE(last1.isPdu2());

    CHECK(first2.pduFormat == 240U);
    CHECK(first2.isPdu2());
    CHECK_FALSE(first2.isPdu1());

    // And the consequence: the same low byte is an address on one side and part
    // of the group number on the other.
    CHECK(last1.pgn() == 0xEF00U);
    CHECK(first2.pgn() == 0xF00AU);
}

TEST_CASE("A broadcast PDU2 is addressed to everyone, not to nobody", "[j1939]")
{
    const J1939Id id = j1939Decompose(0x18FECA00U);

    REQUIRE(id.isPdu2());
    CHECK(id.destinationAddress() == kJ1939GlobalAddress);
    CHECK(id.isBroadcast());
}

TEST_CASE("A PDU1 sent to the global address is a broadcast too", "[j1939]")
{
    // 0x18EAFF00 - a request, to everybody. PDU1 with 0xFF in the destination
    // is how one asks the whole bus at once, and calling it "addressed" because
    // its PF is low would hide the one message a bus scan is built on.
    const J1939Id id = j1939Decompose(0x18EAFF00U);

    REQUIRE(id.isPdu1());
    CHECK(id.pgn() == kPgnRequest);
    CHECK(id.destinationAddress() == kJ1939GlobalAddress);
    CHECK(id.isBroadcast());
}

TEST_CASE("The protocol PGNs decode to the numbers the standard gives them", "[j1939]")
{
    // Named constants are only worth having if they are the right numbers.
    CHECK(kPgnRequest == 59904U);
    CHECK(kPgnTransportData == 60160U);
    CHECK(kPgnTransportConnection == 60416U);
    CHECK(kPgnAddressClaimed == 60928U);
    CHECK(kPgnDm1 == 65226U);
    CHECK(kPgnDm2 == 65227U);

    // The three that carry the protocol itself are PDU1, so their low byte is a
    // destination - which is why a BAM goes to 0xFF and a CTS to one address.
    CHECK(j1939Decompose(0x1CECFF00U).pgn() == kPgnTransportConnection);
    CHECK(j1939Decompose(0x1CEB0A0BU).pgn() == kPgnTransportData);
    CHECK(j1939Decompose(0x18EEFF80U).pgn() == kPgnAddressClaimed);

    // DM1 is PDU2 and keeps its low byte.
    CHECK(j1939Decompose(0x18FECA00U).pgn() == kPgnDm1);
}

TEST_CASE("An identifier survives being taken apart and put back", "[j1939]")
{
    for (const std::uint32_t original :
         {0x18FEE500U, 0x18EF0A0BU, 0x1CECFF00U, 0x18EEFF80U, 0x0CF00400U, 0x1FFFFFFFU}) {
        CHECK(j1939Decompose(original).identifier() == original);
    }
}

TEST_CASE("Building an identifier puts the destination where the PDU allows", "[j1939]")
{
    // PDU1: the destination goes into the low byte.
    CHECK(j1939Identifier(kPgnRequest, 0x0BU, 0x0AU, 6U) == 0x18EA0A0BU);

    // PDU2: there is nowhere to put one, and the group extension stays.
    CHECK(j1939Identifier(kPgnDm1, 0x00U, 0x0AU, 6U) == 0x18FECA00U);
}

TEST_CASE("A PDU1 PGN carrying a stray low byte does not steer the message", "[j1939]")
{
    // 0xEA0A is not a PGN - the low byte of a PDU1 group is a destination, so a
    // PGN built with one set describes no group at all. The destination given
    // by the caller is what the message is addressed with; honouring the stray
    // byte would send it to whichever ECU that value happened to name.
    CHECK(j1939Identifier(0xEA0AU, 0x0BU, 0xFFU) == j1939Identifier(kPgnRequest, 0x0BU, 0xFFU));
}

TEST_CASE("Priority is three bits, and a wider one cannot reach the page bits", "[j1939]")
{
    // Priority arrives from a parameter form somebody can type into. Eight is
    // one past the top, and letting it carry into EDP would silently move the
    // message to another page - a different PGN, still transmitted, never
    // matched.
    const std::uint32_t built = j1939Identifier(kPgnDm1, 0x00U, kJ1939GlobalAddress, 8U);

    CHECK(j1939Decompose(built).priority == 0U);
    CHECK(j1939Decompose(built).pgn() == kPgnDm1);
}

TEST_CASE("The data page bits widen the PGN beyond two bytes", "[j1939]")
{
    // 0x1DFF0100 - DP set, so the PGN is 0x1FF01 and not 0xFF01. A decoder that
    // reads only PF and PS collides two different groups onto one number.
    const J1939Id id = j1939Decompose(0x1DFF0100U);

    CHECK(id.dataPage);
    CHECK_FALSE(id.extendedDataPage);
    CHECK(id.pgn() == 0x1FF01U);
    CHECK(id.pgn() <= kJ1939MaxPgn);
}

TEST_CASE("A standard frame on a J1939 bus is not a J1939 message", "[j1939]")
{
    // An 11-bit frame shares the wire with J1939 on plenty of machines. Reading
    // its identifier as though the top byte were a PDU format invents a PGN out
    // of nothing, and the message it names belongs to nobody.
    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;

    CHECK_FALSE(j1939Decompose(standard).has_value());

    const std::optional<J1939Id> extended = j1939Decompose(extendedFrame(0x18FEE500U));
    REQUIRE(extended.has_value());
    CHECK(extended->pgn() == 0xFEE5U);
}

TEST_CASE("The null address is decoded, not filtered away", "[j1939]")
{
    // 254 means the ECU lost an address contest and has none. It is on the bus,
    // it is transmitting, and it cannot be addressed - which is a diagnosis,
    // and only reaches somebody if the address is shown as it arrived.
    const J1939Id id = j1939Decompose(0x18FEE5FEU);

    CHECK(id.sourceAddress == kJ1939NullAddress);
}
