// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Id.h"

namespace torquebus {
namespace {

/// What a Request carries: the PGN asked for, in three bytes.
constexpr std::uint8_t kRequestBytes = 3U;

/// What an Acknowledgement carries: control, group function, two bytes not used, the address it
/// answers and the PGN in three.
constexpr std::uint8_t kAcknowledgementBytes = 8U;

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

CanFrame j1939AcknowledgementFrame(const J1939Acknowledgement& acknowledgement,
                                   std::uint8_t source,
                                   std::uint8_t destination,
                                   std::uint8_t channel) noexcept
{
    CanFrame frame{};
    frame.identifier = j1939Identifier(kPgnAcknowledgement, source, destination);
    frame.format = CanFrameFormat::Extended;
    frame.direction = CanDirection::Tx;
    frame.channel = channel;
    frame.dlc = kAcknowledgementBytes;
    frame.length = kAcknowledgementBytes;

    const std::uint32_t pgn = acknowledgement.pgn & kJ1939MaxPgn;
    frame.data[0] = acknowledgement.control;
    frame.data[1] = acknowledgement.groupFunction;
    frame.data[2] = 0xFFU;
    frame.data[3] = 0xFFU;
    frame.data[4] = acknowledgement.address;
    frame.data[5] = static_cast<std::uint8_t>(pgn & 0xFFU);
    frame.data[6] = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
    frame.data[7] = static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU);

    return frame;
}

std::optional<J1939Acknowledgement> j1939Acknowledgement(const CanFrame& frame) noexcept
{
    const std::optional<J1939Id> id = j1939Decompose(frame);
    if (!id.has_value() || id->pgn() != kPgnAcknowledgement
        || frame.payloadSize() < kAcknowledgementBytes) {
        return std::nullopt;
    }

    J1939Acknowledgement acknowledgement;
    acknowledgement.control = frame.data[0];
    acknowledgement.groupFunction = frame.data[1];
    acknowledgement.address = frame.data[4];

    // The same three bytes a Request names a PGN in, and the same six reserved bits above it.
    acknowledgement.pgn = static_cast<std::uint32_t>(frame.data[5])
                          | (static_cast<std::uint32_t>(frame.data[6]) << 8U)
                          | ((static_cast<std::uint32_t>(frame.data[7]) & 0x03U) << 16U);

    return acknowledgement;
}

} // namespace torquebus
