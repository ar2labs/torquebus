// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFrame.h"

#include <gtest/gtest.h>

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

TEST(CanFrameTests, CanFrameStaysCheapToCopy)
{
    // Rule #5: the pipeline moves millions of these. The moment CanFrame stops
    // being trivially copyable, the trace store stops being a flat buffer.
    static_assert(std::is_trivially_copyable_v<CanFrame>);
    static_assert(sizeof(CanFrame) <= 96);
}

TEST(CanFrameTests, ADefaultFrameIsAZeroedStandardRxFrame)
{
    const CanFrame frame;

    EXPECT_TRUE(frame.identifier == 0);
    EXPECT_TRUE(frame.length == 0);
    EXPECT_TRUE(frame.dlc == 0);
    EXPECT_TRUE(frame.direction == CanDirection::Rx);
    EXPECT_TRUE(frame.format == CanFrameFormat::Standard);
    EXPECT_FALSE(frame.fd);
    EXPECT_FALSE(frame.isExtended());
    EXPECT_TRUE(frame.isRx());
}

TEST(CanFrameTests, DLCExpansionFollowsISO118981)
{
    {
        for (std::uint8_t dlc = 0; dlc <= 8; ++dlc) {
            EXPECT_TRUE(payloadLengthFromDlc(dlc, false) == dlc);
        }
        for (std::uint8_t dlc = 9; dlc <= 15; ++dlc) {
            EXPECT_TRUE(payloadLengthFromDlc(dlc, false) == 8);
        }
    }
    {
        EXPECT_TRUE(payloadLengthFromDlc(8, true) == 8);
        EXPECT_TRUE(payloadLengthFromDlc(9, true) == 12);
        EXPECT_TRUE(payloadLengthFromDlc(10, true) == 16);
        EXPECT_TRUE(payloadLengthFromDlc(11, true) == 20);
        EXPECT_TRUE(payloadLengthFromDlc(12, true) == 24);
        EXPECT_TRUE(payloadLengthFromDlc(13, true) == 32);
        EXPECT_TRUE(payloadLengthFromDlc(14, true) == 48);
        EXPECT_TRUE(payloadLengthFromDlc(15, true) == 64);
    }
    {
        for (const int step : {0, 1, 8, 12, 16, 20, 24, 32, 48, 64}) {
            const auto length = static_cast<std::uint8_t>(step);
            const std::uint8_t dlc = dlcFromPayloadLength(length, true);
            EXPECT_TRUE(payloadLengthFromDlc(dlc, true) == length);
        }
    }
    {
        // Rounding down would silently truncate the payload of a CAN FD frame,
        // which is the kind of bug that only shows up on a real ECU.
        EXPECT_TRUE(payloadLengthFromDlc(dlcFromPayloadLength(9, true), true) == 12);
        EXPECT_TRUE(payloadLengthFromDlc(dlcFromPayloadLength(33, true), true) == 48);
        EXPECT_TRUE(payloadLengthFromDlc(dlcFromPayloadLength(63, true), true) == 64);
    }
}

TEST(CanFrameTests, IdentifierValidationRespectsTheFrameFormat)
{
    EXPECT_TRUE(isValidIdentifier(0x000, CanFrameFormat::Standard));
    EXPECT_TRUE(isValidIdentifier(0x7FF, CanFrameFormat::Standard));
    EXPECT_FALSE(isValidIdentifier(0x800, CanFrameFormat::Standard));

    EXPECT_TRUE(isValidIdentifier(0x800, CanFrameFormat::Extended));
    EXPECT_TRUE(isValidIdentifier(0x1FFF'FFFF, CanFrameFormat::Extended));
    EXPECT_FALSE(isValidIdentifier(0x2000'0000, CanFrameFormat::Extended));
}

TEST(CanFrameTests, IdentifiersAreFormattedTheWayAutomotiveToolsPrintThem)
{
    EXPECT_TRUE(toIdentifierString(makeFrame(0x100, CanFrameFormat::Standard, 0)) == "100");
    EXPECT_TRUE(toIdentifierString(makeFrame(0x7, CanFrameFormat::Standard, 0)) == "007");
    EXPECT_TRUE(toIdentifierString(makeFrame(0x18FF50E5, CanFrameFormat::Extended, 0))
                == "18FF50E5");
    EXPECT_TRUE(toIdentifierString(makeFrame(0xCF00400, CanFrameFormat::Extended, 0))
                == "0CF00400");
}

TEST(CanFrameTests, PayloadsAreFormattedAsSpacedUppercaseHex)
{
    EXPECT_TRUE(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 0)).empty());
    EXPECT_TRUE(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 1)) == "01");
    EXPECT_TRUE(toHexString(makeFrame(0x100, CanFrameFormat::Standard, 4)) == "01 02 03 04");

    CanFrame frame = makeFrame(0x100, CanFrameFormat::Standard, 3);
    frame.data[0] = 0x00;
    frame.data[1] = 0xAB;
    frame.data[2] = 0xFF;
    EXPECT_TRUE(toHexString(frame) == "00 AB FF");
}

TEST(CanFrameTests, BusLoadAccountingCountsOverheadPayloadAndStuffing)
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
    EXPECT_TRUE(standardBits > 47 + 64);
    EXPECT_TRUE(standardBits <= 135);
    EXPECT_TRUE(extendedBits > standardBits);

    // An empty frame still costs its overhead: a bus flooded with zero-length
    // frames is not a free bus.
    EXPECT_TRUE(approximateFrameBitCount(makeFrame(0x100, CanFrameFormat::Standard, 0)) > 40);
}
