// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One signal inside a CAN message, and how to get its value out of eight bytes.
//
// This is the part of a database that has to be exactly right. A decoder that
// is off by one bit does not fail loudly - it shows a plausible number that is
// wrong, and the engineer reading it has no way to tell. So the extraction
// below is written against the DBC specification rather than inferred from a
// working example, and the tests check it against a published vector.
//
// The two byte orders are the whole difficulty:
//
//   Intel (little-endian, @1 in a .dbc)
//       The start bit is the signal's least significant bit. Bits run upwards
//       through the byte and then into the next byte.
//
//   Motorola (big-endian, @0 in a .dbc)
//       The start bit is the signal's MOST significant bit, numbered in the
//       same odd DBC scheme, and the value continues into *lower* bit numbers,
//       crossing into the next byte when the current one runs out.
//
// DBC numbers bits within a byte from 7 (first on the wire) down to 0, but
// numbers bytes upwards. Bit 8 is therefore the top bit of byte 1. Getting this
// wrong is the single most common defect in a homemade CAN decoder.

#pragma once

#include "core/can/CanFrame.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace torquebus {

enum class ByteOrder : std::uint8_t {
    Motorola, ///< `@0` in a .dbc. Big-endian.
    Intel     ///< `@1` in a .dbc. Little-endian.
};

/// A named value for a signal, from a `VAL_` entry.
///
/// What turns a 3 into "Reverse" in the trace. Kept with the signal rather than
/// in a side table, because a value description that outlives its signal is a
/// value description nobody can use.
struct SignalValueName final {
    std::int64_t value{};
    std::string name;

    [[nodiscard]] friend bool operator==(const SignalValueName&,
                                         const SignalValueName&) = default;
};

/// One signal definition, as a .dbc `SG_` line describes it.
struct CanSignal final {
    std::string name;

    /// DBC bit numbering. For Intel this is the least significant bit; for
    /// Motorola, the most significant.
    std::uint16_t startBit{0};

    std::uint16_t bitLength{1};

    ByteOrder byteOrder{ByteOrder::Intel};

    /// The raw value is two's complement.
    bool isSigned{false};

    /// physical = raw * factor + offset.
    double factor{1.0};
    double offset{0.0};

    /// The range the database declares. Carried for display and for spotting a
    /// decode that lands outside what the author expected; never used to clamp,
    /// because silently clamping a wrong value hides the fault that produced it.
    double minimum{0.0};
    double maximum{0.0};

    std::string unit;

    /// Nodes that receive this signal. Informational.
    std::vector<std::string> receivers;

    /// Value names, ordered by value.
    std::vector<SignalValueName> valueNames;

    /// The multiplexer switch for this message, `M` in a .dbc.
    bool isMultiplexer{false};

    /// Present only when the switch has this value, `m3` in a .dbc.
    std::optional<std::uint32_t> multiplexerValue;

    /// The name for `raw`, or empty when the signal has no value table entry
    /// covering it.
    [[nodiscard]] std::string_view nameForValue(std::int64_t raw) const noexcept;

    /// The raw bits, before factor and offset.
    ///
    /// Returns 0 when the signal does not fit in `payloadLength` bytes - a
    /// database and a bus that disagree about message length is a real and
    /// common situation, and reading past the payload would be the worse
    /// answer.
    [[nodiscard]] std::int64_t rawValue(const std::uint8_t* payload,
                                        std::size_t payloadLength) const noexcept;

    /// The physical value: `rawValue() * factor + offset`.
    [[nodiscard]] double decode(const std::uint8_t* payload,
                                std::size_t payloadLength) const noexcept;

    [[nodiscard]] double decode(const CanFrame& frame) const noexcept
    {
        return decode(frame.data.data(), frame.length);
    }

    /// Whether every bit this signal needs is inside a payload of that length.
    [[nodiscard]] bool fitsIn(std::size_t payloadLength) const noexcept;

    [[nodiscard]] friend bool operator==(const CanSignal&, const CanSignal&) = default;
};

} // namespace torquebus
