// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What travels along an Events edge: a diagnostic message, or the news that one
// did not arrive.
//
// The Events port type has existed in PortType.h since v0.7 with a note saying
// its payload would land when there was one to carry. This is it - and the
// shape it takes is the one difference between this port and the others.
//
// **A diagnostic event owns its bytes.** Every other payload in this pipeline
// is a fixed-size aggregate precisely because it moves at bus speed and an
// allocation per item would dominate the work: a CanFrame is 80 bytes with the
// payload inline, and a million of them live contiguously. A diagnostic message
// is not that traffic. It is a request and an answer, a few of them a second at
// the most frantic, each up to a firmware image long. Giving it a vector costs
// one allocation per message and saves fixing 4 kB of inline buffer to every
// event that will never use it.
//
// The batching contract is unchanged: the producing node owns the vector of
// events it published and keeps it alive until its next process().

#pragma once

#include "core/isotp/IsoTpTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

struct DiagnosticEvent final {
    enum class Kind : std::uint8_t {
        /// A complete message arrived from the bus.
        MessageReceived,

        /// A message asked for by an upstream node is now entirely on the bus.
        MessageSent,

        /// A transfer stopped. `error` says why, and `data` is empty - what had
        /// arrived of an incomplete message is not passed on, because half a
        /// response read as a whole one is worse than no response.
        TransferFailed,
    };

    Kind kind{Kind::MessageReceived};

    /// The application channel it happened on, so a two-bus setup can tell
    /// which ECU answered.
    std::uint8_t channel{0};

    /// The identifier the message arrived on, or went out on. Kept because on a
    /// functionally addressed request several ECUs answer, and the identifier
    /// is what says which one this is.
    std::uint32_t identifier{0};

    IsoTpError error{IsoTpError::None};

    /// Nanoseconds on the measurement's clock, like every other timestamp here.
    std::uint64_t timestampNs{0};

    std::vector<std::uint8_t> data;
};

/// "22 F1 90" or "22f190" as bytes.
///
/// Separators are optional and anything that is not a hex digit is one, so
/// every way somebody writes a diagnostic request works: spaces, commas, `0x`
/// prefixes on each byte. Returns false on an odd number of digits, which is
/// the one mistake that cannot be resolved by guessing - `2 2F 19 0` and
/// `22 F1 90` are different requests.
[[nodiscard]] bool parseHexBytes(std::string_view text, std::vector<std::uint8_t>& out);

/// The inverse, for a message on its way to a log line: "22 F1 90".
[[nodiscard]] std::string toHexBytes(const std::vector<std::uint8_t>& bytes,
                                     std::size_t maximum = 32);

} // namespace torquebus
