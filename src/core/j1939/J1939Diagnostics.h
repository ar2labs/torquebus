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
[[nodiscard]] std::optional<J1939Diagnostic>
j1939DecodeDiagnostic(std::uint32_t pgn,
                      std::uint8_t sourceAddress,
                      std::span<const std::uint8_t> payload,
                      J1939SpnReading reading,
                      std::uint64_t timestampNs);

} // namespace torquebus
