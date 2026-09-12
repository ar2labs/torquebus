// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The J1939 NAME: 64 bits saying what an ECU *is*, independently of where it
// answers.
//
// An address is a seat, and seats are taken on a first-come basis at power-up.
// The NAME is the identity that argues for a seat, and it is what makes an
// address mean anything: two machines with the same layout hand out the same
// addresses in a different order, and the only stable way to say "the engine"
// is the NAME that claimed it.
//
// The layout, from the least significant bit up - which is also the order the
// eight data bytes of an Address Claimed arrive in:
//
//   bits 0..20    identity number            21 bits, the serial number
//   bits 21..31   manufacturer code          11 bits, assigned by SAE
//   bits 32..34   ECU instance                3 bits
//   bits 35..39   function instance           5 bits
//   bits 40..47   function                    8 bits
//   bit  48       reserved
//   bits 49..55   vehicle system              7 bits
//   bits 56..59   vehicle system instance     4 bits
//   bits 60..62   industry group              3 bits
//   bit  63       arbitrary address capable
//
// **Lower NAME wins a contest for an address.** That the arbitrary-address
// capable bit sits at the very top is the point rather than an accident: a
// device able to move somewhere else loses every tie, and moves.
//
// --- What this file deliberately does not do --------------------------------
//
// It does not turn a function number into a function name, and it does not turn
// a manufacturer code into a manufacturer.
//
// Functions 0..127 are industry-group independent; 128..255 mean different
// things depending on the industry group *and* the vehicle system, so the same
// number is a different device on a tractor and on a boat. The manufacturer
// list is a registry of roughly two thousand entries that gains more every
// year. Both belong in a data file that can be corrected without a rebuild, and
// shipping a half-remembered table would produce exactly the failure this
// milestone is written to avoid: a confident label that is wrong, which is
// worse than the number it replaced, because a number invites somebody to look
// it up and a wrong name does not.
//
// The industry group is the exception, and is named below: eight values, fixed
// since the standard was written.

#pragma once

#include "core/can/CanFrame.h"
#include "core/j1939/J1939Id.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace torquebus {

/// Number of data bytes an Address Claimed carries. Exactly eight, always.
inline constexpr std::uint8_t kJ1939NameBytes = 8U;

/// A NAME with its fields separated.
struct J1939Name final {
    /// This ECU can move to another address if it loses a contest. Being the
    /// top bit of the NAME, it also makes the ECU lose every tie it could have
    /// won - which is the intended order: whoever can move, moves.
    bool arbitraryAddressCapable{false};

    /// 0..7. Decides what `vehicleSystem` means, and what a function above 127
    /// means. See j1939IndustryGroupName().
    std::uint8_t industryGroup{0U};

    /// 0..15. Which of several identical vehicle systems this one is.
    std::uint8_t vehicleSystemInstance{0U};

    /// 0..127, read against the industry group.
    std::uint8_t vehicleSystem{0U};

    /// Kept rather than dropped. A NAME with this set is unusual and worth
    /// seeing, and round-tripping a NAME that silently normalised a bit would
    /// change an ECU identity while claiming to have copied it.
    bool reserved{false};

    /// 0..255. Not named here; see the note at the top of this file.
    std::uint8_t function{0U};

    /// 0..31. Which of several identical functions this one is - the second
    /// engine of two, the third axle of four.
    std::uint8_t functionInstance{0U};

    /// 0..7.
    std::uint8_t ecuInstance{0U};

    /// 0..2047, assigned by SAE. Not named here.
    std::uint16_t manufacturerCode{0U};

    /// 0..2097151. Together with the manufacturer code this is what makes a
    /// NAME unique on a bus.
    std::uint32_t identityNumber{0U};

    /// Back to the 64-bit number, which is the form arbitration compares.
    [[nodiscard]] constexpr std::uint64_t value() const noexcept
    {
        return (static_cast<std::uint64_t>(identityNumber & 0x1F'FFFFU))
               | (static_cast<std::uint64_t>(manufacturerCode & 0x7FFU) << 21U)
               | (static_cast<std::uint64_t>(ecuInstance & 0x07U) << 32U)
               | (static_cast<std::uint64_t>(functionInstance & 0x1FU) << 35U)
               | (static_cast<std::uint64_t>(function) << 40U)
               | (static_cast<std::uint64_t>(reserved) << 48U)
               | (static_cast<std::uint64_t>(vehicleSystem & 0x7FU) << 49U)
               | (static_cast<std::uint64_t>(vehicleSystemInstance & 0x0FU) << 56U)
               | (static_cast<std::uint64_t>(industryGroup & 0x07U) << 60U)
               | (static_cast<std::uint64_t>(arbitraryAddressCapable) << 63U);
    }

    /// True when this NAME takes the address from `other` in a contest.
    ///
    /// The whole 64-bit value is compared, not a field at a time: that is what
    /// the standard says and it is also the only rule under which two ECUs,
    /// deciding independently, always reach the same answer.
    [[nodiscard]] constexpr bool winsAgainst(const J1939Name& other) const noexcept
    {
        return value() < other.value();
    }
};

/// Splits a 64-bit NAME into its fields. Cannot fail: every bit pattern is a
/// NAME, including the ones nobody should have shipped.
[[nodiscard]] constexpr J1939Name j1939DecodeName(std::uint64_t name) noexcept
{
    J1939Name result;
    result.identityNumber = static_cast<std::uint32_t>(name & 0x1F'FFFFU);
    result.manufacturerCode = static_cast<std::uint16_t>((name >> 21U) & 0x7FFU);
    result.ecuInstance = static_cast<std::uint8_t>((name >> 32U) & 0x07U);
    result.functionInstance = static_cast<std::uint8_t>((name >> 35U) & 0x1FU);
    result.function = static_cast<std::uint8_t>((name >> 40U) & 0xFFU);
    result.reserved = ((name >> 48U) & 0x01U) != 0U;
    result.vehicleSystem = static_cast<std::uint8_t>((name >> 49U) & 0x7FU);
    result.vehicleSystemInstance = static_cast<std::uint8_t>((name >> 56U) & 0x0FU);
    result.industryGroup = static_cast<std::uint8_t>((name >> 60U) & 0x07U);
    result.arbitraryAddressCapable = ((name >> 63U) & 0x01U) != 0U;

    return result;
}

/// Reads the eight payload bytes of an Address Claimed as a 64-bit NAME.
///
/// Least significant byte first, which is how J1939 puts multi-byte values on
/// the wire. The caller has already established that this is a claim.
[[nodiscard]] std::uint64_t j1939NameBits(const CanFrame& frame) noexcept;

/// The NAME an Address Claimed carries, or nothing when the frame is not one.
///
/// Rejects a claim shorter than eight bytes rather than zero-extending it. A
/// short claim is a malformed frame, and padding it produces a NAME with an
/// identity number that nobody transmitted - which would then win or lose
/// contests, and be shown as an ECU identity in a panel.
[[nodiscard]] std::optional<J1939Name> j1939NameFromClaim(const CanFrame& frame) noexcept;

/// True when this frame is an ECU saying it could not get an address.
///
/// An Address Claimed sent *from* the null address is the standard way to
/// announce defeat. It carries a real NAME and must not be filed as a claim on
/// address 254: nobody owns 254, and treating it as an occupant would put a
/// phantom ECU in the network table.
[[nodiscard]] bool j1939IsCannotClaimAddress(const CanFrame& frame) noexcept;

/// The industry group by name. Eight values, fixed since the standard was
/// written, which is why this one is safe to hold in code.
[[nodiscard]] std::string_view j1939IndustryGroupName(std::uint8_t industryGroup) noexcept;

} // namespace torquebus
