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

#include <string>
#include <vector>

namespace torquebus {

struct ClusterSource final {
    ClusterRole role{ClusterRole::Speed};
    DashboardBinding binding;
};

struct ClusterProfile final {
    /// Stable: written into project files, so a rename orphans every cluster anybody saved.
    std::string id;

    /// What the editor shows.
    std::string name;

    std::vector<ClusterSource> sources;

    /// The binding of `role`, or nullptr when the profile has no source for it.
    [[nodiscard]] const DashboardBinding* sourceOf(ClusterRole role) const noexcept
    {
        for (const ClusterSource& source : sources) {
            if (source.role == role) {
                return &source.binding;
            }
        }

        return nullptr;
    }
};

} // namespace torquebus
