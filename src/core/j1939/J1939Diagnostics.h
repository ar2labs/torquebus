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
// The SPN field has been packed more than one way over the life of the
// standard, and each DTC carries a bit saying which way its sender used. Read
// under the wrong convention the bits still produce a number, and the number
// still looks like an SPN - which is the worst kind of wrong, because nothing
// about it invites checking.
//
// So: this build assembles the current packing, the one a cleared conversion
// bit means. When a code says it used the other one, the SPN is **not
// assembled at all** and the four raw bytes stand in its place. That is a
// deliberate gap rather than a missing feature - implementing a packing from
// memory is how a confident wrong number gets shipped - and the raw bytes are
// carried on every code regardless, because somebody who knows the ECU reads
// the right convention out of them in a second and nobody reads anything out of
// a number that was already converted wrongly.

#pragma once

#include "core/j1939/J1939Id.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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
};

/// How this build should read the SPN field.
enum class J1939SpnReading : std::uint8_t {
    /// Assemble the current packing, which a cleared conversion bit declares.
    /// A code declaring the other packing is left unassembled.
    Version4,

    /// Assemble nothing; carry the raw bytes only. For a bus whose ECUs use a
    /// packing this build does not implement, where a number would be worse
    /// than no number.
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
[[nodiscard]] std::optional<J1939Diagnostic> j1939DecodeDiagnostic(
    std::uint32_t pgn,
    std::uint8_t sourceAddress,
    std::span<const std::uint8_t> payload,
    J1939SpnReading reading,
    std::uint64_t timestampNs);

} // namespace torquebus
