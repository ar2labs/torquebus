// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939ClaimRequest.h"

#include "core/can/CanEngine.h"
#include "core/j1939/J1939Id.h"

#include <format>

namespace torquebus {

J1939ClaimRequestOutcome j1939RequestAddressClaims(CanEngine& engine)
{
    J1939ClaimRequestOutcome outcome;

    for (std::size_t index = 0; index < engine.channelCount(); ++index) {
        const auto channel = static_cast<std::uint8_t>(index);

        const Result result = engine.transmit(
            channel,
            j1939RequestFrame(kPgnAddressClaimed, kJ1939GlobalAddress, kJ1939NullAddress, channel));

        if (result.succeeded()) {
            outcome.sent.push_back(channel);
        } else {
            outcome.refused.push_back(
                std::format("CAN {}: {}", index + 1, std::string{result.message()}));
        }
    }

    return outcome;
}

} // namespace torquebus
