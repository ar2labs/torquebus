// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/cluster/ClusterProfiles.h"

#include <algorithm>
#include <utility>

namespace torquebus {
namespace {

[[nodiscard]] ClusterSource signalSource(ClusterRole role, std::string message, std::string signal)
{
    ClusterSource source;
    source.role = role;
    source.binding.source = DashboardBinding::Source::Signal;
    source.binding.message = std::move(message);
    source.binding.signal = std::move(signal);
    return source;
}

/// The 11-bit vehicle of examples/projects/virtual-vehicle.tbsproj, as its two databases describe
/// it: examples/databases/vehicle.dbc and ecu.dbc.
///
/// Only roles with a signal in those databases are here. The rest - gear, fuel, lamps and so on -
/// have no source, and the cluster shows dashes for them until a profile that has one is chosen.
/// ClusterProfilesTests checks every name below against the two files, so a rename in a database
/// cannot leave the cluster reading a signal that is gone.
[[nodiscard]] ClusterProfile example11Bit()
{
    ClusterProfile profile;
    profile.id = std::string{kDefaultClusterProfile};
    profile.name = "Example vehicle (11-bit)";

    profile.sources = {
        signalSource(ClusterRole::Speed, "VehicleSpeed", "SpeedKmh"),
        signalSource(ClusterRole::Coolant, "EngineTemp", "EngTemp"),
        signalSource(ClusterRole::Rpm, "EngineSpeed", "RPM"),

        signalSource(ClusterRole::AiRegime, "TinyML_Telemetry", "RegimeClass"),
        signalSource(ClusterRole::AiAnomaly, "TinyML_Telemetry", "AnomalyScore"),
        signalSource(ClusterRole::AiConfidence, "TinyML_Telemetry", "Confidence"),
        signalSource(ClusterRole::AiHealth, "TinyML_Telemetry", "ThermalHealth"),
    };

    return profile;
}

} // namespace

ClusterProfiles::ClusterProfiles()
{
    registerProfile(example11Bit());
}

ClusterProfiles& ClusterProfiles::instance()
{
    static ClusterProfiles registry;
    return registry;
}

void ClusterProfiles::registerProfile(ClusterProfile profile)
{
    const auto existing = std::ranges::find(m_profiles, profile.id, &ClusterProfile::id);

    if (existing != m_profiles.end()) {
        *existing = std::move(profile);
        return;
    }

    m_profiles.push_back(std::move(profile));
}

const ClusterProfile* ClusterProfiles::find(std::string_view id) const noexcept
{
    const auto found = std::ranges::find(m_profiles, id, &ClusterProfile::id);

    return found == m_profiles.end() ? nullptr : &*found;
}

} // namespace torquebus
