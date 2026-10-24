// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Request.h"

#include "core/can/CanEngine.h"

#include <format>

namespace torquebus {

J1939RequestOutcome j1939RequestPgn(CanEngine& engine,
                                    std::uint32_t pgn,
                                    std::uint8_t destination,
                                    std::optional<std::uint8_t> channel)
{
    J1939RequestOutcome outcome;

    const auto ask = [&](std::uint8_t index) {
        const Result result =
            engine.transmit(index, j1939RequestFrame(pgn, destination, kJ1939NullAddress, index));

        if (result.succeeded()) {
            outcome.sent.push_back(index);
        } else {
            outcome.refused.push_back(
                std::format("CAN {}: {}", index + 1, std::string{result.message()}));
        }
    };

    if (channel.has_value()) {
        ask(*channel);
        return outcome;
    }

    for (std::size_t index = 0; index < engine.channelCount(); ++index) {
        ask(static_cast<std::uint8_t>(index));
    }

    return outcome;
}

J1939RequestOutcome j1939RequestAddressClaims(CanEngine& engine)
{
    return j1939RequestPgn(engine, kPgnAddressClaimed);
}

} // namespace torquebus
