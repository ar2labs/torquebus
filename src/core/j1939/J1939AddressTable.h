// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Who is on this bus.
//
// It is the first question somebody has when they connect to a machine they did
// not build, and on J1939 it has a real answer: every ECU announces a NAME and
// argues for an address at power-up, and the argument is public.
//
// --- This watches. It does not claim ----------------------------------------
//
// TorqueBus does not claim an address by default, and this file cannot claim
// one at all - it has no way to transmit. A tool that claimed an address on
// being switched on could knock a real ECU off the bus that was using it: on a
// bench that is an afternoon, in a vehicle it is worse. Claiming will be an
// explicit option with a NAME typed in by somebody who knows what they are
// doing, and it will never be what happens to a person who opened the program
// to look.
//
// --- What "never claimed" honestly means -------------------------------------
//
// Claims happen at power-up. A measurement started on a machine that has been
// running for an hour sees none of them, and every ECU on it will show here as
// transmitting from an address it was never seen claiming. That is not a fault
// and this file must not imply it is one: the flag says what was observed
// *since the measurement started*, which is a different sentence from "this ECU
// never claimed its address".
//
// The way to turn one into the other is to ask - a Request for Address Claimed,
// which every ECU answers. That is a transmission, so it belongs with the
// explicit opt-in above rather than here.
//
// What this does catch, and what makes it worth having: an address that starts
// transmitting after a claim was already seen for it, a second NAME arriving on
// an address somebody else holds, and an ECU announcing that it could not get
// an address at all.

#pragma once

#include "core/can/CanFrame.h"
#include "core/j1939/J1939Name.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace torquebus {

/// Highest address an ECU can actually occupy. 254 is the null address and 255
/// is the global one; neither is a seat.
inline constexpr std::uint8_t kJ1939MaxNodeAddress = 253U;

/// One address, and what has been seen of whoever is using it.
struct J1939NetworkNode final {
    std::uint8_t address{0U};

    /// The NAME that claimed this address, when a claim was seen.
    std::optional<J1939Name> name;

    /// A claim for this address was seen since the measurement started. Read
    /// the note at the top of this file before showing this as "unclaimed".
    bool claimSeen{false};

    /// This address has carried ordinary traffic, not only claims.
    bool trafficSeen{false};

    std::uint64_t firstSeenNs{0U};
    std::uint64_t lastSeenNs{0U};

    /// Frames seen from this address, claims included. A rough measure of how
    /// busy an ECU is, and enough to tell a chatty node from a silent one.
    std::uint32_t framesSeen{0U};
};

/// An ECU that announced it could not get an address.
struct J1939Defeated final {
    J1939Name name;
    std::uint64_t firstSeenNs{0U};
    std::uint64_t lastSeenNs{0U};
    std::uint32_t announcements{0U};
};

/// Something about the membership of the bus changed.
struct J1939NetworkEvent final {
    enum class Kind : std::uint8_t {
        /// A claim was seen for an address nobody was holding.
        AddressClaimed,

        /// A second NAME claimed an address somebody else holds. `name` is the
        /// claimant; `previousName` is who held it.
        AddressContested,

        /// A contest changed the occupant: the arriving NAME was lower.
        AddressTaken,

        /// An ECU announced from the null address that it has none.
        CannotClaim,

        /// Traffic from an address no claim has been seen for. Reported once
        /// per address - see the note at the top of this file about what this
        /// does and does not mean.
        UnclaimedTraffic,
    };

    Kind kind{Kind::AddressClaimed};
    std::uint8_t address{0U};

    /// The NAME the event is about. Absent for UnclaimedTraffic, which is an
    /// event about not knowing one.
    std::optional<J1939Name> name;

    /// Who held the address before, for a contest.
    std::optional<J1939Name> previousName;

    std::uint64_t timestampNs{0U};
};

/// The membership of one bus, built from what went past.
class J1939AddressTable final {
public:
    /// Offers a frame. Returns true when it was a 29-bit frame, which is the
    /// only kind this table learns anything from.
    bool onFrame(const CanFrame& frame, std::uint64_t nowNs);

    /// Every address seen, in ascending order - which is also the order a panel
    /// wants, and the reason these are kept in a sorted vector rather than a
    /// map the caller would have to copy out of.
    [[nodiscard]] std::span<const J1939NetworkNode> nodes() const noexcept { return m_nodes; }

    /// The ECUs that announced they could not get an address. Kept apart from
    /// the table on purpose: 254 is not a seat, and listing them as occupants
    /// of it would put a phantom ECU at an address that does not exist.
    [[nodiscard]] std::span<const J1939Defeated> defeated() const noexcept { return m_defeated; }

    [[nodiscard]] const J1939NetworkNode* find(std::uint8_t address) const noexcept;

    [[nodiscard]] std::span<const J1939NetworkEvent> events() const noexcept { return m_events; }

    void clearEvents() { m_events.clear(); }

    /// Forgets the bus. For a measurement starting, because the membership of a
    /// bus is a fact about the run and not about the tool.
    void reset();

private:
    [[nodiscard]] J1939NetworkNode& nodeFor(std::uint8_t address, std::uint64_t nowNs);

    void onClaim(std::uint8_t address, const J1939Name& name, std::uint64_t nowNs);
    void onCannotClaim(const J1939Name& name, std::uint64_t nowNs);

    std::vector<J1939NetworkNode> m_nodes;
    std::vector<J1939Defeated> m_defeated;
    std::vector<J1939NetworkEvent> m_events;
};

} // namespace torquebus
