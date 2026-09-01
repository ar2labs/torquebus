// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Compile-time verification of the frame model, plus the few helpers that are
// not worth inlining into every translation unit.

#include "core/can/CanFrame.h"

#include <algorithm>
#include <string>

namespace torquebus {

// ---------------------------------------------------------------------------
// Invariants checked by the compiler, so a regression cannot reach a test run
// ---------------------------------------------------------------------------

static_assert(payloadLengthFromDlc(8, false) == 8);
static_assert(payloadLengthFromDlc(15, false) == 8, "Classic CAN caps at 8 bytes");
static_assert(payloadLengthFromDlc(15, true) == 64);
static_assert(payloadLengthFromDlc(9, true) == 12);
static_assert(payloadLengthFromDlc(13, true) == 32);

static_assert(dlcFromPayloadLength(8, true) == 8);
static_assert(dlcFromPayloadLength(9, true) == 9, "9 bytes must round up to the 12-byte step");
static_assert(dlcFromPayloadLength(64, true) == 15);
static_assert(dlcFromPayloadLength(20, false) == 8);

static_assert(isValidIdentifier(0x7FF, CanFrameFormat::Standard));
static_assert(!isValidIdentifier(0x800, CanFrameFormat::Standard));
static_assert(isValidIdentifier(0x1FFF'FFFF, CanFrameFormat::Extended));
static_assert(!isValidIdentifier(0x2000'0000, CanFrameFormat::Extended));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

} // namespace

/// Formats the payload as space-separated uppercase hex, e.g. "01 02 FF".
/// Used by the trace, the console and the .asc exporter.
std::string toHexString(const CanFrame& frame)
{
    if (frame.length == 0) {
        return {};
    }

    const std::size_t count = std::min(static_cast<std::size_t>(frame.length), kMaxCanPayload);

    std::string result;
    result.reserve(count * 3U - 1U);

    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            result.push_back(' ');
        }
        const std::uint8_t byte = frame.data[index];
        result.push_back(kHexDigits[byte >> 4U]);
        result.push_back(kHexDigits[byte & 0x0FU]);
    }

    return result;
}

/// Formats the identifier the way every automotive tool does: three hex digits
/// for standard frames, eight for extended, always uppercase and zero padded.
std::string toIdentifierString(const CanFrame& frame)
{
    const int digits = frame.isExtended() ? 8 : 3;

    std::string result(static_cast<std::size_t>(digits), '0');

    std::uint32_t value = frame.identifier;
    for (int position = digits - 1; position >= 0; --position) {
        result[static_cast<std::size_t>(position)] = kHexDigits[value & 0x0FU];
        value >>= 4U;
    }

    return result;
}

} // namespace torquebus
