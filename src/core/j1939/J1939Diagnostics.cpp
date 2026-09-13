// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Diagnostics.h"

#include <algorithm>

namespace torquebus {
namespace {

/// A four-byte group of 0xFF is the padding at the end of a short message, not
/// a code. SPN 0x7FFFF with FMI 31 is not a fault anybody reports.
[[nodiscard]] bool isPadding(std::span<const std::uint8_t> code) noexcept
{
    return std::all_of(code.begin(), code.end(),
                       [](std::uint8_t byte) { return byte == 0xFFU; });
}

/// The placeholder a healthy ECU sends: every byte zero.
///
/// Tested on SPN and FMI rather than on all four bytes, because an occurrence
/// count that survived a repair is the one field that plausibly is not zero -
/// and a code with no SPN and no failure mode is not a fault whatever the
/// counter says.
[[nodiscard]] bool isNoFaultPlaceholder(const J1939Dtc& dtc,
                                        std::span<const std::uint8_t> code) noexcept
{
    const bool spnBitsClear = code[0] == 0U && code[1] == 0U && (code[2] & 0xE0U) == 0U;

    return spnBitsClear && dtc.fmi == 0U;
}

} // namespace

std::optional<J1939Diagnostic> j1939DecodeDiagnostic(std::uint32_t pgn,
                                                     std::uint8_t sourceAddress,
                                                     std::span<const std::uint8_t> payload,
                                                     J1939SpnReading reading,
                                                     std::uint64_t timestampNs)
{
    if (!j1939IsDiagnosticPgn(pgn)) {
        return std::nullopt;
    }

    if (payload.size() < kJ1939LampBytes) {
        return std::nullopt;
    }

    J1939Diagnostic result;
    result.active = pgn == kPgnDm1;
    result.sourceAddress = sourceAddress;
    result.lamps = j1939DecodeLamps(payload[0]);
    result.flash = j1939DecodeLamps(payload[1]);
    result.timestampNs = timestampNs;

    // Whole codes only. A trailing byte or three is padding that did not reach
    // four; reading it would build a code out of whatever followed the message.
    const std::size_t codes = (payload.size() - kJ1939LampBytes) / kJ1939DtcBytes;
    result.faults.reserve(codes);

    for (std::size_t index = 0U; index < codes; ++index) {
        const std::span<const std::uint8_t> code =
            payload.subspan(kJ1939LampBytes + index * kJ1939DtcBytes, kJ1939DtcBytes);

        if (isPadding(code)) {
            continue;
        }

        J1939Dtc dtc;
        std::copy(code.begin(), code.end(), dtc.raw.begin());

        dtc.fmi = static_cast<std::uint8_t>(code[2] & 0x1FU);
        dtc.conversionMethod = (code[3] & 0x80U) != 0U;
        dtc.occurrenceCount = static_cast<std::uint8_t>(code[3] & 0x7FU);

        // Version 4: Intel across all 19 bits, the top three in the top three
        // bits of the third byte. Also what version 3 looks like on the wire.
        const auto intelSpn = [&code]() noexcept {
            return static_cast<std::uint32_t>(code[0])
                   | (static_cast<std::uint32_t>(code[1]) << 8U)
                   | (static_cast<std::uint32_t>(code[2] & 0xE0U) << 11U);
        };

        if (reading != J1939SpnReading::RawOnly && !dtc.conversionMethod) {
            // A cleared conversion bit is version 4 and is not ambiguous, so it
            // is assembled whatever legacy packing was declared for the rest.
            dtc.spn = intelSpn();
            dtc.spnAssembled = true;
        } else if (dtc.conversionMethod) {
            switch (reading) {
            case J1939SpnReading::Version1:
                // Most significant bit first.
                dtc.spn = (static_cast<std::uint32_t>(code[0]) << 11U)
                          | (static_cast<std::uint32_t>(code[1]) << 3U)
                          | (static_cast<std::uint32_t>(code[2]) >> 5U);
                dtc.spnAssembled = true;
                break;

            case J1939SpnReading::Version2:
                // Intel for the top 16 bits; the low three sit beside the FMI.
                dtc.spn = (static_cast<std::uint32_t>(code[1]) << 11U)
                          | (static_cast<std::uint32_t>(code[0]) << 3U)
                          | (static_cast<std::uint32_t>(code[2]) >> 5U);
                dtc.spnAssembled = true;
                break;

            case J1939SpnReading::Version3:
                dtc.spn = intelSpn();
                dtc.spnAssembled = true;
                break;

            case J1939SpnReading::Version4:
            case J1939SpnReading::RawOnly:
                // Nothing was declared for the legacy packing, so this code
                // could be any of three and the raw bytes are the answer.
                break;
            }
        }

        if (isNoFaultPlaceholder(dtc, code)) {
            // The ECU is saying it is healthy. Reporting this as a fault would
            // put SPN 0 on every working ECU on the bus.
            continue;
        }

        result.faults.push_back(dtc);
    }

    return result;
}

} // namespace torquebus
