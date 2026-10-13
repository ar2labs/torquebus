// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// From one block per channel access to one block per bus.
//
// Before the CAN Bus block, a module reached a channel through two blocks of its
// own: a CAN Channel wired to its input, a CAN Transmit wired from its output.
// Six ECUs on one bus were twelve wires and a CAN Channel the six of them hung
// from, which is what a wiring diagram of a bus looks like only to the person
// who has not seen one - a bus is a line, and every ECU is one tap on it.
//
// This rewrites the old wiring as the new, without changing what runs. The CAN
// Bus builds into exactly the nodes the blocks it replaces were: its channel is
// read by one CAN Channel, and every wire that transmits on it ends in a CAN
// Transmit of its own. A project that is grouped behaves, frame for frame, as it
// did before - which is what makes offering it safe, and why it is an action
// somebody takes and not something a project undergoes when it is opened.

#pragma once

#include "core/pipeline/GraphDescription.h"

#include <cstddef>

namespace torquebus {

/// What groupOnBuses changed.
struct BusGrouping final {
    /// CAN Bus blocks the graph has gained, one per channel that was wired.
    std::size_t busesAdded{0};

    /// CAN Channel and CAN Transmit blocks that were only a way onto a bus, and
    /// that the bus now is.
    std::size_t blocksRemoved{0};

    /// Wires that now end at a bus where they ended at one of those blocks.
    std::size_t wiresMoved{0};

    [[nodiscard]] bool changed() const noexcept { return wiresMoved != 0; }
};

/// True when the graph reaches a channel through CAN Channel or CAN Transmit
/// blocks that groupOnBuses would replace. What a canvas asks before it offers
/// the action.
[[nodiscard]] bool canGroupOnBuses(const GraphDescription& graph);

/// Replaces the wiring of CAN Channel and CAN Transmit blocks with a CAN Bus per
/// channel.
///
/// A block is replaced when it is switched on, wired to something, and not wired
/// to another one of them: a CAN Channel feeding a CAN Transmit on another
/// channel is a gateway, which is something a pair of blocks says and a bus does
/// not, so both are left as they are. The bus takes over the id and the place of
/// the first CAN Channel it replaces, so the project's wires and its diff change
/// as little as they can; a channel with a CAN Bus already on it gets that one.
///
/// Wires keep their frame format, and a module that had two CAN Transmit blocks
/// fed by one output ends up with one wire to the bus rather than two.
BusGrouping groupOnBuses(GraphDescription& graph);

} // namespace torquebus
