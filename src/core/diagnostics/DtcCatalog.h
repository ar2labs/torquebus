// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Diagnostic Trouble Code (DTC) structure, status decoding, and reference
// catalog: SAE J2012 / ISO 15031-6 (OBD-II) and ISO 14229-1 (UDS), with cross-
// references to SAE J1939-73 (SPN / FMI).
//
// A trouble code is not a diagnosis; it is the starting point of one. Showing
// `P0300` or `01 28 00` without saying what part of the vehicle it belongs to,
// whether the standard or the manufacturer defined it, what its J1939 heavy-
// duty counterpart looks like, and what to check before replacing parts blindly
// leaves the engineer to look all of that up in a second window.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace torquebus {

/// Which vehicle domain a trouble code belongs to: the leading letter of
/// `L D D D D` (bits 7..6 of the high byte on the wire).
enum class DtcSystem : std::uint8_t {
    /// `P` — Engine, transmission, and drivetrain emissions/controls.
    Powertrain = 0U,

    /// `C` — ABS/ESP, traction, steering, and suspension.
    Chassis = 1U,

    /// `B` — Airbags, lighting, climate control, locks, and cabin comfort.
    Body = 2U,

    /// `U` — CAN/LIN networks, gateways, shared power/ground, and inter-module
    /// communication.
    Network = 3U,
};

/// Who defines the code's meaning: SAE/ISO across all brands, or a specific
/// vehicle manufacturer (OEM).
enum class DtcScope : std::uint8_t {
    /// Defined by SAE J2012 / ISO 15031-6 (`P0xxx`, `P2xxx`, `P34xx–P39xx`,
    /// `B0xxx`, `C0xxx`, `U0xxx`, `U2xxx`, `U3xxx`).
    Generic = 0U,

    /// Defined by the vehicle manufacturer (`P1xxx`, `P30xx–P33xx`, `B1xxx`,
    /// `B2xxx`, `C1xxx`, `C2xxx`, `U1xxx`).
    ManufacturerSpecific = 1U,
};

/// The structural breakdown of any valid trouble code (`L D D D D`), whether or
/// not it appears in the curated reference table.
struct DtcStructure final {
    /// Canonical 5-character name: `"P0300"`, `"C0035"`, `"B1000"`, `"U0100"`.
    std::string code;

    /// The 24-bit UDS representation on the wire (two code bytes followed by
    /// the failure type byte, e.g. `0x012800` for `P0128`).
    std::uint32_t rawCode{};

    /// The UDS Failure Type Byte (third byte on the wire), `0x00` when none was
    /// specified.
    std::uint8_t failureType{};

    /// True when the input explicitly carried a failure type byte (e.g.
    /// `"P0128-35"` or three hex bytes `"01 28 35"`).
    bool hasFailureType{false};

    DtcSystem system{DtcSystem::Powertrain};
    DtcScope scope{DtcScope::Generic};

    /// The first digit after the letter (`'0'`..`'3'`).
    char firstDigit{'0'};

    /// The second digit after the letter (`'0'`..`'9'`, `'A'`..`'F'`), which
    /// identifies the subsystem.
    char subsystemDigit{'0'};

    /// Human-readable vehicle domain, e.g. `"Powertrain (Engine & Transmission)"`.
    std::string_view systemName;

    /// Human-readable scope, e.g. `"Generic (SAE/ISO standard)"` or
    /// `"Manufacturer-specific (OEM)"`.
    std::string_view scopeName;

    /// Human-readable subsystem from the 2nd digit, e.g.
    /// `"Ignition System or Misfire"`.
    std::string_view subsystemName;
};

/// One curated entry in the built-in workshop reference catalog.
struct DtcCatalogEntry final {
    /// Canonical 5-character code, e.g. `"P0300"`, `"P0171"`, `"U0100"`.
    std::string_view code;

    /// Standard title of the fault, e.g.
    /// `"Random/Multiple Cylinder Misfire Detected"`.
    std::string_view summary;

    /// Corresponding heavy-duty SAE J1939 SPN / FMI mapping by system and
    /// symptom, e.g. `"SPN 1322 FMI 11/31 (Engine Misfire for Multiple Cylinders)"`.
    std::string_view j1939Equivalent;

    /// Practical first checks (freeze frame, live data parameters, physical
    /// inspection, and electrical verification before replacing parts).
    std::string_view firstChecks;
};

/// Decomposes a 24-bit UDS trouble code (as carried in `0x59 0x02`) into its
/// `L D D D D` structure, system, scope, and subsystem.
[[nodiscard]] DtcStructure analyzeDtcCode(std::uint32_t rawCode);

/// Parses a human-written trouble code into its canonical structure and 24-bit
/// UDS wire code.
///
/// Accepts:
///   * Standard 5-character codes: `"P0300"`, `"p0171"`, `"U0100"`, `"C0035"`
///   * Extended UDS codes with failure type byte: `"P0128-35"`, `"P0128:35"`
///   * Raw 3-byte UDS hex: `"01 28 00"`, `"0x012800"`, `"C0 35 00"`
///
/// Returns `std::nullopt` when `text` is not a valid trouble code.
[[nodiscard]] std::optional<DtcStructure> parseDtcCode(std::string_view text);

/// Decodes an ISO 14229-1 DTC status byte into the flags an engineer acts on
/// (`"active, confirmed"`, `"pending"`, `"confirmed, MIL on"`, etc.).
[[nodiscard]] std::string describeDtcStatus(std::uint8_t status);

/// Every curated code in the built-in reference catalog.
[[nodiscard]] std::span<const DtcCatalogEntry> dtcCatalogEntries() noexcept;

/// Looks up `code` (case-insensitive, e.g. `"P0300"`, `"p0171"`, or `"P0128-00"`)
/// in the built-in reference catalog, or returns `nullptr` when the code is not
/// in the curated list.
[[nodiscard]] const DtcCatalogEntry* findDtcCatalogEntry(std::string_view code) noexcept;

} // namespace torquebus
