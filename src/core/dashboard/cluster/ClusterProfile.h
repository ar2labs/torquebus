// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where each role of an instrument cluster gets its value.
//
// A profile is plain data: for some of the roles, a binding. It reuses the Dashboard's own
// DashboardBinding - a decoded CAN signal by message and signal name, or a system variable - so the
// cluster reads exactly what a Gauge on the same panel would read, and a signal that the Graph can
// plot is a signal the cluster can show. A second way of naming a value would be a second way for
// the two to disagree.
//
// A role the profile does not mention has no source. That is not a mistake: the cluster draws
// dashes for it and lights up the day a source arrives, which is how one cluster serves a vehicle
// that reports a lot and one that reports a little.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterRole.h"

#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

/// How long a bus signal may go without a new sample before the cluster stops showing it. Two
/// seconds: a signal sent every 100 ms has missed twenty, and a person looking at a speedometer has
/// long since stopped believing it.
inline constexpr std::uint32_t kClusterDefaultMaxAgeMs = 2000;

struct ClusterSource final {
    ClusterRole role{ClusterRole::Speed};
    DashboardBinding binding;

    /// For a CAN signal: how long without a new sample before the role is no data again, as it is
    /// for a signal never seen - the cluster draws dashes rather than the last number the bus
    /// sent. The store keeps that number for ever, which is right for a plot and wrong for an
    /// instrument.
    ///
    /// A profile sets this per role because messages are sent at their own rates: about three
    /// times the message's cycle time, and never less than the default. **Zero means never**, which
    /// is what a message sent only when something changes needs - a lamp that is lit stays lit
    /// through the silence. A variable has no age at all: one written once is a steady value.
    std::uint32_t maxAgeMs{kClusterDefaultMaxAgeMs};
};

struct ClusterProfile final {
    /// Stable: written into project files, so a rename orphans every cluster anybody saved.
    std::string id;

    /// What the editor shows.
    std::string name;

    std::vector<ClusterSource> sources;

    /// The source of `role`, or nullptr when the profile has none for it.
    [[nodiscard]] const ClusterSource* sourceOf(ClusterRole role) const noexcept
    {
        for (const ClusterSource& source : sources) {
            if (source.role == role) {
                return &source;
            }
        }

        return nullptr;
    }
};

} // namespace torquebus
