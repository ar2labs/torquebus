// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Name.h"

namespace torquebus {
namespace {

/// True when the frame is an Address Claimed carrying a full NAME.
[[nodiscard]] bool isWellFormedClaim(const CanFrame& frame) noexcept
{
    const std::optional<J1939Id> id = j1939Decompose(frame);

    return id.has_value() && id->pgn() == kPgnAddressClaimed
           && frame.length >= kJ1939NameBytes;
}

} // namespace

std::uint64_t j1939NameBits(const CanFrame& frame) noexcept
{
    std::uint64_t name = 0U;

    // Least significant byte first: data[0] is bits 0..7, data[7] is 56..63.
    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        name |= static_cast<std::uint64_t>(frame.data[index]) << (index * 8U);
    }

    return name;
}

std::optional<J1939Name> j1939NameFromClaim(const CanFrame& frame) noexcept
{
    if (!isWellFormedClaim(frame)) {
        return std::nullopt;
    }

    return j1939DecodeName(j1939NameBits(frame));
}

bool j1939IsCannotClaimAddress(const CanFrame& frame) noexcept
{
    const std::optional<J1939Id> id = j1939Decompose(frame);

    return id.has_value() && id->pgn() == kPgnAddressClaimed
           && id->sourceAddress == kJ1939NullAddress;
}

std::string_view j1939IndustryGroupName(std::uint8_t industryGroup) noexcept
{
    switch (industryGroup & 0x07U) {
    case 0U: return "Global";
    case 1U: return "On-Highway Equipment";
    case 2U: return "Agricultural and Forestry Equipment";
    case 3U: return "Construction Equipment";
    case 4U: return "Marine";
    case 5U: return "Industrial-Process Control";
    default: break;
    }

    // Six and seven are reserved. Named as reserved rather than left blank: a
    // blank cell reads as a decoding failure, and this one decoded fine.
    return "Reserved";
}

} // namespace torquebus
