// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/cluster/ClusterProfiles.h"

#include "core/j1939/J1939Diagnostics.h"

#include <algorithm>
#include <utility>

namespace torquebus {
namespace {

[[nodiscard]] ClusterSource signalSource(ClusterRole role,
                                         std::string message,
                                         std::string signal,
                                         std::uint32_t maxAgeMs = kClusterDefaultMaxAgeMs)
{
    ClusterSource source;
    source.role = role;
    source.binding.source = DashboardBinding::Source::Signal;
    source.binding.message = std::move(message);
    source.binding.signal = std::move(signal);
    source.maxAgeMs = maxAgeMs;
    return source;
}

/// A role read from a system variable. A variable has no age, so there is nothing to say about one:
/// it is a value somebody wrote, and it is steady until they write another.
[[nodiscard]] ClusterSource variableSource(ClusterRole role, std::string_view variable)
{
    ClusterSource source;
    source.role = role;
    source.binding.source = DashboardBinding::Source::Variable;
    source.binding.variable = std::string{variable};
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
        // Sent once a second by the example's ECU, so the default two seconds would be two periods.
        signalSource(ClusterRole::Coolant, "EngineTemp", "EngTemp", 3000),
        signalSource(ClusterRole::Rpm, "EngineSpeed", "RPM"),

        signalSource(ClusterRole::AiRegime, "TinyML_Telemetry", "RegimeClass"),
        signalSource(ClusterRole::AiAnomaly, "TinyML_Telemetry", "AnomalyScore"),
        signalSource(ClusterRole::AiConfidence, "TinyML_Telemetry", "Confidence"),
        signalSource(ClusterRole::AiHealth, "TinyML_Telemetry", "ThermalHealth"),
    };

    return profile;
}

/// A J1939 commercial vehicle, as examples/databases/j1939.dbc describes it and
/// examples/projects/j1939-vehicle.tbsproj runs it: every signal here is read from the plot store,
/// which a J1939 block (or a DBC decoder) behind a Signal Plot fills.
///
/// Three things in it are not the first thing one would write, and each is a decision:
///
///  * The DM1 lamps are variables, not signals. Every ECU of a vehicle sends its own DM1, a
///  database
///    names a message once, and so the DM1 of the engine and the DM1 of the five ECUs with nothing
///    to report all land in the one series "DM1.RedStopLamp": a red lamp would show for the few
///    milliseconds each second between the engine's message and the next one. The J1939 block
///    knows which ECU said what, and publishes the lamps of the whole bus as variables
///    (J1939Diagnostics.h) - which is what a cluster in a vehicle does.
///  * The engine lamp has no source of its own. It is the malfunction indicator of the DM1, which
///    is dmMil, and the cluster lights the engine lamp for either.
///  * The AI regime is the model's own RegimeClass, and not its risk level. They look alike - a
///    small number - and mean different things: the cluster names regime 0 "Idle" and 1 "Cruise",
///    and would say so of a vehicle at 100 km/h whenever the model was calm.
///
/// The ages are tighter than the default two seconds, because here they can be: a J1939 message has
/// a cycle time and keeps it, and a speed that stopped arriving 300 ms ago should say so.
/// ClusterProfilesTests checks every name below against the database, and the project test checks
/// that the pipeline delivers every one of them.
[[nodiscard]] ClusterProfile j1939Commercial()
{
    ClusterProfile profile;
    profile.id = std::string{kJ1939CommercialProfile};
    profile.name = "J1939 Commercial Vehicle";

    profile.sources = {
        signalSource(ClusterRole::Speed, "CCVS1", "WheelBasedVehicleSpeed", 300),
        signalSource(ClusterRole::Rpm, "EEC1", "EngineSpeed", 100),
        signalSource(ClusterRole::Gear, "ETC2", "TransmissionCurrentGear", 300),
        signalSource(ClusterRole::Coolant, "ET1", "EngineCoolantTemperature", 3000),
        signalSource(ClusterRole::Oil, "EFL_P1", "EngineOilPressure", 1500),
        signalSource(ClusterRole::Fuel, "DD1", "FuelLevel1", 3000),
        signalSource(ClusterRole::Def, "AT1T1I1", "DieselExhaustFluidTankLevel", 3000),
        signalSource(ClusterRole::Battery, "VEP1", "BatteryPotential", 3000),
        signalSource(ClusterRole::AirPressure, "AIR1", "ServiceBrakeCircuit1AirPress", 3000),
        signalSource(ClusterRole::Ambient, "AMB", "AmbientAirTemperature", 3000),
        signalSource(ClusterRole::Odometer, "VDHR", "TotalVehicleDistance", 3000),
        signalSource(ClusterRole::Hours, "HOURS", "TotalEngineHours", 3000),

        signalSource(ClusterRole::AiRegime, "TinyML_Proprietary", "RegimeClass", 300),
        signalSource(ClusterRole::AiAnomaly, "TinyML_Proprietary", "AnomalyScore", 300),
        signalSource(ClusterRole::AiConfidence, "TinyML_Proprietary", "Confidence", 300),
        signalSource(ClusterRole::AiHealth, "TinyML_Proprietary", "ThermalHealth", 300),

        signalSource(ClusterRole::Cruise, "CCVS1", "CruiseControlActive", 300),
        signalSource(ClusterRole::LampHigh, "LD", "HighBeamHeadlight", 3000),
        signalSource(ClusterRole::LampLow, "LD", "LowBeamHeadlight", 3000),
        signalSource(ClusterRole::LampPosition, "LD", "PositionLights", 3000),
        signalSource(ClusterRole::LampPark, "CCVS1", "ParkingBrakeSwitch", 300),
        signalSource(ClusterRole::LampBelt, "BAS", "SeatBeltWarningLamp", 3000),
        signalSource(ClusterRole::LampAbs, "EBC1", "ABSAmberWarning", 300),
        signalSource(ClusterRole::LampLeft, "LD", "LeftTurnSignal", 3000),
        signalSource(ClusterRole::LampRight, "LD", "RightTurnSignal", 3000),
        signalSource(ClusterRole::LampHazard, "OEL", "HazardSwitch", 600),

        variableSource(ClusterRole::DmStop, kJ1939LampStopVariable),
        variableSource(ClusterRole::DmWarn, kJ1939LampWarningVariable),
        variableSource(ClusterRole::DmMil, kJ1939LampMilVariable),
        variableSource(ClusterRole::DmProtect, kJ1939LampProtectVariable),
    };

    return profile;
}

} // namespace

ClusterProfiles::ClusterProfiles()
{
    registerProfile(example11Bit());
    registerProfile(j1939Commercial());
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
