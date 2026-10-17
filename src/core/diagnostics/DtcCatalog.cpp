// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/DtcCatalog.h"

#include "core/diagnostics/DiagnosticEvent.h"
#include "core/diagnostics/UdsTypes.h"

#include <array>
#include <cctype>
#include <format>
#include <vector>

namespace torquebus {
namespace {

[[nodiscard]] int hexDigitValue(char ch) noexcept
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] char toUpperHex(unsigned nibble) noexcept
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    return kHex[nibble & 0x0FU];
}

[[nodiscard]] std::string_view trimSpaces(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string_view systemDescription(DtcSystem system) noexcept
{
    switch (system) {
    case DtcSystem::Powertrain:
        return "Powertrain (Engine & Transmission)";
    case DtcSystem::Chassis:
        return "Chassis (ABS/ESP, Brakes, Steering & Suspension)";
    case DtcSystem::Body:
        return "Body (Airbags, Comfort, A/C & Locks)";
    case DtcSystem::Network:
        return "Network (CAN/LIN Communications & Gateways)";
    }
    return "Powertrain (Engine & Transmission)";
}

[[nodiscard]] DtcScope
classifyScope(DtcSystem system, unsigned firstDigit, unsigned subsystemNibble) noexcept
{
    // SAE J2012 / ISO 15031-6 scope assignment:
    //   0 -> Generic across P, C, B, U
    //   1 -> Manufacturer-specific (OEM) across P, C, B, U
    //   2 -> Generic for P2xxx and U2xxx; OEM for B2xxx and C2xxx
    //   3 -> For P3xxx: P3000-P3399 is OEM, P3400-P3999 is Generic;
    //        U3xxx, B3xxx, C3xxx are Generic / ISO-reserved
    if (firstDigit == 0U) {
        return DtcScope::Generic;
    }
    if (firstDigit == 1U) {
        return DtcScope::ManufacturerSpecific;
    }
    if (firstDigit == 2U) {
        return (system == DtcSystem::Powertrain || system == DtcSystem::Network)
                   ? DtcScope::Generic
                   : DtcScope::ManufacturerSpecific;
    }
    if (system == DtcSystem::Powertrain && subsystemNibble <= 3U) {
        return DtcScope::ManufacturerSpecific;
    }
    return DtcScope::Generic;
}

[[nodiscard]] std::string_view subsystemDescription(DtcSystem system,
                                                    unsigned subsystemNibble) noexcept
{
    switch (system) {
    case DtcSystem::Powertrain:
        switch (subsystemNibble) {
        case 0U:
            return "Fuel, Air Metering & Auxiliary Emission Controls";
        case 1U:
            return "Fuel and Air Metering";
        case 2U:
            return "Fuel and Air Metering (Injector Circuit)";
        case 3U:
            return "Ignition System or Misfire";
        case 4U:
            return "Auxiliary Emission Controls";
        case 5U:
            return "Vehicle Speed & Idle Control System";
        case 6U:
            return "Computer & Auxiliary Output Circuits";
        case 7U:
        case 8U:
        case 9U:
            return "Transmission & Drivetrain Controls";
        case 0xAU:
        case 0xBU:
        case 0xCU:
            return "Hybrid / Electric Propulsion System";
        default:
            return "Powertrain Subsystem (ISO/SAE Reserved)";
        }

    case DtcSystem::Chassis:
        switch (subsystemNibble) {
        case 0U:
            return "Common Chassis & ABS/Traction Controls";
        case 1U:
            return "Brake & Wheel Speed Sensor Subsystem";
        case 2U:
            return "Steering & Yaw/Lateral Dynamics";
        case 3U:
            return "Suspension & Ride Height Control";
        default:
            return "Chassis Subsystem";
        }

    case DtcSystem::Body:
        switch (subsystemNibble) {
        case 0U:
            return "Restraints, Airbags & Occupant Safety";
        case 1U:
            return "HVAC & Cabin Climate Control";
        case 2U:
            return "Instrument Cluster, Lighting & Audible Warnings";
        case 3U:
            return "Doors, Locks, Windows & Seats";
        default:
            return "Body & Cabin Subsystem";
        }

    case DtcSystem::Network:
        switch (subsystemNibble) {
        case 0U:
            return "CAN/LIN Bus Electrical & Physical Layer";
        case 1U:
            return "Lost Communication With Control Module";
        case 2U:
            return "Lost Communication With Secondary/Sub-Network Module";
        case 3U:
            return "Network Software, Checksum & Data Plausibility";
        case 4U:
            return "Invalid / Implausible Data Received From Module";
        default:
            return "Network & Inter-Module Communication";
        }
    }

    return "Vehicle Subsystem";
}

constexpr DtcCatalogEntry kCatalog[] = {
    {
        "P0100",
        "Mass or Volume Air Flow (MAF) \"A\" Circuit",
        "SPN 132 FMI 2/5 (Engine Inlet Air Mass Flow Rate)",
        "Check MAF connector terminals, 5V reference, 12V supply fuse, ground return continuity, "
        "and intake duct seating before replacing the sensor.",
    },
    {
        "P0101",
        "Mass or Volume Air Flow (MAF) \"A\" Circuit Range/Performance",
        "SPN 132 FMI 0/1/11 (MAF Above/Below Normal Range)",
        "Compare live MAF (g/s) against RPM and calculated load; inspect air filter restriction, "
        "post-MAF false air leaks (cracked intake boot/PCV), and contaminated hot-wire element.",
    },
    {
        "P0102",
        "Mass or Volume Air Flow (MAF) \"A\" Circuit Low",
        "SPN 132 FMI 4 (MAF Voltage Below Normal / Shorted Low)",
        "Check for unplugged MAF sensor, open signal wire, blown supply fuse, or signal shorted to "
        "ground.",
    },
    {
        "P0103",
        "Mass or Volume Air Flow (MAF) \"A\" Circuit High",
        "SPN 132 FMI 3 (MAF Voltage Above Normal / Shorted High)",
        "Check for MAF signal wire shorted to 5V/12V or open sensor ground return.",
    },
    {
        "P0106",
        "Manifold Absolute Pressure (MAP) / Barometric Pressure Range/Performance",
        "SPN 102 FMI 2 (Engine Intake Manifold #1 Pressure Erratic)",
        "With ignition ON and engine OFF (KOEO), verify MAP matches barometric pressure (~100 kPa "
        "at sea level); check vacuum hose and 5V reference.",
    },
    {
        "P0113",
        "Intake Air Temperature (IAT) Sensor 1 Circuit High",
        "SPN 105 FMI 3/5 (Engine Intake Manifold #1 Temp High/Open)",
        "Live data showing -40 degC indicates an open circuit or unplugged connector; inspect "
        "sensor pins and ground return.",
    },
    {
        "P0118",
        "Engine Coolant Temperature (ECT) Sensor 1 Circuit High",
        "SPN 110 FMI 3/5 (Engine Coolant Temperature Voltage High / Open)",
        "Live data reading -40 degC points to an open NTC circuit, corroded 2-pin plug, or broken "
        "sensor ground wire.",
    },
    {
        "P0120",
        "Throttle / Pedal Position Sensor / Switch \"A\" Circuit",
        "SPN 91 FMI 3/4 (Accelerator Pedal Position 1)",
        "Sweep throttle slowly with ignition ON while monitoring TPS1/TPS2 on oscilloscope for "
        "dropouts, 5V reference stability, and ground voltage drop.",
    },
    {
        "P0128",
        "Coolant Thermostat (Coolant Temperature Below Regulating Temperature)",
        "SPN 110 FMI 16/18/3/4 (Engine Coolant Temperature Abnormal / Below Range)",
        "Verify actual coolant level, monitor warm-up curve in live data, check for a stuck-open "
        "thermostat, and verify ECT sensor calibration and wiring/grounds.",
    },
    {
        "P0130",
        "O2 Sensor Circuit (Bank 1, Sensor 1)",
        "SPN 3216 FMI 2/4 (Aftertreatment 1 Intake NOx / O2 Sensor)",
        "Check O2 heater fuse and supply, inspect upstream exhaust manifold for air leaks, and "
        "verify closed-loop signal switching at operating temperature.",
    },
    {
        "P0171",
        "System Too Lean (Bank 1)",
        "SPN 4237 FMI 0/16 (Long-Term Fuel Trim High / Lean Mixture)",
        "Capture Freeze Frame; compare STFT/LTFT (> +10..15% at idle that improves at 2500 RPM "
        "points to a vacuum/PCV leak; lean under load points to weak fuel pressure or contaminated "
        "MAF). Smoke-test intake and PCV plumbing before replacing O2 sensors.",
    },
    {
        "P0172",
        "System Too Rich (Bank 1)",
        "SPN 4237 FMI 1/18 (Long-Term Fuel Trim Low / Rich Mixture)",
        "Check negative STFT/LTFT (< -10..15%); inspect stuck-open EVAP purge solenoid, leaking "
        "fuel injectors, excessive fuel rail pressure, or skewed MAF reading.",
    },
    {
        "P0174",
        "System Too Lean (Bank 2)",
        "SPN 4238 FMI 0/16 (Bank 2 Fuel Trim High / Lean)",
        "Compare Bank 1 vs Bank 2 trims; if both banks are lean, suspect shared MAF, PCV, or fuel "
        "supply; if only Bank 2, check Bank 2 intake runner gasket or exhaust leak.",
    },
    {
        "P0175",
        "System Too Rich (Bank 2)",
        "SPN 4238 FMI 1/18 (Bank 2 Fuel Trim Low / Rich)",
        "Compare Bank 1 vs Bank 2 trims; inspect Bank 2 leaking injectors or stuck-open EVAP purge "
        "path.",
    },
    {
        "P0190",
        "Fuel Rail Pressure Sensor \"A\" Circuit",
        "SPN 157 FMI 3/4 (Engine Injector Metering Rail 1 Pressure)",
        "Verify 5V sensor reference, ground voltage drop (< 0.1V), and KOEO pressure plausibility "
        "before condemning high-pressure pump or rail sensor.",
    },
    {
        "P0200",
        "Injector Circuit / Open",
        "SPN 651-656 FMI 5 (Engine Cylinder Injector Current Below Normal / Open)",
        "Measure injector solenoid resistance (~11-16 Ohms for port, lower for piezo/GDI), verify "
        "12V feed, and check driver pulse on oscilloscope.",
    },
    {
        "P0234",
        "Turbocharger / Supercharger \"A\" Overboost Condition",
        "SPN 102 FMI 0/16 (Intake Manifold Pressure Above Normal Range)",
        "Compare commanded vs actual boost (IMAP) under load; check sticking wastegate/VNT vanes, "
        "boost control solenoid hose, and MAP sensor calibration.",
    },
    {
        "P0236",
        "Turbocharger / Supercharger Boost Sensor \"A\" Circuit Range/Performance",
        "SPN 102 FMI 2/10 (Intake Manifold Pressure Erratic / Abnormal Rate)",
        "Compare MAP, boost sensor, and barometric sensor at KOEO; inspect sensor port for soot "
        "restriction and check 5V reference wiring.",
    },
    {
        "P0299",
        "Turbocharger / Supercharger \"A\" Underboost",
        "SPN 102 FMI 1/18/10 (Intake Manifold Pressure Below Normal / Slow Response)",
        "Compare desired vs actual IMAP in live data; pressure-test charge-air cooler and hoses "
        "for boost leaks, check vacuum/electronic VNT actuator travel, and inspect for exhaust/DPF "
        "restriction.",
    },
    {
        "P0300",
        "Random / Multiple Cylinder Misfire Detected",
        "SPN 1322 FMI 11/31 (Engine Misfire for Multiple Cylinders)",
        "Decision tree: (A) Read Freeze Frame & per-cylinder misfire counters before clearing; "
        "(B) Visual inspection: coils, plugs, rodent-damaged harnesses, cracked PCV/vacuum hoses; "
        "(C) Mixture: check STFT/LTFT (> +/-10..15%), MAF plausibility & O2 switching; "
        "(D) Ignition: swap coils & measure supply/ground voltage drop (< 0.3..0.5V under load); "
        "(E) Fuel: rail pressure, injector balance & EVAP purge; (F) Mechanical compression.",
    },
    {
        "P0301",
        "Cylinder 1 Misfire Detected",
        "SPN 1323 FMI 11 (Engine Cylinder 1 Misfire)",
        "Swap Cylinder 1 coil and spark plug with different cylinders to see if misfire follows "
        "the coil or plug; check Cylinder 1 injector pulse, harness continuity, and compression.",
    },
    {
        "P0302",
        "Cylinder 2 Misfire Detected",
        "SPN 1324 FMI 11 (Engine Cylinder 2 Misfire)",
        "Swap Cylinder 2 coil and spark plug with a healthy cylinder; inspect harness lead, "
        "injector balance, and cylinder compression.",
    },
    {
        "P0303",
        "Cylinder 3 Misfire Detected",
        "SPN 1325 FMI 11 (Engine Cylinder 3 Misfire)",
        "Swap Cylinder 3 coil and spark plug with a healthy cylinder; inspect harness lead, "
        "injector balance, and cylinder compression.",
    },
    {
        "P0304",
        "Cylinder 4 Misfire Detected",
        "SPN 1326 FMI 11 (Engine Cylinder 4 Misfire)",
        "Swap Cylinder 4 coil and spark plug with a healthy cylinder; inspect harness lead, "
        "injector balance, and cylinder compression.",
    },
    {
        "P0305",
        "Cylinder 5 Misfire Detected",
        "SPN 1327 FMI 11 (Engine Cylinder 5 Misfire)",
        "Swap Cylinder 5 coil and spark plug with a healthy cylinder; inspect harness lead, "
        "injector balance, and cylinder compression.",
    },
    {
        "P0306",
        "Cylinder 6 Misfire Detected",
        "SPN 1328 FMI 11 (Engine Cylinder 6 Misfire)",
        "Swap Cylinder 6 coil and spark plug with a healthy cylinder; inspect harness lead, "
        "injector balance, and cylinder compression.",
    },
    {
        "P0335",
        "Crankshaft Position Sensor \"A\" Circuit",
        "SPN 190 / SPN 636 FMI 2/8 (Engine Speed / Position Sensor)",
        "Capture CKP waveform with oscilloscope while cranking; check sensor air gap, tone-wheel "
        "teeth, connector pins, and wiring routed near starter or exhaust.",
    },
    {
        "P0340",
        "Camshaft Position Sensor \"A\" Circuit (Bank 1 or Single Sensor)",
        "SPN 637 / SPN 723 FMI 2/8 (Engine Timing / Camshaft Sensor)",
        "Correlate CKP and CMP waveforms on dual-channel oscilloscope to separate electrical "
        "sensor/wiring failure from mechanical timing chain/belt stretch.",
    },
    {
        "P0401",
        "Exhaust Gas Recirculation (EGR) \"A\" Flow Insufficient Detected",
        "SPN 411 / SPN 2659 FMI 1/18 (EGR Differential Pressure / Mass Flow Low)",
        "Command EGR valve open in active test while watching MAP/MAF reaction; inspect EGR cooler "
        "and passages for carbon blockage.",
    },
    {
        "P0420",
        "Catalyst System Efficiency Below Threshold (Bank 1)",
        "SPN 3050 FMI 18 (Catalyst 1 System Monitor Below Normal)",
        "Never replace the catalytic converter first: check for upstream exhaust or flex-joint "
        "micro-leaks, verify fuel trims are near 0%, compare pre-cat (oscillating) vs post-cat "
        "(steady) O2 waveforms, and check OEM software TSBs.",
    },
    {
        "P0440",
        "Evaporative Emission (EVAP) System",
        "SPN 1800 FMI 11 (Evaporative Purge System)",
        "Inspect fuel filler cap seal, command purge and vent valves in functional test, and "
        "smoke-test EVAP canister hoses.",
    },
    {
        "P0455",
        "Evaporative Emission (EVAP) System Leak Detected (Gross Leak)",
        "SPN 1800 FMI 1/7 (EVAP System Large Leak)",
        "Verify fuel cap is tightened/sealing, check for stuck-open purge solenoid at idle, and "
        "inspect split charcoal canister hoses with smoke machine.",
    },
    {
        "P0500",
        "Vehicle Speed Sensor (VSS) \"A\"",
        "SPN 84 FMI 2/9 (Wheel-Based Vehicle Speed)",
        "Compare ECM vehicle speed with ABS individual wheel speed sensors in live data; check "
        "CAN broadcast from ABS or output shaft speed sensor wiring.",
    },
    {
        "P0507",
        "Idle Air Control System RPM Higher Than Expected",
        "SPN 188 FMI 0 (Engine Speed at Idle Above Normal)",
        "Smoke-test intake manifold for false air / cracked PCV hose; inspect throttle body bore "
        "and perform electronic throttle idle relearn.",
    },
    {
        "P0562",
        "System Voltage Low",
        "SPN 168 FMI 4/18 (Battery Potential / Power Input 1 Below Normal)",
        "Measure battery and alternator voltage under load; test voltage drop (> 0.3V) across "
        "battery negative to chassis/engine grounds and ECM main relay supply pins.",
    },
    {
        "P0563",
        "System Voltage High",
        "SPN 168 FMI 3/16 (Battery Potential / Power Input 1 Above Normal)",
        "Check alternator regulator output (> 15.0V) and LIN/PWM alternator control circuit "
        "immediately to prevent ECU over-voltage damage.",
    },
    {
        "P0606",
        "ECM / PCM Processor",
        "SPN 629 FMI 12 (Controller #1 Internal Fault)",
        "Verify battery stability, inspect main ECU power/ground pins under load, and rule out "
        "non-resistor spark plugs or inductive coil interference before replacing the ECU.",
    },
    {
        "P0700",
        "Transmission Control System (MIL Request)",
        "SPN 523 / SPN 161 FMI 11 (Transmission System Fault)",
        "Informational code set by ECM when TCM requests the Check Engine light; scan the "
        "Transmission Control Module directly for specific solenoid, pressure, or slip codes.",
    },
    {
        "B1000",
        "Body Control Module / Restraints ECU Internal or Supply Fault",
        "SPN 629 FMI 12 (Body Controller Fault)",
        "Check BCM/Airbag power supply fuses, cabin ground points, and battery voltage stability "
        "during cranking.",
    },
    {
        "C0035",
        "Left Front Wheel Speed Sensor Supply / Circuit",
        "SPN 789 FMI 2/5 (Wheel Sensor ABS Axle 1 Left)",
        "Monitor all four wheel speeds at low speed; inspect hub magnetic encoder ring, sensor air "
        "gap, and flexed harness pigtail inside the wheel arch.",
    },
    {
        "U0001",
        "High Speed CAN Communication Bus",
        "SPN 639 FMI 2/19 (J1939 Network #1 Bus Error)",
        "Disconnect battery and measure resistance between CAN-H (pin 6) and CAN-L (pin 14) on "
        "J1962 connector: expect ~60 Ohms (two 120-Ohm terminators in parallel). Check waveforms "
        "with oscilloscope (~2.5V recessive, 3.5V/1.5V dominant).",
    },
    {
        "U0035",
        "Vehicle Area Network (CAN) Bus Wiring / Communication Fault",
        "SPN 639 FMI 14 (CAN Network Physical Layer Fault)",
        "Check CAN twisted-pair continuity, termination resistors, star-connector splice packs, "
        "and module chassis grounds.",
    },
    {
        "U0100",
        "Lost Communication With ECM / PCM \"A\"",
        "SPN 639 / SPN 2000 FMI 9/19 (Loss of Communications With Engine SA 0)",
        "Before blaming the ECM: inspect J1962 diagnostic connector pin fitment/corrosion, verify "
        "ECM terminal 30/15 power feeds and main engine grounds under load, and check CAN-H/CAN-L "
        "continuity to the ECM plug.",
    },
    {
        "U0121",
        "Lost Communication With Anti-Lock Brake System (ABS) Control Module",
        "SPN 639 FMI 9 (Loss of Communications With Brake Controller SA 11)",
        "Verify ABS module fuses (pump and logic feeds), chassis ground stud behind wheel liner or "
        "frame rail, and CAN branch drop wiring.",
    },
    {
        "U0140",
        "Lost Communication With Body Control Module (BCM)",
        "SPN 639 FMI 9 (Loss of Communications With Body Controller SA 33)",
        "Check BCM constant and ignition power supplies, cabin ground eyelets, and gateway wake-up "
        "line.",
    },
    {
        "U0155",
        "Lost Communication With Instrument Panel Cluster (IPC) Control Module",
        "SPN 639 FMI 9 (Loss of Communications With Instrument Cluster SA 23)",
        "Inspect cluster connector lock, ignition wake-up feed, ground return, and CAN bus "
        "termination.",
    },
};

} // namespace

DtcStructure analyzeDtcCode(std::uint32_t rawCode)
{
    DtcStructure result;
    result.rawCode = rawCode & 0xFF'FFFFU;
    result.failureType = static_cast<std::uint8_t>(rawCode & 0xFFU);
    result.hasFailureType = result.failureType != 0U;

    const auto high = static_cast<std::uint8_t>((rawCode >> 16U) & 0xFFU);
    const auto mid = static_cast<std::uint8_t>((rawCode >> 8U) & 0xFFU);

    const auto systemBits = static_cast<unsigned>((high >> 6U) & 0x03U);
    const auto firstDigit = static_cast<unsigned>((high >> 4U) & 0x03U);
    const auto subsystemNibble = static_cast<unsigned>(high & 0x0FU);

    result.system = static_cast<DtcSystem>(systemBits);
    result.scope = classifyScope(result.system, firstDigit, subsystemNibble);
    result.firstDigit = static_cast<char>('0' + firstDigit);
    result.subsystemDigit = toUpperHex(subsystemNibble);

    static constexpr char kLetters[] = {'P', 'C', 'B', 'U'};
    result.code = std::format("{}{}{:01X}{:02X}",
                              kLetters[systemBits],
                              firstDigit,
                              subsystemNibble,
                              static_cast<unsigned>(mid));

    result.systemName = systemDescription(result.system);
    result.scopeName = result.scope == DtcScope::Generic ? "Generic (SAE/ISO standard)"
                                                         : "Manufacturer-specific (OEM)";
    result.subsystemName = subsystemDescription(result.system, subsystemNibble);

    return result;
}

std::optional<DtcStructure> parseDtcCode(std::string_view text)
{
    text = trimSpaces(text);
    if (text.empty()) {
        return std::nullopt;
    }

    // Form 1: Standard `L D D D D` (optionally followed by `-FT` or `:FT` or ` FT`)
    const char first = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    int systemIndex = -1;
    if (first == 'P') {
        systemIndex = 0;
    } else if (first == 'C') {
        systemIndex = 1;
    } else if (first == 'B') {
        systemIndex = 2;
    } else if (first == 'U') {
        systemIndex = 3;
    }

    if (systemIndex >= 0 && text.size() >= 5) {
        const int d1 = hexDigitValue(text[1]);
        const int d2 = hexDigitValue(text[2]);
        const int d3 = hexDigitValue(text[3]);
        const int d4 = hexDigitValue(text[4]);

        if (d1 >= 0 && d1 <= 3 && d2 >= 0 && d3 >= 0 && d4 >= 0) {
            std::uint8_t ftb = 0U;
            bool hasFtb = false;
            bool form1Valid = true;

            std::string_view rest = trimSpaces(text.substr(5));
            if (!rest.empty()) {
                if (rest.front() == '-' || rest.front() == ':') {
                    rest = trimSpaces(rest.substr(1));
                }
                if (rest.size() != 2) {
                    form1Valid = false;
                } else {
                    const int f1 = hexDigitValue(rest[0]);
                    const int f2 = hexDigitValue(rest[1]);
                    if (f1 < 0 || f2 < 0) {
                        form1Valid = false;
                    } else {
                        ftb = static_cast<std::uint8_t>((f1 << 4) | f2);
                        hasFtb = true;
                    }
                }
            }

            if (form1Valid) {
                const auto high =
                    static_cast<std::uint8_t>((systemIndex << 6) | (d1 << 4) | (d2 & 0x0F));
                const auto mid = static_cast<std::uint8_t>((d3 << 4) | (d4 & 0x0F));
                const std::uint32_t raw = (static_cast<std::uint32_t>(high) << 16U)
                                          | (static_cast<std::uint32_t>(mid) << 8U) | ftb;

                DtcStructure structure = analyzeDtcCode(raw);
                structure.hasFailureType = hasFtb;
                return structure;
            }
        }
    }

    // Form 2: Three raw UDS hex bytes ("01 28 00", "0x012800", "C0 35 00")
    std::vector<std::uint8_t> bytes;
    if (parseHexBytes(text, bytes) && bytes.size() == 3) {
        const std::uint32_t raw = (static_cast<std::uint32_t>(bytes[0]) << 16U)
                                  | (static_cast<std::uint32_t>(bytes[1]) << 8U) | bytes[2];
        DtcStructure structure = analyzeDtcCode(raw);
        structure.hasFailureType = true;
        return structure;
    }

    return std::nullopt;
}

std::string describeDtcStatus(std::uint8_t status)
{
    if (status == 0U) {
        return "cleared";
    }

    std::string flags;
    const auto appendFlag = [&flags](std::string_view label) {
        if (!flags.empty()) {
            flags += ", ";
        }
        flags += label;
    };

    if ((status & 0x01U) != 0U) {
        appendFlag("active");
    }
    if ((status & 0x04U) != 0U) {
        appendFlag("pending");
    }
    if ((status & 0x08U) != 0U) {
        appendFlag("confirmed");
    }
    if ((status & 0x80U) != 0U) {
        appendFlag("MIL on");
    }
    if (flags.empty() && (status & 0x02U) != 0U) {
        appendFlag("failed this cycle");
    }
    if (flags.empty() && (status & 0x20U) != 0U) {
        appendFlag("failed since clear");
    }
    if (flags.empty()) {
        return std::format("status 0x{:02X}", status);
    }

    return flags;
}

std::span<const DtcCatalogEntry> dtcCatalogEntries() noexcept
{
    return kCatalog;
}

const DtcCatalogEntry* findDtcCatalogEntry(std::string_view code) noexcept
{
    const std::optional<DtcStructure> parsed = parseDtcCode(code);
    if (!parsed.has_value()) {
        return nullptr;
    }

    for (const DtcCatalogEntry& entry : kCatalog) {
        if (entry.code == parsed->code) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace torquebus
