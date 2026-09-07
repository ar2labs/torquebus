// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What each UDS service takes, and where it goes in the bytes.
//
// The console can already send `22 F1 90`, which is enough for somebody who
// knows that 0x22 takes a two-byte identifier written big-endian. That is a
// real thing to know and it is also the exact knowledge a tool exists to spare
// people: typing `22 90 F1` produces "request out of range", which points at
// the identifier and not at the byte order, and somebody can lose a morning
// there.
//
// So a service is described rather than remembered: a name, and a list of
// fields with a kind each. The form in the Diagnostics panel is generated from
// these descriptions, and the bytes are assembled here - which means the byte
// order is written down once, tested once, and cannot drift between the panel
// and anything else that wants to build a request.
//
// **This is not a database of every identifier an ECU might have.** Knowing
// that 0xF190 is the VIN is the job of a DID database, and one manufacturer's
// 0x2001 is another's nothing. What is here is what ISO 14229 itself defines:
// the shape of each service, the sub-functions it takes, and the handful of
// identifiers the standard reserves.

#pragma once

#include "core/Result.h"
#include "core/diagnostics/UdsTypes.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// One thing the user fills in.
struct UdsField final {
    enum class Kind : std::uint8_t {
        /// A one-byte sub-function, usually chosen from `choices`.
        SubFunction,

        /// A two-byte identifier, big-endian. The field this whole file exists
        /// for.
        Identifier,

        /// One byte, written as hex.
        Byte,

        /// Three bytes, as a DTC group: FFFFFF is "all of them".
        Group,

        /// Any number of bytes: a value being written, a key, a routine's
        /// parameters.
        Bytes,
    };

    std::string_view name;
    Kind kind{Kind::Byte};

    /// What it is for, in a sentence. Shown beside the field, because a form
    /// that says "Sub-function" and nothing else is a form for people who did
    /// not need it.
    std::string_view help;

    /// What to start with. Empty for a field with no sensible default.
    std::string_view initial;

    /// True when the request is still valid without it - the value of a
    /// WriteDataByIdentifier is not, the parameters of a RoutineControl are.
    bool optional{false};
};

/// One value a sub-function field offers.
struct UdsChoice final {
    std::uint8_t value{};
    std::string_view label;
};

/// A service, as a form.
struct UdsServiceTemplate final {
    std::uint8_t service{};
    std::string_view name;

    /// What the service does, for the line above the form.
    std::string_view help;

    std::span<const UdsField> fields;

    /// Values for the first field, when it is a sub-function. Empty when the
    /// sub-function is a number rather than a choice.
    std::span<const UdsChoice> choices;
};

/// Every service this build can put a form on, in the order a menu should show
/// them: the ones somebody uses daily first.
[[nodiscard]] std::span<const UdsServiceTemplate> serviceTemplates() noexcept;

/// The template for `service`, or nullptr when there is none - which is not an
/// error. A service without a form is still sendable as hex.
[[nodiscard]] const UdsServiceTemplate* templateFor(std::uint8_t service) noexcept;

/// Assembles a request from what the form holds.
///
/// `values` is one string per field, in order, as typed. Fails naming the field
/// and what is wrong with it - "Identifier: F1 is one byte, this field takes
/// two" tells somebody what to do, where "invalid input" does not.
[[nodiscard]] Result buildRequest(const UdsServiceTemplate& service,
                                  std::span<const std::string> values,
                                  std::vector<std::uint8_t>& out);

} // namespace torquebus
