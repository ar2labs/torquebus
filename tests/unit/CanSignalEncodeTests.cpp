// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Encoding is decoding's inverse, and the strongest test available is that
// composing them is the identity - on the same published vector the decoder is
// anchored to. Encoding the three motohawk signals into an empty payload has to
// produce C0 06 E0 00 00 00 00 00, byte for byte, or one of the two directions
// is wrong.
//
// That is a much better check than asserting on values this implementation
// produced, because it fails if either half drifts.

#include <gtest/gtest.h>

#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"

#include <cstdint>
#include <limits>

using namespace torquebus;

namespace {

[[nodiscard]] CanSignal motorola(
    std::uint16_t startBit, std::uint16_t bitLength, bool isSigned, double factor, double offset)
{
    CanSignal signal;
    signal.startBit = startBit;
    signal.bitLength = bitLength;
    signal.byteOrder = ByteOrder::Motorola;
    signal.isSigned = isSigned;
    signal.factor = factor;
    signal.offset = offset;
    return signal;
}

[[nodiscard]] CanSignal
intel(std::uint16_t startBit, std::uint16_t bitLength, bool isSigned, double factor, double offset)
{
    CanSignal signal = motorola(startBit, bitLength, isSigned, factor, offset);
    signal.byteOrder = ByteOrder::Intel;
    return signal;
}

[[nodiscard]] bool near(double actual, double expected)
{
    return (actual - expected) < 1e-9 && (expected - actual) < 1e-9;
}

} // namespace

TEST(CanSignalEncodeTests, EncodingThePublishedValuesReproducesThePublishedBytes)
{
    std::uint8_t payload[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    EXPECT_TRUE(motorola(0, 12, true, 0.01, 250.0).encode(250.55, payload, 8));
    EXPECT_TRUE(motorola(6, 6, false, 0.1, 0.0).encode(3.2, payload, 8));
    EXPECT_TRUE(motorola(7, 1, false, 1.0, 0.0).encode(1.0, payload, 8));

    const std::uint8_t expected[8] = {0xC0, 0x06, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (std::size_t i = 0; i < 8; ++i) {
        SCOPED_TRACE(::testing::Message() << "byte " << i);
        EXPECT_TRUE(payload[i] == expected[i]);
    }
}

TEST(CanSignalEncodeTests, WritingOneSignalLeavesItsNeighboursAlone)
{
    // Two 4-bit codes sharing a byte. Writing whole bytes would be the simpler
    // implementation and would mean setting the gear always zeroes the mode.
    const CanSignal gear = intel(0, 4, false, 1.0, 0.0);
    const CanSignal mode = intel(4, 4, false, 1.0, 0.0);

    std::uint8_t payload[1] = {0x00};

    EXPECT_TRUE(gear.encode(3.0, payload, 1));
    EXPECT_TRUE(mode.encode(5.0, payload, 1));
    EXPECT_TRUE(payload[0] == 0x53);

    // And overwriting one does not disturb the other.
    EXPECT_TRUE(gear.encode(1.0, payload, 1));
    EXPECT_TRUE(payload[0] == 0x51);
    EXPECT_TRUE(near(mode.decode(payload, 1), 5.0));
}

TEST(CanSignalEncodeTests, EncodeAndDecodeAreInversesAcrossEveryCaseThatMatters)
{
    struct Case final {
        const char* what;
        CanSignal signal;
        double value;
    };

    const Case cases[] = {
        {"Motorola signed, not byte aligned", motorola(0, 12, true, 0.01, 250.0), 235.67},
        {"Motorola unsigned", motorola(6, 6, false, 0.1, 0.0), 4.3},
        {"Intel unsigned, two bytes", intel(0, 16, false, 0.1, 0.0), 6553.5},
        {"Intel signed with an offset", intel(8, 8, true, 1.0, -40.0), -87.0},
        {"a single bit", intel(3, 1, false, 1.0, 0.0), 1.0},
        {"a nibble", intel(4, 4, true, 1.0, 0.0), -8.0},
        {"the full 64 bits", intel(0, 64, true, 1.0, 0.0), -1.0},
    };

    for (const Case& test : cases) {
        SCOPED_TRACE(::testing::Message() << test.what);
        std::uint8_t payload[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        EXPECT_TRUE(test.signal.encode(test.value, payload, 8));
        EXPECT_TRUE(near(test.signal.decode(payload, 8), test.value));
    }
}

TEST(CanSignalEncodeTests, AValueTooLargeSaturatesAndSaysSo)
{
    // Saturating rather than wrapping, because a torque request of 300%
    // arriving as -56% is the kind of failure that moves an actuator. And
    // reported, because a value that did not fit is a mistake in the caller.
    const CanSignal byteSignal = intel(0, 8, false, 1.0, 0.0);
    std::uint8_t payload[1] = {0x00};

    EXPECT_FALSE(byteSignal.encode(300.0, payload, 1));
    EXPECT_TRUE(payload[0] == 0xFF);

    EXPECT_FALSE(byteSignal.encode(-5.0, payload, 1));
    EXPECT_TRUE(payload[0] == 0x00);

    // A signed signal saturates at its own limits, not at zero.
    const CanSignal signedSignal = intel(0, 8, true, 1.0, 0.0);
    EXPECT_FALSE(signedSignal.encode(-200.0, payload, 1));
    EXPECT_TRUE(signedSignal.rawValue(payload, 1) == -128);
}

TEST(CanSignalEncodeTests, RoundingHappensBeforeTheRangeTestNotAfter)
{
    // 255.4 on an 8-bit unsigned signal rounds to 255 and fits. Testing the
    // range first would reject a value the field can hold.
    const CanSignal byteSignal = intel(0, 8, false, 1.0, 0.0);
    std::uint8_t payload[1] = {0x00};

    EXPECT_TRUE(byteSignal.encode(255.4, payload, 1));
    EXPECT_TRUE(payload[0] == 0xFF);

    // Ordinary rounding, not truncation: 2.6 is 3, not 2.
    EXPECT_TRUE(byteSignal.encode(2.6, payload, 1));
    EXPECT_TRUE(payload[0] == 3);
}

TEST(CanSignalEncodeTests, TheDeclaredMinimumAndMaximumDoNotClamp)
{
    // Same rule as decode. The physical width of the field is a hard limit;
    // the database's declared range is documentation, and silently clamping to
    // it would hide the fault that produced the out-of-range value.
    CanSignal speed = intel(0, 16, false, 0.1, 0.0);
    speed.minimum = 0.0;
    speed.maximum = 250.0;

    std::uint8_t payload[2] = {0, 0};

    EXPECT_TRUE(speed.encode(400.0, payload, 2));
    EXPECT_TRUE(near(speed.decode(payload, 2), 400.0));
}

TEST(CanSignalEncodeTests, NaNDoesNotBecomeAnArbitraryNumber)
{
    // NaN fails every comparison, so a range test written the obvious way lets
    // it through to a cast whose result is undefined.
    const CanSignal byteSignal = intel(0, 8, false, 1.0, 0.0);
    std::uint8_t payload[1] = {0x42};

    EXPECT_FALSE(byteSignal.encode(std::numeric_limits<double>::quiet_NaN(), payload, 1));
    EXPECT_TRUE(payload[0] == 0x00);
}

TEST(CanSignalEncodeTests, EncodingIntoAPayloadThatIsTooShortWritesNothing)
{
    // The counterpart of decode returning zero rather than reading past the
    // end. Writing past the end would be worse: it corrupts memory rather than
    // inventing a value.
    const CanSignal speed = intel(0, 16, false, 0.1, 0.0);
    std::uint8_t payload[2] = {0xAA, 0xBB};

    EXPECT_FALSE(speed.encode(85.0, payload, 1));
    EXPECT_TRUE(payload[0] == 0xAA);
    EXPECT_TRUE(payload[1] == 0xBB);
}

TEST(CanSignalEncodeTests, AMessageHandsOutAFrameShapedLikeItself)
{
    CanMessage message;
    message.identifier = 0x18FEDF00;
    message.format = CanFrameFormat::Extended;
    message.length = 8;
    message.signalList.push_back(intel(0, 16, false, 0.125, 0.0));
    message.signalList.back().name = "EngineSpeed";

    CanFrame frame = message.makeFrame();

    EXPECT_TRUE(frame.identifier == 0x18FEDF00);
    EXPECT_TRUE(frame.format == CanFrameFormat::Extended);
    EXPECT_TRUE(frame.length == 8);
    EXPECT_TRUE(frame.direction == CanDirection::Tx);

    // Zero-filled, so a signal the caller does not set is something definite.
    for (std::size_t i = 0; i < frame.length; ++i) {
        EXPECT_TRUE(frame.data[i] == 0);
    }

    ASSERT_TRUE(message.findSignal("EngineSpeed") != nullptr);
    EXPECT_TRUE(message.findSignal("EngineSpeed")->encode(1500.0, frame.data.data(), frame.length));
    EXPECT_TRUE(near(message.findSignal("EngineSpeed")->decode(frame), 1500.0));
}
