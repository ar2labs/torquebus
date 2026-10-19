// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// DM1 and DM2: what is wrong now, and what was wrong before.
//
// Two lamp bytes and then a run of four-byte trouble codes - SPN (what), FMI
// (how it is wrong), CM (which way the SPN was packed) and OC (how many times).
//
// More than one active fault does not fit in eight bytes, so a real DM1 arrives
// over BAM. That is why this file takes a payload rather than a frame: what it
// decodes comes either straight out of a single frame or out of
// J1939Transport, and it must not care which.
//
// --- The zero code ----------------------------------------------------------
//
// **A DM1 with no active fault still sends one code, and every byte of it is
// zero.** That is the standard, and it reads as SPN 0 with FMI 0 in any tool
// that does not know about it - a fault that does not exist, on every healthy
// ECU on the bus. It is filtered here, once, so that nothing downstream has to
// remember.
//
// --- The SPN conversion, declared and not guessed ----------------------------
//
// SAE J1939-73 AUG2022, section 5.7.1.14. The SPN field has been packed four
// ways over the life of the standard, and the conversion bit in a code says
// only which *half* of that history its sender belongs to:
//
//   * **0** - version 4, the current packing. Unambiguous.
//   * **1** - version 1, 2 **or** 3, and the wire does not say which.
//
// That is the whole difficulty, and it is not one better code can remove. A
// code with the bit set could be any of three packings, each of which produces
// a different number from the same bytes - and every one of those numbers looks
// like an SPN. Guessing would be the worst kind of wrong, because nothing about
// the result invites checking.
//
// So the reading is **declared**. Somebody who knows the bus says which legacy
// packing its ECUs use, and only then is a code with the bit set assembled. A
// code with the bit clear is version 4 whatever the setting says, because it is
// not ambiguous. And when nothing is declared, no number is produced at all.
//
// The four, as 5.7.1.14 defines them:
//
//   1. The SPN sent most significant bit first.
//   2. Intel format for the most significant 16 bits, with the 3 least
//      significant of the 19 packed in with the FMI.
//   3. Intel format for all 19 bits.
//   4. The same as 3, announced with the conversion bit cleared - so versions 3
//      and 4 are the same bytes and differ only in what the sender claims.
//
// The raw four bytes are carried on every code regardless of any of this,
// because somebody who knows the ECU reads the right convention out of them in
// a second and nobody reads anything out of a number converted wrongly.

#pragma once

#include "core/j1939/J1939Id.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace torquebus {

/// Bytes in one trouble code.
inline constexpr std::size_t kJ1939DtcBytes = 4U;

/// Bytes of lamp status before the first code.
inline constexpr std::size_t kJ1939LampBytes = 2U;

/// A lamp, as J1939 reports one. Two bits, and two of the four values are not
/// "on" or "off".
enum class J1939LampState : std::uint8_t {
    Off = 0U,
    On = 1U,

    /// Reserved by the standard. Seen in the wild from ECUs that write the
    /// whole byte at once without meaning to.
    Reserved = 2U,

    /// This ECU does not have this lamp, or does not drive it. Distinct from
    /// Off: a lamp that is not there is not a lamp that is dark.
    NotAvailable = 3U,
};

/// The four lamps of a diagnostic message.
struct J1939Lamps final {
    J1939LampState malfunction{J1939LampState::NotAvailable};
    J1939LampState redStop{J1939LampState::NotAvailable};
    J1939LampState amberWarning{J1939LampState::NotAvailable};
    J1939LampState protect{J1939LampState::NotAvailable};

    friend constexpr bool operator==(const J1939Lamps&, const J1939Lamps&) noexcept = default;
};

/// How a code that declares the legacy packing should be read.
///
/// A code with the conversion bit clear is version 4 under every one of these:
/// it is unambiguous, and reading it any other way would be choosing to be
/// wrong. The choice only ever applies to codes that declare the old packing.
enum class J1939SpnReading : std::uint8_t {
    /// Nothing is declared, so a code with the conversion bit set gets no
    /// number. The default, and the only setting under which this build never
    /// produces an SPN that might be a different SPN.
    Version4,

    /// Most significant bit first.
    Version1,

    /// Intel for the top 16 bits, with the low 3 packed in beside the FMI.
    Version2,

    /// Intel for all 19 bits - the same layout as version 4, from a sender that
    /// sets the conversion bit anyway.
    Version3,

    /// Assemble nothing at all, not even an unambiguous code. For reading a bus
    /// whose bytes are being checked by hand.
    RawOnly,
};

/// One trouble code.
struct J1939Dtc final {
    /// The suspect parameter number, when it was assembled. Meaningless unless
    /// `spnAssembled` is set - check that rather than testing for zero, because
    /// zero is also what the no-fault placeholder carries.
    std::uint32_t spn{0U};

    /// Failure mode identifier, 0..31. Always read: its position does not
    /// depend on the conversion.
    std::uint8_t fmi{0U};

    /// How many times this fault has occurred, 0..127. 127 means "at least
    /// 127" rather than exactly that many.
    std::uint8_t occurrenceCount{0U};

    /// The conversion bit as transmitted. Set means the sender used a packing
    /// this build does not assemble.
    bool conversionMethod{false};

    /// False when the SPN was deliberately not assembled. The raw bytes are the
    /// answer in that case, and they are always present.
    bool spnAssembled{false};

    /// The four bytes exactly as they arrived. Carried on every code, always.
    std::array<std::uint8_t, kJ1939DtcBytes> raw{};

    friend constexpr bool operator==(const J1939Dtc&, const J1939Dtc&) noexcept = default;
};

/// A decoded DM1 or DM2.
struct J1939Diagnostic final {
    /// True for DM1 (active), false for DM2 (previously active). The same bytes
    /// mean different things, and a panel that mixed them would report a
    /// repaired fault as a current one.
    bool active{true};

    std::uint8_t sourceAddress{0U};

    J1939Lamps lamps;

    /// The flash rate of each lamp, in the second byte. Same encoding.
    J1939Lamps flash;

    /// The faults. **Empty means the ECU reported none** - the all-zero
    /// placeholder has already been removed.
    std::vector<J1939Dtc> faults;

    std::uint64_t timestampNs{0U};
};

// --- The lamps of the whole bus ----------------------------------------------
//
// Every ECU reports its own lamps in its own DM1, and the instrument cluster of a vehicle lights a
// lamp when ANY of them says so. The decoded signals cannot say that: a database names a message
// once, so the DM1 of the engine, the brakes and the body all land in the one series
// "DM1.RedStopLamp", and whichever arrived last is what a reader of it sees - six ECUs sending a
// DM1 a second, five of them with every lamp dark, hide the sixth's red lamp for all but a few
// milliseconds of each second.
//
// So the J1939 block says it: when it has somewhere to put it, it writes the OR of the lamps of
// every ECU whose DM1 is still current into these four system variables, which is where a cluster
// profile (and a script, and a Dashboard widget) reads it.

inline constexpr std::string_view kJ1939LampStopVariable = "j1939.lamp_stop";
inline constexpr std::string_view kJ1939LampWarningVariable = "j1939.lamp_warning";
inline constexpr std::string_view kJ1939LampMilVariable = "j1939.lamp_mil";
inline constexpr std::string_view kJ1939LampProtectVariable = "j1939.lamp_protect";

/// How long a DM1 stays current. J1939-73 has an ECU repeat it every second, so this is three
/// missed ones: an ECU that has gone quiet - switched off, or unplugged - is not still lighting a
/// lamp.
inline constexpr std::uint64_t kJ1939DiagnosticCurrentNs = 3'000'000'000ULL;

/// True when `message` is a DM1 whose ECU is still repeating it at `nowNs`.
///
/// One rule for everything that is a statement about the bus *now*: the lamps above, and the list
/// of trouble codes below. A DM2 is never current. It is the answer to a request and is not
/// repeated, so "still being repeated" is a question it cannot answer, and a repaired fault must
/// not be reported as an active one.
///
/// `nowNs` is the newest time seen on the bus, not the clock of the machine: a log being replayed
/// has a bus time of its own.
[[nodiscard]] constexpr bool j1939DiagnosticIsCurrent(const J1939Diagnostic& message,
                                                      std::uint64_t nowNs) noexcept
{
    // A timestamp from the future, which a replay that looped makes, is as current as it gets.
    return message.active
           && !(nowNs > message.timestampNs
                && nowNs - message.timestampNs > kJ1939DiagnosticCurrentNs);
}

// --- The trouble codes of the whole bus ----------------------------------------
//
// The same thing for the codes that the lamps are for the lamps: what the bus is reporting now,
// whichever ECU is reporting it. The J1939 block works it out on every pass and hands it over with
// the rest of its view of the bus (J1939Network.h), because that is where the clock of the bus is.
// A reader cannot do it from the diagnostics alone: those are published when their content changes,
// so the timestamp in one is the time it *changed* and says nothing about whether the ECU still
// repeats it.

/// One trouble code that is active on the bus, taken out of the DM1 that carried it.
struct J1939ActiveFault final {
    std::uint8_t sourceAddress{0U};

    J1939Dtc code;

    /// The lamps of the DM1 it arrived in: what its ECU says that code lights. A lamp belongs to a
    /// message and not to a single code, so two codes of one DM1 carry the same ones.
    J1939Lamps lamps;

    friend constexpr bool operator==(const J1939ActiveFault&,
                                     const J1939ActiveFault&) noexcept = default;
};

/// Replaces the contents of `out` with every code of every DM1 that is current at `nowNs`: ECU by
/// ECU in the order `diagnostics` holds them, and within one ECU in the order it sent them. `out`
/// keeps its capacity, so a block calling this on every pass allocates only when the list grows.
void j1939CollectActiveFaults(std::span<const J1939Diagnostic> diagnostics,
                              std::uint64_t nowNs,
                              std::vector<J1939ActiveFault>& out);

[[nodiscard]] std::vector<J1939ActiveFault>
j1939ActiveFaults(std::span<const J1939Diagnostic> diagnostics, std::uint64_t nowNs);

/// Replaces the contents of `out` with the source address of every ECU whose DM1 is current at
/// `nowNs`: in the order `diagnostics` holds them, which is address order, and once each. The ECUs
/// with nothing to report are in it, and that is what it is for. They have no code for the list
/// above to name, so a healthy ECU and one that has fallen silent are the same empty list there -
/// and a panel that shows what each ECU last said could not tell them apart. `out` keeps its
/// capacity.
void j1939CollectReporting(std::span<const J1939Diagnostic> diagnostics,
                           std::uint64_t nowNs,
                           std::vector<std::uint8_t>& out);

[[nodiscard]] std::vector<std::uint8_t> j1939Reporting(std::span<const J1939Diagnostic> diagnostics,
                                                       std::uint64_t nowNs);

/// True when `pgn` is DM1 or DM2.
[[nodiscard]] constexpr bool j1939IsDiagnosticPgn(std::uint32_t pgn) noexcept
{
    return pgn == kPgnDm1 || pgn == kPgnDm2;
}

/// Splits one lamp byte into its four lamps.
[[nodiscard]] constexpr J1939Lamps j1939DecodeLamps(std::uint8_t byte) noexcept
{
    J1939Lamps lamps;
    lamps.malfunction = static_cast<J1939LampState>((byte >> 6U) & 0x03U);
    lamps.redStop = static_cast<J1939LampState>((byte >> 4U) & 0x03U);
    lamps.amberWarning = static_cast<J1939LampState>((byte >> 2U) & 0x03U);
    lamps.protect = static_cast<J1939LampState>(byte & 0x03U);

    return lamps;
}

/// Decodes one DM1 or DM2 payload.
///
/// `payload` is the whole message: two lamp bytes and then the codes. It comes
/// from a single frame when there is at most one fault, and from the transport
/// reassembler when there are more - and this cannot tell, which is the point.
///
/// Returns nothing when `pgn` is not a diagnostic message, or when the payload
/// is too short to hold the lamps. A payload that holds the lamps but no whole
/// code is a valid message with no faults.
[[nodiscard]] std::optional<J1939Diagnostic>
j1939DecodeDiagnostic(std::uint32_t pgn,
                      std::uint8_t sourceAddress,
                      std::span<const std::uint8_t> payload,
                      J1939SpnReading reading,
                      std::uint64_t timestampNs);

/// Returns the SAE J1939-73 Table 2 failure mode description for `fmi` (0..31).
/// Thirty-two values, fixed by the standard.
[[nodiscard]] std::string_view j1939FmiName(std::uint8_t fmi) noexcept;

} // namespace torquebus
