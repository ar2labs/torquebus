// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/database/CanSignal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace torquebus {
namespace {

/// Position of a DBC bit number within the payload.
///
/// A DBC bit number is simply `byte * 8 + bitInByte`, where bit 0 is the least
/// significant bit of its byte. So bit 8 is the least significant bit of byte 1
/// and bit 15 is the most significant bit of byte 1.
///
/// The confusing part is elsewhere: within a byte the bit that goes on the wire
/// *first* is bit 7, the most significant. That is what makes Motorola signals
/// run downwards through the numbering, and it is handled in bitNumberFor
/// rather than here - this function is only arithmetic.
struct BitPosition final {
    std::size_t byteIndex;
    std::uint8_t bitInByte; ///< 0 = least significant bit of that byte.
};

[[nodiscard]] constexpr BitPosition locate(std::uint16_t dbcBit) noexcept
{
    return BitPosition{dbcBit / 8U, static_cast<std::uint8_t>(dbcBit % 8U)};
}

[[nodiscard]] constexpr bool bitAt(const std::uint8_t* payload,
                                   std::size_t payloadLength,
                                   std::uint16_t dbcBit) noexcept
{
    const BitPosition position = locate(dbcBit);
    if (position.byteIndex >= payloadLength) {
        return false;
    }

    return ((payload[position.byteIndex] >> position.bitInByte) & 1U) != 0U;
}

/// DBC bit number for bit `index` (counting from the signal's least significant
/// bit) of a signal starting at `startBit` with `bitLength` bits.
[[nodiscard]] constexpr std::uint16_t bitNumberFor(std::uint16_t startBit,
                                                   std::uint16_t bitLength,
                                                   ByteOrder order,
                                                   std::uint16_t index) noexcept
{
    if (order == ByteOrder::Intel) {
        return static_cast<std::uint16_t>(startBit + index);
    }

    // Motorola. Work in "wire order": position 0 is bit 7 of byte 0, position 1
    // is bit 6 of byte 0, ... position 8 is bit 7 of byte 1. In that space the
    // signal is a contiguous run starting at its most significant bit, so the
    // arithmetic is ordinary addition - which is exactly why the conversion is
    // worth doing rather than reasoning in DBC numbering directly.
    const std::uint16_t msbWire =
        static_cast<std::uint16_t>((startBit / 8U) * 8U + (7U - (startBit % 8U)));

    // index counts from the least significant bit, which sits at the far end of
    // the run.
    const std::uint16_t wire =
        static_cast<std::uint16_t>(msbWire + (bitLength - 1U) - index);

    // Back to DBC numbering.
    return static_cast<std::uint16_t>((wire / 8U) * 8U + (7U - (wire % 8U)));
}

} // namespace

bool CanSignal::fitsIn(std::size_t payloadLength) const noexcept
{
    if (bitLength == 0) {
        return false;
    }

    const std::size_t bits = payloadLength * 8U;

    for (std::uint16_t index = 0; index < bitLength; ++index) {
        if (bitNumberFor(startBit, bitLength, byteOrder, index) >= bits) {
            return false;
        }
    }

    return true;
}

std::int64_t CanSignal::rawValue(const std::uint8_t* payload,
                                 std::size_t payloadLength) const noexcept
{
    if (payload == nullptr || bitLength == 0 || bitLength > 64) {
        return 0;
    }

    if (!fitsIn(payloadLength)) {
        // A database and a bus that disagree about message length is ordinary -
        // a shortened frame, a database from a different model year. Reading
        // past the payload would invent data; zero is at least honest, and the
        // decoder reports the mismatch separately.
        return 0;
    }

    std::uint64_t raw = 0;

    for (std::uint16_t index = 0; index < bitLength; ++index) {
        const std::uint16_t bit = bitNumberFor(startBit, bitLength, byteOrder, index);

        if (bitAt(payload, payloadLength, bit)) {
            raw |= (std::uint64_t{1} << index);
        }
    }

    if (!isSigned || bitLength == 64) {
        return static_cast<std::int64_t>(raw);
    }

    // Sign extension: if the top bit of the signal is set, fill the bits above
    // it. Done by hand rather than with a shift pair, because shifting by 64 is
    // undefined and a 64-bit signal would do exactly that.
    const std::uint64_t signBit = std::uint64_t{1} << (bitLength - 1U);
    if ((raw & signBit) != 0U) {
        const std::uint64_t mask = ~((std::uint64_t{1} << bitLength) - 1U);
        raw |= mask;
    }

    return static_cast<std::int64_t>(raw);
}

std::int64_t CanSignal::minimumRaw() const noexcept
{
    if (bitLength == 0 || bitLength > 64) {
        return 0;
    }
    if (!isSigned) {
        return 0;
    }
    if (bitLength == 64) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -(std::int64_t{1} << (bitLength - 1U));
}

std::int64_t CanSignal::maximumRaw() const noexcept
{
    if (bitLength == 0 || bitLength > 64) {
        return 0;
    }
    if (bitLength == 64) {
        return std::numeric_limits<std::int64_t>::max();
    }

    const std::uint16_t valueBits = isSigned ? static_cast<std::uint16_t>(bitLength - 1U)
                                             : bitLength;
    return static_cast<std::int64_t>((std::uint64_t{1} << valueBits) - 1U);
}

bool CanSignal::encodeRaw(std::int64_t raw,
                          std::uint8_t* payload,
                          std::size_t payloadLength) const noexcept
{
    if (payload == nullptr || bitLength == 0 || bitLength > 64 || !fitsIn(payloadLength)) {
        return false;
    }

    const auto bits = static_cast<std::uint64_t>(raw);

    for (std::uint16_t index = 0; index < bitLength; ++index) {
        const std::uint16_t bit = bitNumberFor(startBit, bitLength, byteOrder, index);
        const BitPosition position = locate(bit);

        // Clear this signal's bit, then set it. Never touches a bit that
        // belongs to a neighbouring signal.
        const auto mask = static_cast<std::uint8_t>(1U << position.bitInByte);
        payload[position.byteIndex] = static_cast<std::uint8_t>(payload[position.byteIndex] & ~mask);

        if (((bits >> index) & 1U) != 0U) {
            payload[position.byteIndex] =
                static_cast<std::uint8_t>(payload[position.byteIndex] | mask);
        }
    }

    return true;
}

bool CanSignal::encode(double physical,
                       std::uint8_t* payload,
                       std::size_t payloadLength) const noexcept
{
    if (factor == 0.0) {
        return false;
    }

    const double scaled = (physical - offset) / factor;

    // Round before the range test, not after. A value of 255.4 on an 8-bit
    // unsigned signal rounds to 255 and fits; testing first would reject it.
    const double rounded = std::round(scaled);

    const std::int64_t lowest = minimumRaw();
    const std::int64_t highest = maximumRaw();

    // Compared as doubles. Converting an out-of-range double to int64 is
    // undefined behaviour, and "the value did not fit" is exactly the case
    // where it would happen.
    //
    // The first test is written as a negated >= rather than a < so that NaN
    // takes this branch: NaN fails every comparison, and left to the cast it
    // would produce an arbitrary number.
    if (!(rounded >= static_cast<double>(lowest))) {
        (void)encodeRaw(lowest, payload, payloadLength);
        return false;
    }

    if (rounded > static_cast<double>(highest)) {
        (void)encodeRaw(highest, payload, payloadLength);
        return false;
    }

    return encodeRaw(static_cast<std::int64_t>(rounded), payload, payloadLength);
}

double CanSignal::decode(const std::uint8_t* payload, std::size_t payloadLength) const noexcept
{
    return static_cast<double>(rawValue(payload, payloadLength)) * factor + offset;
}

std::string_view CanSignal::nameForValue(std::int64_t raw) const noexcept
{
    const auto match = std::find_if(valueNames.begin(), valueNames.end(),
                                    [raw](const SignalValueName& entry) {
                                        return entry.value == raw;
                                    });

    return match == valueNames.end() ? std::string_view{} : std::string_view{match->name};
}

} // namespace torquebus
