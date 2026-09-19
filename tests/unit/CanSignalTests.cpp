// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Signal extraction is the one place in this project where being wrong is
// silent. A decoder off by one bit does not crash or log anything - it shows a
// plausible number, and the engineer reading it has no way to tell. So these
// tests are anchored to vectors published by someone else rather than to
// values this implementation produced.
//
// The anchor is `motohawk.dbc`, the example database cantools ships, whose
// documentation states that encoding
//
//     Temperature = 250.55, AverageRadius = 3.2, Enable = 1
//
// produces the bytes C0 06 E0 00 00 00 00 00. Decoding those bytes has to give
// those three values back. All three are Motorola, none is byte-aligned, and
// one is signed - which between them exercise every part of the numbering that
// is easy to get wrong.

#include <catch2/catch_test_macros.hpp>

#include "core/database/CanSignal.h"

#include <cstdint>

using namespace torquebus;

namespace {

/// The published motohawk vector.
constexpr std::uint8_t kMotohawk[8] = {0xC0, 0x06, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};

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

/// Compares without pulling in Catch2's Approx, so this file has one dependency
/// fewer and the tolerance is stated where it is used.
[[nodiscard]] bool near(double actual, double expected)
{
    return (actual - expected) < 1e-9 && (expected - actual) < 1e-9;
}

} // namespace

TEST_CASE("The published motohawk vector decodes to its published values", "[dbc][signal]")
{
    // SG_ Temperature : 0|12@0- (0.01,250)
    const CanSignal temperature = motorola(0, 12, true, 0.01, 250.0);
    CHECK(near(temperature.decode(kMotohawk, 8), 250.55));

    // SG_ AverageRadius : 6|6@0+ (0.1,0)
    const CanSignal radius = motorola(6, 6, false, 0.1, 0.0);
    CHECK(near(radius.decode(kMotohawk, 8), 3.2));

    // SG_ Enable : 7|1@0+ (1,0)
    const CanSignal enable = motorola(7, 1, false, 1.0, 0.0);
    CHECK(near(enable.decode(kMotohawk, 8), 1.0));
}

TEST_CASE("Intel signals read upwards through the bytes", "[dbc][signal]")
{
    // vehicle.dbc: SG_ SpeedKmh : 0|16@1+ (0.1,0). 85.0 km/h is raw 850, which
    // little-endian puts on the wire as 52 03.
    const std::uint8_t payload[2] = {0x52, 0x03};
    const CanSignal speed = intel(0, 16, false, 0.1, 0.0);

    CHECK(speed.rawValue(payload, 2) == 850);
    CHECK(near(speed.decode(payload, 2), 85.0));
}

TEST_CASE("An offset moves the whole range", "[dbc][signal]")
{
    // vehicle.dbc: SG_ EngTemp : 0|8@1+ (1,-40) - the classic temperature
    // encoding, where an unsigned byte covers -40 to 215.
    const std::uint8_t payload[1] = {0x6E};
    const CanSignal temperature = intel(0, 8, false, 1.0, -40.0);

    CHECK(near(temperature.decode(payload, 1), 70.0));
}

TEST_CASE("A signed signal is sign extended, not read as unsigned", "[dbc][signal]")
{
    const std::uint8_t allOnes[1] = {0xFF};

    CHECK(intel(0, 8, true, 1.0, 0.0).rawValue(allOnes, 1) == -1);
    CHECK(intel(0, 8, false, 1.0, 0.0).rawValue(allOnes, 1) == 255);

    // A width that is not a whole byte is where a shift-based sign extension
    // usually goes wrong: 4 bits of 1111 is -1, not 15.
    const std::uint8_t nibble[1] = {0x0F};
    CHECK(intel(0, 4, true, 1.0, 0.0).rawValue(nibble, 1) == -1);
}

TEST_CASE("A 64-bit signal does not shift by 64", "[dbc][signal]")
{
    // Shifting a 64-bit value by 64 is undefined, and the sign-extension path
    // is where that would happen. The value here has its top bit set, so a
    // signed 64-bit read exercises exactly that branch.
    const std::uint8_t payload[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    CHECK(intel(0, 64, true, 1.0, 0.0).rawValue(payload, 8) == -1);
}

TEST_CASE("A signal that does not fit the payload reads zero", "[dbc][signal]")
{
    // A database and a bus that disagree about message length is ordinary: a
    // shortened frame, or a database from a different model year. Reading past
    // the payload would invent data.
    const std::uint8_t payload[2] = {0x52, 0x03};
    const CanSignal speed = intel(0, 16, false, 0.1, 0.0);

    CHECK_FALSE(speed.fitsIn(1));
    CHECK(speed.rawValue(payload, 1) == 0);

    CHECK(speed.fitsIn(2));
}

TEST_CASE("A Motorola signal spans the bytes its numbering implies, not the ones "
          "that look obvious",
          "[dbc][signal]")
{
    // Twelve bits reaching into the *third* byte, which is the counterintuitive
    // result and the reason this case is here.
    //
    // Temperature starts at DBC bit 0. In Motorola order the start bit is the
    // signal's most significant bit, and DBC bit 0 is the LAST bit of byte 0 to
    // go on the wire. So the signal runs from there forwards: the rest of byte
    // 0 (one bit), all of byte 1 (eight), and the first three bits of byte 2.
    //
    // Note that 12 bits from bit 0 touches three bytes rather than two:
    // the published reference vector decodes to
    // 250.55 only when byte 2 is read.
    const CanSignal temperature = motorola(0, 12, true, 0.01, 250.0);

    CHECK_FALSE(temperature.fitsIn(1));
    CHECK_FALSE(temperature.fitsIn(2));
    CHECK(temperature.fitsIn(3));

    // And the value confirms it: truncating to two bytes loses the low bits, so
    // this is not a bound that could be relaxed without changing the answer.
    CHECK(temperature.rawValue(kMotohawk, 8) == 55);
    CHECK(temperature.rawValue(kMotohawk, 2) == 0);
}

TEST_CASE("Value names turn a raw number into a word", "[dbc][signal]")
{
    CanSignal gear = intel(0, 4, false, 1.0, 0.0);
    gear.valueNames = {{0, "Neutral"}, {1, "Drive"}, {2, "Reverse"}};

    CHECK(gear.nameForValue(2) == "Reverse");
    CHECK(gear.nameForValue(0) == "Neutral");

    // An unlisted value gets nothing rather than a guess: a gear position the
    // database does not describe is exactly the case an engineer needs to see
    // as a raw number.
    CHECK(gear.nameForValue(7).empty());
}
