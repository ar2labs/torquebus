// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Id.h"

namespace torquebus {
namespace {

/// What a Request carries: the PGN asked for, in three bytes.
constexpr std::uint8_t kRequestBytes = 3U;

} // namespace

std::optional<J1939Id> j1939Decompose(const CanFrame& frame) noexcept
{
    if (frame.format != CanFrameFormat::Extended) {
        return std::nullopt;
    }

    return j1939Decompose(frame.identifier);
}

CanFrame j1939RequestFrame(std::uint32_t requestedPgn,
                           std::uint8_t destination,
                           std::uint8_t source,
                           std::uint8_t channel) noexcept
{
    CanFrame frame{};
    frame.identifier = j1939Identifier(kPgnRequest, source, destination);
    frame.format = CanFrameFormat::Extended;
    frame.direction = CanDirection::Tx;
    frame.channel = channel;
    frame.dlc = kRequestBytes;
    frame.length = kRequestBytes;

    const std::uint32_t pgn = requestedPgn & kJ1939MaxPgn;
    frame.data[0] = static_cast<std::uint8_t>(pgn & 0xFFU);
    frame.data[1] = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
    frame.data[2] = static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU);

    return frame;
}

std::optional<std::uint32_t> j1939RequestedPgn(const CanFrame& frame) noexcept
{
    const std::optional<J1939Id> id = j1939Decompose(frame);
    if (!id.has_value() || id->pgn() != kPgnRequest || frame.payloadSize() < kRequestBytes) {
        return std::nullopt;
    }

    // The top six bits of the last byte are not part of a PGN, whatever a sender put in them.
    return static_cast<std::uint32_t>(frame.data[0])
           | (static_cast<std::uint32_t>(frame.data[1]) << 8U)
           | ((static_cast<std::uint32_t>(frame.data[2]) & 0x03U) << 16U);
}

} // namespace torquebus
