// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Asking a bus who is on it.
//
// An ECU claims its address when it powers up, once. A measurement started on a machine that was
// already running therefore sees none of the claims, and every ECU it hears is an address with no
// NAME - the J1939 Network panel says "not seen claiming", and the fault list has a number where
// there could be "Engine". J1939-81 has the remedy for exactly that: a Request for Address Claimed,
// which every ECU answers by claiming again. This sends it.
//
// It is sent from the null address, because TorqueBus is not an ECU and has none to claim; that is
// the address J1939-81 gives a device that is on the bus without a place on it, and the J1939
// block does not list it as an ECU.
//
// It goes out only when asked for. A block that read a bus and also spoke on it unasked would be an
// analyser that changes what it analyses - and on a real vehicle, a tool that transmits without
// being told to is a tool nobody leaves connected.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

class CanEngine;

/// What asking came to.
struct J1939ClaimRequestOutcome final {
    /// The application channels the request went out on.
    std::vector<std::uint8_t> sent;

    /// A line for each channel that refused it - one that is not started, or one that is only
    /// listening - ready for the Output panel.
    std::vector<std::string> refused;
};

/// Asks every ECU on every channel of `engine` to claim its address again.
///
/// Every channel, and not the ones a J1939 block has been seen to read: an ECU that is asleep until
/// it is asked is on a bus that has said nothing, and a bus the block has not heard from yet is
/// exactly the one somebody wants to ask. A channel that is not a J1939 bus ignores a request it
/// cannot read.
///
/// Meant for a running measurement. On one that is not, every channel refuses.
[[nodiscard]] J1939ClaimRequestOutcome j1939RequestAddressClaims(CanEngine& engine);

} // namespace torquebus
