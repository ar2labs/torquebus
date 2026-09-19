// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// DM1 and DM2.
//
// Two failures here would be believed rather than noticed. The all-zero code a
// healthy ECU sends, read as a fault, puts SPN 0 on every working ECU on the
// bus. And an SPN assembled under the wrong packing is still a number that
// looks like an SPN - so the cases below check that the wrong packing produces
// *no* number rather than a plausible one.

#include "core/j1939/J1939Diagnostics.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint8_t kEngine = 0x00U;

/// One trouble code, packed the way the current standard packs it.
[[nodiscard]] std::vector<std::uint8_t> dtcBytes(std::uint32_t spn,
                                                 std::uint8_t fmi,
                                                 std::uint8_t occurrences,
                                                 bool conversionMethod = false)
{
    return {
        static_cast<std::uint8_t>(spn & 0xFFU),
        static_cast<std::uint8_t>((spn >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((((spn >> 16U) & 0x07U) << 5U) | (fmi & 0x1FU)),
        static_cast<std::uint8_t>((conversionMethod ? 0x80U : 0x00U) | (occurrences & 0x7FU)),
    };
}

/// A whole DM payload: two lamp bytes and then the codes.
[[nodiscard]] std::vector<std::uint8_t> payloadOf(
    std::uint8_t lamps, std::uint8_t flash, const std::vector<std::vector<std::uint8_t>>& codes)
{
    std::vector<std::uint8_t> payload{lamps, flash};
    for (const std::vector<std::uint8_t>& code : codes) {
        payload.insert(payload.end(), code.begin(), code.end());
    }

    return payload;
}

/// What a healthy ECU sends: one code, every byte zero.
[[nodiscard]] std::vector<std::uint8_t> noFaultCode()
{
    return {0x00U, 0x00U, 0x00U, 0x00U};
}

} // namespace

TEST_CASE("A healthy ECU reports no faults, not a fault numbered zero", "[j1939][dm]")
{
    // The standard says a DM1 with nothing active still carries one code, and
    // every byte of it is zero. Any tool that does not know this shows SPN 0
    // FMI 0 on every working ECU on the bus.
    // Exactly what goes on the wire: two lamp bytes, the zero code, and two
    // bytes of padding to fill the frame.
    std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {noFaultCode()});
    payload.push_back(0xFFU);
    payload.push_back(0xFFU);
    REQUIRE(payload.size() == 8U);

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    CHECK(message->faults.empty());
    CHECK(message->active);
}

TEST_CASE("One fault decodes into its three numbers", "[j1939][dm]")
{
    const std::vector<std::uint8_t> payload = payloadOf(0x40U, 0x00U, {dtcBytes(100U, 1U, 5U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 1U);

    const J1939Dtc& fault = message->faults[0];
    CHECK(fault.spnAssembled);
    CHECK(fault.spn == 100U);
    CHECK(fault.fmi == 1U);
    CHECK(fault.occurrenceCount == 5U);
    CHECK_FALSE(fault.conversionMethod);
}

TEST_CASE("An SPN above two bytes keeps its top three bits", "[j1939][dm]")
{
    // Those three bits live at the top of the byte the FMI shares. A decoder
    // that reads only the first two bytes collides every SPN above 65535 onto a
    // different one, and the number it produces is a real SPN belonging to
    // something else.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(0x1'FEDCU, 3U, 1U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 1U);
    CHECK(message->faults[0].spn == 0x1'FEDCU);
    CHECK(message->faults[0].fmi == 3U);
}

TEST_CASE("A code declaring the other packing gets no number at all", "[j1939][dm]")
{
    // Read under the wrong convention the bits still produce something that
    // looks like an SPN, and nothing about it invites checking. So there is no
    // number - and the four raw bytes, which are always carried, are what
    // somebody who knows the ECU reads instead.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 5U, true)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 1U);

    const J1939Dtc& fault = message->faults[0];
    CHECK(fault.conversionMethod);
    CHECK_FALSE(fault.spnAssembled);

    // The fields whose position does not depend on the packing are still read.
    CHECK(fault.fmi == 1U);
    CHECK(fault.occurrenceCount == 5U);

    // And the bytes are there, exactly as they arrived.
    CHECK(fault.raw[0] == 100U);
    CHECK(fault.raw[3] == (0x80U | 5U));
}

TEST_CASE("The three legacy packings are read the way the standard defines them", "[j1939][dm]")
{
    // J1939-73 AUG2022 section 5.7.1.14. A conversion bit of one means the SPN
    // is in version 1, 2 or 3 format and the wire does not say which - so the
    // same three bytes are three different SPNs, and each of them looks real.
    //
    // The bytes here are from the standard own worked example: DM22 clearing
    // "SPN 1208, FMI 3" carries 00 97 03, and reading it most significant bit
    // first gives 1208 exactly. That is version 1, and it is what pins this
    // case to the document rather than to an implementation.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {{0x00U, 0x97U, 0x03U | 0x00U, 0x80U | 1U}});

    const auto spnUnder = [&payload](J1939SpnReading reading) -> std::optional<std::uint32_t> {
        const std::optional<J1939Diagnostic> message =
            j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, reading, 1000U);

        if (!message.has_value() || message->faults.size() != 1U
            || !message->faults[0].spnAssembled) {
            return std::nullopt;
        }

        return message->faults[0].spn;
    };

    // Version 1: most significant bit first. The standard own example.
    CHECK(spnUnder(J1939SpnReading::Version1) == 1208U);

    // Version 2: Intel for the top 16 bits, the low three beside the FMI.
    CHECK(spnUnder(J1939SpnReading::Version2) == ((0x97U << 11U) | (0x00U << 3U)));

    // Version 3: Intel across all 19 bits - the same layout version 4 uses.
    CHECK(spnUnder(J1939SpnReading::Version3) == (0x00U | (0x97U << 8U)));

    // Three readings, three different numbers, one set of bytes. That is why
    // guessing is not allowed and why nothing is declared by default.
    CHECK(spnUnder(J1939SpnReading::Version1) != spnUnder(J1939SpnReading::Version2));
    CHECK(spnUnder(J1939SpnReading::Version2) != spnUnder(J1939SpnReading::Version3));

    // And with nothing declared, no number at all.
    CHECK_FALSE(spnUnder(J1939SpnReading::Version4).has_value());
}

TEST_CASE("A code that is not ambiguous is read whatever was declared", "[j1939][dm]")
{
    // A cleared conversion bit is version 4 and can only be version 4. Reading
    // it as a legacy packing because somebody declared one for the codes that
    // need it would be choosing to be wrong about a code that was clear.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 5U, false)});

    for (const J1939SpnReading reading : {J1939SpnReading::Version1,
                                          J1939SpnReading::Version2,
                                          J1939SpnReading::Version3,
                                          J1939SpnReading::Version4}) {
        const std::optional<J1939Diagnostic> message =
            j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, reading, 1000U);

        REQUIRE(message.has_value());
        REQUIRE(message->faults.size() == 1U);
        CHECK(message->faults[0].spnAssembled);
        CHECK(message->faults[0].spn == 100U);
    }

    // Except raw, which is the setting for reading bytes by hand.
    const std::optional<J1939Diagnostic> raw =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::RawOnly, 1000U);

    REQUIRE(raw.has_value());
    REQUIRE(raw->faults.size() == 1U);
    CHECK_FALSE(raw->faults[0].spnAssembled);
}

TEST_CASE("Asking for no assembly still reports the fault", "[j1939][dm]")
{
    // For a bus whose ECUs use a packing this build does not implement. The
    // fault is real and must be shown; only the SPN is withheld.
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 5U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::RawOnly, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 1U);
    CHECK_FALSE(message->faults[0].spnAssembled);
    CHECK(message->faults[0].fmi == 1U);
    CHECK(message->faults[0].raw[0] == 100U);
}

TEST_CASE("Several faults arrive together, as they do over transport", "[j1939][dm]")
{
    // More than one active fault does not fit in eight bytes, so this payload
    // comes out of the reassembler - and the decoder cannot tell, which is why
    // it takes a payload and not a frame.
    const std::vector<std::uint8_t> payload = payloadOf(
        0x50U, 0x00U, {dtcBytes(100U, 1U, 2U), dtcBytes(110U, 3U, 1U), dtcBytes(190U, 16U, 127U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 3U);
    CHECK(message->faults[0].spn == 100U);
    CHECK(message->faults[1].spn == 110U);
    CHECK(message->faults[2].spn == 190U);
    CHECK(message->faults[2].occurrenceCount == 127U);
}

TEST_CASE("Padding at the end of a message is not a fault", "[j1939][dm]")
{
    // A single-frame DM1 with one fault pads to eight bytes. Four bytes of 0xFF
    // would otherwise decode as SPN 0x7FFFF with FMI 31.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U), {0xFFU, 0xFFU, 0xFFU, 0xFFU}});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    REQUIRE(message->faults.size() == 1U);
    CHECK(message->faults[0].spn == 100U);
}

TEST_CASE("A trailing stub too short to be a code is ignored", "[j1939][dm]")
{
    // Reading it would build a code out of whatever followed the message.
    std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U)});
    payload.push_back(0xFFU);
    payload.push_back(0xFFU);

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    CHECK(message->faults.size() == 1U);
}

TEST_CASE("DM2 is what used to be wrong, and says so", "[j1939][dm]")
{
    // The same bytes mean different things. A panel that mixed them would
    // report a repaired fault as a current one, and send somebody to look for
    // a problem that was already fixed.
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 3U)});

    const std::optional<J1939Diagnostic> active =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);
    const std::optional<J1939Diagnostic> previous =
        j1939DecodeDiagnostic(kPgnDm2, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(active.has_value());
    REQUIRE(previous.has_value());
    CHECK(active->active);
    CHECK_FALSE(previous->active);
    CHECK(active->faults.size() == previous->faults.size());
}

TEST_CASE("The lamps are four two-bit fields, and two of the values are not on or off",
          "[j1939][dm]")
{
    // 0b01'10'11'00 - malfunction on, red stop reserved, amber not available,
    // protect off. "Not available" is a lamp the ECU does not drive, which is a
    // different statement from a lamp that is dark.
    const std::vector<std::uint8_t> payload = payloadOf(0x6CU, 0x00U, {noFaultCode()});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    CHECK(message->lamps.malfunction == J1939LampState::On);
    CHECK(message->lamps.redStop == J1939LampState::Reserved);
    CHECK(message->lamps.amberWarning == J1939LampState::NotAvailable);
    CHECK(message->lamps.protect == J1939LampState::Off);

    // The second byte is the flash rate, read the same way and kept apart.
    CHECK(message->flash.malfunction == J1939LampState::Off);
}

TEST_CASE("A message that is not DM1 or DM2 is not decoded", "[j1939][dm]")
{
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U)});

    CHECK_FALSE(j1939DecodeDiagnostic(0x0'FEE5U, kEngine, payload, J1939SpnReading::Version4, 1000U)
                    .has_value());

    CHECK(j1939IsDiagnosticPgn(kPgnDm1));
    CHECK(j1939IsDiagnosticPgn(kPgnDm2));
    CHECK_FALSE(j1939IsDiagnosticPgn(kPgnRequest));
}

TEST_CASE("A payload too short to hold the lamps is not a message", "[j1939][dm]")
{
    const std::vector<std::uint8_t> one{0x00U};

    CHECK_FALSE(
        j1939DecodeDiagnostic(kPgnDm1, kEngine, one, J1939SpnReading::Version4, 1000U).has_value());

    // Two bytes and nothing else is a legal message with no faults in it - the
    // lamps are the news, and an ECU with a lamp on and no code is a real and
    // annoying thing to meet.
    const std::vector<std::uint8_t> lampsOnly{0x40U, 0x00U};
    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, lampsOnly, J1939SpnReading::Version4, 1000U);

    REQUIRE(message.has_value());
    CHECK(message->faults.empty());
    CHECK(message->lamps.malfunction == J1939LampState::On);
}

TEST_CASE("The occurrence count does not steal the conversion bit", "[j1939][dm]")
{
    // Seven bits and one. A count of 127 with the bit clear, and the same count
    // with it set, differ in the one place that decides whether the SPN gets
    // assembled at all.
    const std::vector<std::uint8_t> plain =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 127U, false)});
    const std::vector<std::uint8_t> flagged =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 127U, true)});

    const auto first =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, plain, J1939SpnReading::Version4, 1000U);
    const auto second =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, flagged, J1939SpnReading::Version4, 1000U);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->faults.size() == 1U);
    REQUIRE(second->faults.size() == 1U);

    CHECK(first->faults[0].occurrenceCount == 127U);
    CHECK(second->faults[0].occurrenceCount == 127U);
    CHECK(first->faults[0].spnAssembled);
    CHECK_FALSE(second->faults[0].spnAssembled);
}
