// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Asking a bus, or an ECU on it, for something.
//
// J1939 has one way to ask for a parameter group that is not being sent: a Request (PGN 59904),
// three bytes naming the PGN, to everyone or to one ECU. What an ECU answers is the group itself;
// when the request was to it and it has nothing of that name it answers with a negative
// Acknowledgement, and it is silent to a request to everyone it cannot meet.
//
// The first thing worth asking for is the claim. An ECU claims its address once, at power-up, so a
// measurement started on a machine that was already running sees none of the claims, and every ECU
// it hears is an address with no NAME - the J1939 Network panel says "not seen claiming", and the
// fault list has a number where there could be "Engine". J1939-81 has the remedy for exactly that:
// a Request for Address Claimed, which every ECU answers by claiming again.
//
// The next are the groups a service tool reads: the codes that were active (DM2), what the ECU, its
// software and its components are called. They are asked for the same way and answered the same
// way, and the answer is whatever the ECU has: a DM2 reaches the J1939 block, which files it under
// "previously active", and an identification is a frame in the trace for a database to describe.
//
// A request is sent from the null address, because TorqueBus is not an ECU and has none to claim;
// that is the address J1939-81 gives a device that is on the bus without a place on it, and an ECU
// answers a request from it to everyone, which a device with no address can still hear. The J1939
// block does not list it as an ECU.
//
// It goes out only when asked for. A block that read a bus and also spoke on it unasked would be an
// analyser that changes what it analyses - and on a real vehicle, a tool that transmits without
// being told to is a tool nobody leaves connected.

#pragma once

#include "core/j1939/J1939Id.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace torquebus {

class CanEngine;

/// What asking came to.
struct J1939RequestOutcome final {
    /// The application channels the request went out on.
    std::vector<std::uint8_t> sent;

    /// A line for each channel that refused it - one that is not started, or one that is only
    /// listening - ready for the Output panel.
    std::vector<std::string> refused;
};

/// Asks `destination` - every ECU, or one - for `pgn`, on `channel` or on every channel of
/// `engine`.
///
/// Every channel when none is named, and not the ones a J1939 block has been seen to read: an ECU
/// that is asleep until it is asked is on a bus that has said nothing, and a bus the block has not
/// heard from yet is exactly the one somebody wants to ask. A channel that is not a J1939 bus
/// ignores a request it cannot read.
///
/// Meant for a running measurement. On one that is not, every channel refuses.
[[nodiscard]] J1939RequestOutcome
j1939RequestPgn(CanEngine& engine,
                std::uint32_t pgn,
                std::uint8_t destination = kJ1939GlobalAddress,
                std::optional<std::uint8_t> channel = std::nullopt);

/// Asks every ECU on every channel to claim its address again.
[[nodiscard]] J1939RequestOutcome j1939RequestAddressClaims(CanEngine& engine);

} // namespace torquebus
