// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What an instrument cluster shows, as roles.
//
// A cluster does not know signals, messages or buses. It knows that there is a speed, an engine
// speed, a lamp for the parking brake. Where each of those comes from is the business of a
// ClusterProfile, and that split is what lets the same cluster sit on the 11-bit example, on a
// J1939/FMS truck or on a signal list somebody typed, without the cluster changing.
//
// The name of a role is also its key in the `vehicle` object the QML cluster reads, so the list
// here and the contract at the top of ClusterView.qml are one list. A role nobody feeds is not an
// error: the cluster draws dashes for it and lights up when a source arrives (ClusterView.qml).

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace torquebus {

enum class ClusterRole : std::uint8_t {
    // --- Numbers -----------------------------------------------------------------------------
    Speed, ///< km/h
    Rpm, ///< engine speed, rpm
    Gear, ///< -1 reverse, 0 neutral, 1..n
    Coolant, ///< degC
    Oil, ///< oil pressure, kPa
    Fuel, ///< %
    Def, ///< diesel exhaust fluid (AdBlue) level, %
    Battery, ///< V
    AirPressure, ///< brake air pressure, bar
    Ambient, ///< outside temperature, degC
    Odometer, ///< km
    Hours, ///< engine hours, h

    /// What the TinyML virtual ECU says about the vehicle.
    AiRegime, ///< 0 idle, 1 cruise, 2 high load, 3 thermal stress, 4 anomaly
    AiAnomaly, ///< %
    AiConfidence, ///< %
    AiHealth, ///< thermal health, %

    // --- Flags: on above 0.5 -----------------------------------------------------------------
    /// The one flag whose absence means "on": a vehicle that does not report its ignition is
    /// not one that is off.
    Ignition,
    Cruise,
    LampHigh,
    LampLow,
    LampPosition,
    LampPark,
    LampBelt,
    LampEngine,
    LampOil,
    LampAbs,
    LampLeft,
    LampRight,
    LampHazard,

    /// The lamps of the J1939 DM1 message: stop, warning, malfunction and protect.
    DmStop,
    DmWarn,
    DmMil,
    DmProtect,
};

/// The role's key in the `vehicle` object of the QML cluster.
[[nodiscard]] std::string_view nameOf(ClusterRole role);

/// True for the roles that are on or off rather than measured: a flag is on when its source reads
/// above 0.5, the threshold a Lamp widget starts with.
[[nodiscard]] bool isFlag(ClusterRole role);

/// Every role, in declaration order.
[[nodiscard]] std::span<const ClusterRole> allClusterRoles();

} // namespace torquebus
