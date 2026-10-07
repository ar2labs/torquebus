// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/cluster/ClusterRole.h"

#include <array>

namespace torquebus {
namespace {

constexpr std::array kRoles{
    ClusterRole::Speed,     ClusterRole::Rpm,        ClusterRole::Gear,
    ClusterRole::Coolant,   ClusterRole::Oil,        ClusterRole::Fuel,
    ClusterRole::Def,       ClusterRole::Battery,    ClusterRole::AirPressure,
    ClusterRole::Ambient,   ClusterRole::Odometer,   ClusterRole::Hours,
    ClusterRole::AiRegime,  ClusterRole::AiAnomaly,  ClusterRole::AiConfidence,
    ClusterRole::AiHealth,  ClusterRole::Ignition,   ClusterRole::Cruise,
    ClusterRole::LampHigh,  ClusterRole::LampLow,    ClusterRole::LampPosition,
    ClusterRole::LampPark,  ClusterRole::LampBelt,   ClusterRole::LampEngine,
    ClusterRole::LampOil,   ClusterRole::LampAbs,    ClusterRole::LampLeft,
    ClusterRole::LampRight, ClusterRole::LampHazard, ClusterRole::DmStop,
    ClusterRole::DmWarn,    ClusterRole::DmMil,      ClusterRole::DmProtect,
};

} // namespace

std::string_view nameOf(ClusterRole role)
{
    // The keys of ClusterView.qml's `vehicle` contract. ClusterRoleTests checks that the QML reads
    // exactly these.
    switch (role) {
    case ClusterRole::Speed:
        return "speed";
    case ClusterRole::Rpm:
        return "rpm";
    case ClusterRole::Gear:
        return "gear";
    case ClusterRole::Coolant:
        return "coolant";
    case ClusterRole::Oil:
        return "oil";
    case ClusterRole::Fuel:
        return "fuel";
    case ClusterRole::Def:
        return "def";
    case ClusterRole::Battery:
        return "battery";
    case ClusterRole::AirPressure:
        return "airPressure";
    case ClusterRole::Ambient:
        return "ambient";
    case ClusterRole::Odometer:
        return "odometer";
    case ClusterRole::Hours:
        return "hours";
    case ClusterRole::AiRegime:
        return "aiRegime";
    case ClusterRole::AiAnomaly:
        return "aiAnomaly";
    case ClusterRole::AiConfidence:
        return "aiConfidence";
    case ClusterRole::AiHealth:
        return "aiHealth";
    case ClusterRole::Ignition:
        return "ignition";
    case ClusterRole::Cruise:
        return "cruise";
    case ClusterRole::LampHigh:
        return "lampHigh";
    case ClusterRole::LampLow:
        return "lampLow";
    case ClusterRole::LampPosition:
        return "lampPosition";
    case ClusterRole::LampPark:
        return "lampPark";
    case ClusterRole::LampBelt:
        return "lampBelt";
    case ClusterRole::LampEngine:
        return "lampEngine";
    case ClusterRole::LampOil:
        return "lampOil";
    case ClusterRole::LampAbs:
        return "lampAbs";
    case ClusterRole::LampLeft:
        return "lampLeft";
    case ClusterRole::LampRight:
        return "lampRight";
    case ClusterRole::LampHazard:
        return "lampHazard";
    case ClusterRole::DmStop:
        return "dmStop";
    case ClusterRole::DmWarn:
        return "dmWarn";
    case ClusterRole::DmMil:
        return "dmMil";
    case ClusterRole::DmProtect:
        return "dmProtect";
    }

    return "speed";
}

bool isFlag(ClusterRole role)
{
    // The flags are the tail of the enum, from Ignition on.
    return role >= ClusterRole::Ignition;
}

std::span<const ClusterRole> allClusterRoles()
{
    return kRoles;
}

} // namespace torquebus
