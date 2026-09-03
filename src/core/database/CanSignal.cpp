// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/database/CanSignal.h"

#include <algorithm>

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
