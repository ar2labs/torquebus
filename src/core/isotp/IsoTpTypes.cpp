// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/isotp/IsoTpTypes.h"

#include <array>

namespace torquebus {
namespace {

/// The 29-bit address forms ISO 15765-2 defines.
///
/// 0x18DA<target><source> physical, 0x18DB<target><source> functional. The
/// 0x18 is priority 6 with the data page and extended data page clear, which is
/// what every heavy-vehicle tester on the market sends.
constexpr std::uint32_t kPhysicalBase = 0x18DA'0000U;
constexpr std::uint32_t kFunctionalBase = 0x18DB'0000U;

[[nodiscard]] constexpr std::uint32_t normalFixedId(std::uint32_t base,
                                                    std::uint8_t target,
                                                    std::uint8_t source) noexcept
{
    return base | (static_cast<std::uint32_t>(target) << 8U) | source;
}

} // namespace

IsoTpAddress IsoTpAddress::normalFixed(std::uint8_t source,
                                       std::uint8_t target,
                                       std::uint8_t channel)
{
    IsoTpAddress address;
    address.addressing = IsoTpAddressing::NormalFixed;
    address.format = CanFrameFormat::Extended;
    address.transmitId = normalFixedId(kPhysicalBase, target, source);

    // The reply comes back with the addresses swapped: the ECU is now the
    // source and the tester the target.
    address.receiveId = normalFixedId(kPhysicalBase, source, target);
    address.channel = channel;

    return address;
}

IsoTpAddress IsoTpAddress::normalFixedFunctional(std::uint8_t source,
                                                 std::uint8_t target,
                                                 std::uint8_t channel)
{
    IsoTpAddress address = normalFixed(source, target, channel);

    // Out functionally, back physically. A functional request is heard by every
    // ECU on the bus; each one that answers does so as itself, which is the
    // whole point of asking that way.
    address.transmitId = normalFixedId(kFunctionalBase, target, source);

    return address;
}

IsoTpAddress IsoTpAddress::obd(std::uint8_t ecu, std::uint8_t channel)
{
    IsoTpAddress address;
    address.addressing = IsoTpAddressing::Normal;
    address.format = CanFrameFormat::Standard;

    // 0x7E0..0x7E7 out, 0x7E8..0x7EF back. Legislated for emissions
    // diagnostics, and by now the default assumption of every generic tool.
    address.transmitId = 0x7E0U + (ecu & 0x07U);
    address.receiveId = 0x7E8U + (ecu & 0x07U);
    address.channel = channel;

    return address;
}

std::string_view describe(IsoTpError error) noexcept
{
    switch (error) {
    case IsoTpError::None:
        return "no error";
    case IsoTpError::FlowControlTimeout:
        return "no flow control arrived - the ECU may not be there, or may be "
               "listening on another identifier";
    case IsoTpError::ConsecutiveTimeout:
        return "the message stopped part way through";
    case IsoTpError::SequenceError:
        return "consecutive frames arrived out of order - two senders on one "
               "identifier?";
    case IsoTpError::Overflow:
        return "the receiver has no buffer big enough for this message";
    case IsoTpError::TooManyWaitFrames:
        return "the receiver kept asking to wait";
    case IsoTpError::InvalidFlowStatus:
        return "the receiver sent a flow status this build does not understand";
    case IsoTpError::TooLong:
        return "the message is longer than this connection will assemble";
    case IsoTpError::ProtocolError:
        return "a frame arrived that cannot belong to any transfer";
    }

    return "unknown error";
}

std::uint32_t separationMicroseconds(std::uint8_t separationTime) noexcept
{
    if (separationTime <= 0x7FU) {
        return static_cast<std::uint32_t>(separationTime) * 1000U;
    }

    if (separationTime >= 0xF1U && separationTime <= 0xF9U) {
        return static_cast<std::uint32_t>(separationTime - 0xF0U) * 100U;
    }

    // Reserved. The standard says to use the slowest legal separation rather
    // than to guess - an unknown value means the other end is speaking a
    // dialect this one does not know.
    return 127U * 1000U;
}

std::size_t fdFrameLength(std::size_t length) noexcept
{
    static constexpr std::array<std::size_t, 8> kLengths{8, 12, 16, 20, 24, 32, 48, 64};

    for (const std::size_t candidate : kLengths) {
        if (length <= candidate) {
            return candidate;
        }
    }

    return kMaxCanPayload;
}

} // namespace torquebus
