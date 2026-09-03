// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFrame.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace torquebus;

namespace {

CanFrame makeFrame(std::uint32_t identifier, CanFrameFormat format, std::uint8_t length)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.format = format;
    frame.length = length;
    frame.dlc = dlcFromPayloadLength(length, false);

    for (std::uint8_t index = 0; index < length; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(index + 1);
    }

    return frame;
}

} // namespace

TEST_CASE("CanFrame stays cheap to copy", "[canframe]")
{
    // Rule #5: the pipeline moves millions of these. The moment CanFrame stops
    // being trivially copyable, the trace store stops being a flat buffer.
    STATIC_REQUIRE(std::is_trivially_copyable_v<CanFrame>);
    STATIC_REQUIRE(sizeof(CanFrame) <= 96);
}

TEST_CASE("A default frame is a zeroed standard Rx frame", "[canframe]")
{
    const CanFrame frame;

    CHECK(frame.identifier == 0);
    CHECK(frame.length == 0);
    CHECK(frame.dlc == 0);
    CHECK(frame.direction == CanDirection::Rx);
    CHECK(frame.format == CanFrameFormat::Standard);
    CHECK_FALSE(frame.fd);
    CHECK_FALSE(frame.isExtended());
    CHECK(frame.isRx());
}

TEST_CASE("DLC expansion follows ISO 11898-1", "[canframe][dlc]")
{
    SECTION("classic CAN never exceeds eight bytes")
    {
        for (std::uint8_t dlc = 0; dlc <= 8; ++dlc) {
            CHECK(payloadLengthFromDlc(dlc, false) == dlc);
        }
        for (std::uint8_t dlc = 9; dlc <= 15; ++dlc) {
            CHECK(payloadLengthFromDlc(dlc, false) == 8);
        }
    }

    SECTION("CAN FD uses the quantised steps")
    {
        CHECK(payloadLengthFromDlc(8, true) == 8);
        CHECK(payloadLengthFromDlc(9, true) == 12);
        CHECK(payloadLengthFromDlc(10, true) == 16);
        CHECK(payloadLengthFromDlc(11, true) == 20);
        CHECK(payloadLengthFromDlc(12, true) == 24);
        CHECK(payloadLengthFromDlc(13, true) == 32);
        CHECK(payloadLengthFromDlc(14, true) == 48);
        CHECK(payloadLengthFromDlc(15, true) == 64);
    }

    SECTION("length -> DLC -> length is stable on the exact steps")
    {
        for (const int step : {0, 1, 8, 12, 16, 20, 24, 32, 48, 64}) {
            const auto length = static_cast<std::uint8_t>(step);
            const std::uint8_t dlc = dlcFromPayloadLength(length, true);
            CHECK(payloadLengthFromDlc(dlc, true) == length);
        }
    }

    SECTION("a length between two steps rounds up, never down")
    {
        // Rounding down would silently truncate the payload of a CAN FD frame,
        // which is the kind of bug that only shows up on a real ECU.
        CHECK(payloadLengthFromDlc(dlcFromPayloadLength(9, true), true) == 12);
        CHECK(payloadLengthFromDlc(dlcFromPayloadLength(33, true), true) == 48);
        CHECK(payloadLengthFromDlc(dlcFromPayloadLength(63, true), true) == 64);
    }
}

TEST_CASE("Identifier validation respects the frame format", "[canframe][identifier]")
{
    CHECK(isValidIdentifier(0x000, CanFrameFormat::Standard));
    CHECK(isValidIdentifier(0x7FF, CanFrameFormat::Standard));
    CHECK_FALSE(isValidIdentifier(0x800, CanFrameFormat::Standard));

    CHECK(isValidIdentifier(0x800, CanFrameFormat::Extended));
    CHECK(isValidIdentifier(0x1FFF'FFFF, CanFrameFormat::Extended));
    CHECK_FALSE(isValidIdentifier(0x2000'0000, CanFrameFormat::Extended));
}

TEST_CASE("Identifiers are formatted the way automotive tools print them",
          "[canframe][format]")
{
    CHECK(toIdentifierString(makeFrame(0x100, CanFrameFormat::Standard, 0)) == "100");
    CHECK(toIdentifierString(makeFrame(0x7, CanFrameFormat::Standard, 0)) == "007");
    CHECK(toIdentifierString(makeFrame(0x18FF50E5, CanFrameFormat::Extended, 0)) == "18FF50E5");
    CHECK(toIdentifierString(makeFrame(0xCF00400, CanFrameFormat::Extended, 0)) == "0CF00400");
}

TEST_CASE("Payloads are formatted as spaced uppercase hex", "[canframe][format]")
{
    CHECK(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 0)).empty());
    CHECK(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 1)) == "01");
    CHECK(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 4)) == "01 02 03 04");

    CanFrame frame = makeFrame(0x100, CanFrameFormat::Standard, 3);
    frame.data[0] = 0x00;
    frame.data[1] = 0xAB;
    frame.data[2] = 0xFF;
    CHECK(toHexString(frame) == "00 AB FF");
}

TEST_CASE("Bus load accounting counts overhead, payload and stuffing",
          "[canframe][statistics]")
{
    const CanFrame standard = makeFrame(0x100, CanFrameFormat::Standard, 8);
    const CanFrame extended = makeFrame(0x18FF50E5, CanFrameFormat::Extended, 8);

    const std::uint32_t standardBits = approximateFrameBitCount(standard);
    const std::uint32_t extendedBits = approximateFrameBitCount(extended);

    // An 8-byte standard frame is 111 bits *without* stuffing and up to 135
    // with it, so the worst case this function reports has to sit between the
    // two. It reports 130.
    //
    // The bound here used to be `< 130`, which excluded the value the function
    // actually produces - the comment had confused 111 bits (unstuffed) with
    // the worst case, and the number was picked to match the confusion.
    CHECK(standardBits > 47 + 64);
    CHECK(standardBits <= 135);
    CHECK(extendedBits > standardBits);

    // An empty frame still costs its overhead: a bus flooded with zero-length
    // frames is not a free bus.
    CHECK(approximateFrameBitCount(makeFrame(0x100, CanFrameFormat::Standard, 0)) > 40);
}
