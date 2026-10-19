// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/DashboardFaults.h"

#include <algorithm>

namespace torquebus {
namespace {

/// Danger first, then Warning, then the rest: the order a person reads a list in when it is cut
/// short.
[[nodiscard]] int severityRank(DashboardTone tone) noexcept
{
    switch (tone) {
    case DashboardTone::Danger:
        return 0;
    case DashboardTone::Warning:
        return 1;
    case DashboardTone::Neutral:
    case DashboardTone::Ok:
    case DashboardTone::Info:
    case DashboardTone::Off:
        break;
    }

    return 2;
}

} // namespace

DashboardTone toneOfLamps(const J1939Lamps& lamps) noexcept
{
    const auto lit = [](J1939LampState state) { return state == J1939LampState::On; };

    if (lit(lamps.redStop)) {
        return DashboardTone::Danger;
    }

    if (lit(lamps.amberWarning) || lit(lamps.malfunction) || lit(lamps.protect)) {
        return DashboardTone::Warning;
    }

    return DashboardTone::Neutral;
}

std::vector<DashboardFault> dashboardFaultsOf(const J1939NetworkSnapshot& snapshot,
                                              const J1939NameTables* names)
{
    std::vector<DashboardFault> faults;
    faults.reserve(snapshot.activeFaults.size());

    for (const J1939ActiveFault& active : snapshot.activeFaults) {
        DashboardFault fault;
        fault.sourceAddress = active.sourceAddress;
        fault.code = active.code;
        fault.tone = toneOfLamps(active.lamps);

        // The NAME that claimed the address, if one was seen to: it is the only thing that says
        // what the ECU is, and an ECU that never claimed one in this measurement is an address.
        if (names != nullptr) {
            const auto node = std::find_if(snapshot.nodes.begin(),
                                           snapshot.nodes.end(),
                                           [&active](const J1939NetworkNode& candidate) {
                                               return candidate.address == active.sourceAddress;
                                           });

            if (node != snapshot.nodes.end() && node->name.has_value()) {
                if (const std::optional<std::string_view> function = names->function(*node->name);
                    function.has_value()) {
                    fault.ecu = std::string{*function};
                }
            }
        }

        faults.push_back(std::move(fault));
    }

    // Stable, so that two codes of one rank keep the order the snapshot gave them.
    std::stable_sort(
        faults.begin(), faults.end(), [](const DashboardFault& left, const DashboardFault& right) {
            return severityRank(left.tone) < severityRank(right.tone);
        });

    return faults;
}

} // namespace torquebus
