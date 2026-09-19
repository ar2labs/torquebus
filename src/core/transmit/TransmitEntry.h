// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One line of a transmit list: a frame, and when to send it.
//
// The payload is the source of truth, not the signal values. An entry built
// from a database keeps the message and signal names so the panel can offer
// them back for editing, but what goes on the bus is `frame` - already encoded.
//
// That split matters at send time. Re-encoding from signal values on every
// transmission would mean a database lookup and a bit-packing pass per frame
// per period, on the executor thread, to produce bytes that have not changed
// since the user last typed. Encoding when the value is edited - at human
// speed - and sending a copy is the same result for a fraction of the work.
//
// It also makes the raw case and the database case one type. A user who types
// `1F4# 01 02 03` and a user who fills in `SpeedKmh = 85` end up with the same
// thing, which is what lets one list hold both.

#pragma once

#include "core/can/CanFrame.h"

#include <cstdint>
#include <string>

namespace torquebus {

/// How an entry decides when to go out.
enum class TransmitTrigger : std::uint8_t {
    /// Sent only when the user asks. The default, because a list that starts
    /// transmitting the moment it is filled in is a list that puts traffic on a
    /// vehicle nobody was ready for.
    Manual,

    /// Sent every `cycleMs` for as long as the measurement runs.
    Periodic
};

struct TransmitEntry final {
    /// What the user calls this line. Free text; the database's message name is
    /// the default when there is one, but a list often holds three variants of
    /// the same message and they need telling apart.
    std::string name;

    /// Exactly what goes on the bus.
    CanFrame frame;

    /// Application channel this row transmits on; 0 is CAN 1.
    ///
    /// On the entry rather than on the frame, even though CanFrame has a
    /// channel field, because this is a property of the *row* - where the user
    /// decided to send it - and the frame's channel is stamped from it at send
    /// time. Keeping the decision and its consequence in separate places is
    /// what lets the panel show a column the user can change.
    std::uint8_t channel{0};

    TransmitTrigger trigger{TransmitTrigger::Manual};

    /// Period in milliseconds. Ignored unless `trigger` is Periodic.
    ///
    /// Zero is treated as "as fast as the dispatch loop runs", which on a busy
    /// bus is not a period at all - so the list refuses it and keeps the last
    /// sane value. See TransmitList::kMinimumCycleMs.
    std::uint32_t cycleMs{100};

    /// Off without being deleted.
    ///
    /// The reason this is not just "remove the row": a transmit list is a test
    /// setup someone spent time building, and turning one line off to see what
    /// changes is the most common thing done with it.
    bool enabled{true};

    /// The database message this was built from, empty for a hand-typed frame.
    ///
    /// Kept so the panel can show the signal values again for editing. Not used
    /// when sending: `frame` already holds the result.
    std::string messageName;

    /// Set when the entry was last sent, in microseconds since the measurement
    /// started. Zero means never.
    std::uint64_t lastSentUs{0};

    /// How many times it has gone out. What tells a user at a glance that a
    /// periodic line is actually running.
    std::uint64_t sentCount{0};

    [[nodiscard]] bool isPeriodic() const noexcept { return trigger == TransmitTrigger::Periodic; }
};

} // namespace torquebus
