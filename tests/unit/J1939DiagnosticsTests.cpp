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

#include <gtest/gtest.h>

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

TEST(J1939DiagnosticsTests, AHealthyECUReportsNoFaultsNotAFaultNumberedZero)
{
    // The standard says a DM1 with nothing active still carries one code, and
    // every byte of it is zero. Any tool that does not know this shows SPN 0
    // FMI 0 on every working ECU on the bus.
    // Exactly what goes on the wire: two lamp bytes, the zero code, and two
    // bytes of padding to fill the frame.
    std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {noFaultCode()});
    payload.push_back(0xFFU);
    payload.push_back(0xFFU);
    ASSERT_TRUE(payload.size() == 8U);

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(message->faults.empty());
    EXPECT_TRUE(message->active);
}

TEST(J1939DiagnosticsTests, OneFaultDecodesIntoItsThreeNumbers)
{
    const std::vector<std::uint8_t> payload = payloadOf(0x40U, 0x00U, {dtcBytes(100U, 1U, 5U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 1U);

    const J1939Dtc& fault = message->faults[0];
    EXPECT_TRUE(fault.spnAssembled);
    EXPECT_TRUE(fault.spn == 100U);
    EXPECT_TRUE(fault.fmi == 1U);
    EXPECT_TRUE(fault.occurrenceCount == 5U);
    EXPECT_FALSE(fault.conversionMethod);
}

TEST(J1939DiagnosticsTests, AnSPNAboveTwoBytesKeepsItsTopThreeBits)
{
    // Those three bits live at the top of the byte the FMI shares. A decoder
    // that reads only the first two bytes collides every SPN above 65535 onto a
    // different one, and the number it produces is a real SPN belonging to
    // something else.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(0x1'FEDCU, 3U, 1U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 1U);
    EXPECT_TRUE(message->faults[0].spn == 0x1'FEDCU);
    EXPECT_TRUE(message->faults[0].fmi == 3U);
}

TEST(J1939DiagnosticsTests, ACodeDeclaringTheOtherPackingGetsNoNumberAtAll)
{
    // Read under the wrong convention the bits still produce something that
    // looks like an SPN, and nothing about it invites checking. So there is no
    // number - and the four raw bytes, which are always carried, are what
    // somebody who knows the ECU reads instead.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 5U, true)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 1U);

    const J1939Dtc& fault = message->faults[0];
    EXPECT_TRUE(fault.conversionMethod);
    EXPECT_FALSE(fault.spnAssembled);

    // The fields whose position does not depend on the packing are still read.
    EXPECT_TRUE(fault.fmi == 1U);
    EXPECT_TRUE(fault.occurrenceCount == 5U);

    // And the bytes are there, exactly as they arrived.
    EXPECT_TRUE(fault.raw[0] == 100U);
    EXPECT_TRUE(fault.raw[3] == (0x80U | 5U));
}

TEST(J1939DiagnosticsTests, TheThreeLegacyPackingsAreReadTheWayTheStandardDefinesThem)
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
    EXPECT_TRUE(spnUnder(J1939SpnReading::Version1) == 1208U);

    // Version 2: Intel for the top 16 bits, the low three beside the FMI.
    EXPECT_TRUE(spnUnder(J1939SpnReading::Version2) == ((0x97U << 11U) | (0x00U << 3U)));

    // Version 3: Intel across all 19 bits - the same layout version 4 uses.
    EXPECT_TRUE(spnUnder(J1939SpnReading::Version3) == (0x00U | (0x97U << 8U)));

    // Three readings, three different numbers, one set of bytes. That is why
    // guessing is not allowed and why nothing is declared by default.
    EXPECT_TRUE(spnUnder(J1939SpnReading::Version1) != spnUnder(J1939SpnReading::Version2));
    EXPECT_TRUE(spnUnder(J1939SpnReading::Version2) != spnUnder(J1939SpnReading::Version3));

    // And with nothing declared, no number at all.
    EXPECT_FALSE(spnUnder(J1939SpnReading::Version4).has_value());
}

TEST(J1939DiagnosticsTests, ACodeThatIsNotAmbiguousIsReadWhateverWasDeclared)
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

        ASSERT_TRUE(message.has_value());
        ASSERT_TRUE(message->faults.size() == 1U);
        EXPECT_TRUE(message->faults[0].spnAssembled);
        EXPECT_TRUE(message->faults[0].spn == 100U);
    }

    // Except raw, which is the setting for reading bytes by hand.
    const std::optional<J1939Diagnostic> raw =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::RawOnly, 1000U);

    ASSERT_TRUE(raw.has_value());
    ASSERT_TRUE(raw->faults.size() == 1U);
    EXPECT_FALSE(raw->faults[0].spnAssembled);
}

TEST(J1939DiagnosticsTests, AskingForNoAssemblyStillReportsTheFault)
{
    // For a bus whose ECUs use a packing this build does not implement. The
    // fault is real and must be shown; only the SPN is withheld.
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 5U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::RawOnly, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 1U);
    EXPECT_FALSE(message->faults[0].spnAssembled);
    EXPECT_TRUE(message->faults[0].fmi == 1U);
    EXPECT_TRUE(message->faults[0].raw[0] == 100U);
}

TEST(J1939DiagnosticsTests, SeveralFaultsArriveTogetherAsTheyDoOverTransport)
{
    // More than one active fault does not fit in eight bytes, so this payload
    // comes out of the reassembler - and the decoder cannot tell, which is why
    // it takes a payload and not a frame.
    const std::vector<std::uint8_t> payload = payloadOf(
        0x50U, 0x00U, {dtcBytes(100U, 1U, 2U), dtcBytes(110U, 3U, 1U), dtcBytes(190U, 16U, 127U)});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 3U);
    EXPECT_TRUE(message->faults[0].spn == 100U);
    EXPECT_TRUE(message->faults[1].spn == 110U);
    EXPECT_TRUE(message->faults[2].spn == 190U);
    EXPECT_TRUE(message->faults[2].occurrenceCount == 127U);
}

TEST(J1939DiagnosticsTests, PaddingAtTheEndOfAMessageIsNotAFault)
{
    // A single-frame DM1 with one fault pads to eight bytes. Four bytes of 0xFF
    // would otherwise decode as SPN 0x7FFFF with FMI 31.
    const std::vector<std::uint8_t> payload =
        payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U), {0xFFU, 0xFFU, 0xFFU, 0xFFU}});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->faults.size() == 1U);
    EXPECT_TRUE(message->faults[0].spn == 100U);
}

TEST(J1939DiagnosticsTests, ATrailingStubTooShortToBeACodeIsIgnored)
{
    // Reading it would build a code out of whatever followed the message.
    std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U)});
    payload.push_back(0xFFU);
    payload.push_back(0xFFU);

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(message->faults.size() == 1U);
}

TEST(J1939DiagnosticsTests, DM2IsWhatUsedToBeWrongAndSaysSo)
{
    // The same bytes mean different things. A panel that mixed them would
    // report a repaired fault as a current one, and send somebody to look for
    // a problem that was already fixed.
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 3U)});

    const std::optional<J1939Diagnostic> active =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);
    const std::optional<J1939Diagnostic> previous =
        j1939DecodeDiagnostic(kPgnDm2, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(previous.has_value());
    EXPECT_TRUE(active->active);
    EXPECT_FALSE(previous->active);
    EXPECT_TRUE(active->faults.size() == previous->faults.size());
}

TEST(J1939DiagnosticsTests, TheLampsAreFourTwoBitFieldsAndTwoOfTheValuesAreNotOnOrOff)
{
    // 0b01'10'11'00 - malfunction on, red stop reserved, amber not available,
    // protect off. "Not available" is a lamp the ECU does not drive, which is a
    // different statement from a lamp that is dark.
    const std::vector<std::uint8_t> payload = payloadOf(0x6CU, 0x00U, {noFaultCode()});

    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, payload, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(message->lamps.malfunction == J1939LampState::On);
    EXPECT_TRUE(message->lamps.redStop == J1939LampState::Reserved);
    EXPECT_TRUE(message->lamps.amberWarning == J1939LampState::NotAvailable);
    EXPECT_TRUE(message->lamps.protect == J1939LampState::Off);

    // The second byte is the flash rate, read the same way and kept apart.
    EXPECT_TRUE(message->flash.malfunction == J1939LampState::Off);
}

TEST(J1939DiagnosticsTests, AMessageThatIsNotDM1OrDM2IsNotDecoded)
{
    const std::vector<std::uint8_t> payload = payloadOf(0x00U, 0x00U, {dtcBytes(100U, 1U, 1U)});

    EXPECT_FALSE(
        j1939DecodeDiagnostic(0x0'FEE5U, kEngine, payload, J1939SpnReading::Version4, 1000U)
            .has_value());

    EXPECT_TRUE(j1939IsDiagnosticPgn(kPgnDm1));
    EXPECT_TRUE(j1939IsDiagnosticPgn(kPgnDm2));
    EXPECT_FALSE(j1939IsDiagnosticPgn(kPgnRequest));
}

TEST(J1939DiagnosticsTests, APayloadTooShortToHoldTheLampsIsNotAMessage)
{
    const std::vector<std::uint8_t> one{0x00U};

    EXPECT_FALSE(
        j1939DecodeDiagnostic(kPgnDm1, kEngine, one, J1939SpnReading::Version4, 1000U).has_value());

    // Two bytes and nothing else is a legal message with no faults in it - the
    // lamps are the news, and an ECU with a lamp on and no code is a real and
    // annoying thing to meet.
    const std::vector<std::uint8_t> lampsOnly{0x40U, 0x00U};
    const std::optional<J1939Diagnostic> message =
        j1939DecodeDiagnostic(kPgnDm1, kEngine, lampsOnly, J1939SpnReading::Version4, 1000U);

    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(message->faults.empty());
    EXPECT_TRUE(message->lamps.malfunction == J1939LampState::On);
}

TEST(J1939DiagnosticsTests, TheOccurrenceCountDoesNotStealTheConversionBit)
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

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(first->faults.size() == 1U);
    ASSERT_TRUE(second->faults.size() == 1U);

    EXPECT_TRUE(first->faults[0].occurrenceCount == 127U);
    EXPECT_TRUE(second->faults[0].occurrenceCount == 127U);
    EXPECT_TRUE(first->faults[0].spnAssembled);
    EXPECT_FALSE(second->faults[0].spnAssembled);
}

TEST(J1939DiagnosticsTests, FmiNumbersMapToSaeJ1939Table2Descriptions)
{
    EXPECT_EQ(j1939FmiName(0U),
              "Data valid but above normal operational range - most severe level");
    EXPECT_EQ(j1939FmiName(3U), "Voltage above normal, or shorted to high source");
    EXPECT_EQ(j1939FmiName(4U), "Voltage below normal, or shorted to low source");
    EXPECT_EQ(j1939FmiName(19U), "Received network data in error");
    EXPECT_EQ(j1939FmiName(25U), "Reserved for SAE assignment");
    EXPECT_EQ(j1939FmiName(31U), "Condition exists");
}

// ---------------------------------------------------------------------------
// The codes of the whole bus
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint8_t kBrakes = 0x0BU;
constexpr std::uint8_t kBody = 0x21U;
constexpr std::uint64_t kSecond = 1'000'000'000ULL;

// Byte 1 of a DM1: protect 1-2, amber warning 3-4, red stop 5-6, malfunction 7-8, and 01 is on.
constexpr std::uint8_t kRedStop = 0x10U;
constexpr std::uint8_t kAmberWarning = 0x04U;

/// A decoded DM1 (or DM2) from `source` at `atNs`, carrying the given codes under the given lamps.
[[nodiscard]] J1939Diagnostic messageFrom(std::uint32_t pgn,
                                          std::uint8_t source,
                                          std::uint8_t lamps,
                                          const std::vector<std::vector<std::uint8_t>>& codes,
                                          std::uint64_t atNs)
{
    const std::optional<J1939Diagnostic> message = j1939DecodeDiagnostic(
        pgn, source, payloadOf(lamps, 0xFFU, codes), J1939SpnReading::Version4, atNs);

    EXPECT_TRUE(message.has_value());

    return message.value_or(J1939Diagnostic{});
}

} // namespace

TEST(J1939DiagnosticsTests, ADm1IsCurrentUntilItsEcuHasBeenSilentForThreeRepeats)
{
    const J1939Diagnostic dm1 = messageFrom(kPgnDm1, kEngine, kRedStop, {}, 10U * kSecond);

    EXPECT_TRUE(j1939DiagnosticIsCurrent(dm1, 10U * kSecond));
    EXPECT_TRUE(j1939DiagnosticIsCurrent(dm1, 12U * kSecond));

    // Exactly three seconds is not yet past them; anything more is.
    EXPECT_TRUE(j1939DiagnosticIsCurrent(dm1, 10U * kSecond + kJ1939DiagnosticCurrentNs));
    EXPECT_FALSE(j1939DiagnosticIsCurrent(dm1, 10U * kSecond + kJ1939DiagnosticCurrentNs + 1U));

    // A bus time before the message, which a log that looped round to its start makes, is as
    // current as it gets.
    EXPECT_TRUE(j1939DiagnosticIsCurrent(dm1, 1U * kSecond));
}

TEST(J1939DiagnosticsTests, ADm2IsNeverCurrentBecauseItIsNotRepeated)
{
    // An answer to a request. A repaired fault reported as an active one is what this prevents.
    const J1939Diagnostic dm2 =
        messageFrom(kPgnDm2, kEngine, kRedStop, {dtcBytes(100U, 1U, 3U)}, 10U * kSecond);

    EXPECT_FALSE(j1939DiagnosticIsCurrent(dm2, 10U * kSecond));
    EXPECT_FALSE(j1939DiagnosticIsCurrent(dm2, 11U * kSecond));
}

TEST(J1939DiagnosticsTests, TheActiveFaultsOfTheBusAreEveryCodeOfEveryCurrentDm1)
{
    // The engine reports two codes, the body none, and the brakes one - in address order, as the
    // block holds them, and each code carries the lamps of the message it arrived in.
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(
            kPgnDm1, kEngine, kRedStop, {dtcBytes(100U, 1U, 3U), dtcBytes(110U, 0U, 1U)}, kSecond),
        messageFrom(kPgnDm1, kBrakes, kAmberWarning, {dtcBytes(1043U, 12U, 127U)}, kSecond),
        messageFrom(kPgnDm1, kBody, 0x00U, {noFaultCode()}, kSecond),
    };

    const std::vector<J1939ActiveFault> faults = j1939ActiveFaults(diagnostics, kSecond);

    ASSERT_EQ(faults.size(), 3U);

    EXPECT_EQ(faults[0].sourceAddress, kEngine);
    EXPECT_EQ(faults[0].code.spn, 100U);
    EXPECT_EQ(faults[0].code.fmi, 1U);
    EXPECT_EQ(faults[0].code.occurrenceCount, 3U);
    EXPECT_TRUE(faults[0].lamps.redStop == J1939LampState::On);

    // Within an ECU, the order it sent them in; both share the lamps of their message.
    EXPECT_EQ(faults[1].sourceAddress, kEngine);
    EXPECT_EQ(faults[1].code.spn, 110U);
    EXPECT_TRUE(faults[1].lamps == faults[0].lamps);

    EXPECT_EQ(faults[2].sourceAddress, kBrakes);
    EXPECT_EQ(faults[2].code.spn, 1043U);
    EXPECT_EQ(faults[2].code.occurrenceCount, 127U);
    EXPECT_TRUE(faults[2].lamps.amberWarning == J1939LampState::On);
    EXPECT_TRUE(faults[2].lamps.redStop != J1939LampState::On);
}

TEST(J1939DiagnosticsTests, AnEcuThatHasGoneQuietAndAnswersToARequestAreNotOnTheList)
{
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(kPgnDm1, kEngine, kRedStop, {dtcBytes(100U, 1U, 3U)}, kSecond),
        messageFrom(kPgnDm2, kEngine, kRedStop, {dtcBytes(111U, 2U, 1U)}, 9U * kSecond),
        messageFrom(kPgnDm1, kBrakes, kAmberWarning, {dtcBytes(1043U, 12U, 1U)}, 9U * kSecond),
    };

    // Nine seconds in: the engine has not repeated its DM1 since the first second, the DM2 is
    // history and not news, and the brakes are still talking.
    const std::vector<J1939ActiveFault> faults = j1939ActiveFaults(diagnostics, 9U * kSecond);

    ASSERT_EQ(faults.size(), 1U);
    EXPECT_EQ(faults[0].sourceAddress, kBrakes);
    EXPECT_EQ(faults[0].code.spn, 1043U);
}

TEST(J1939DiagnosticsTests, NothingReportedIsAnEmptyListAndCollectingKeepsItsRoom)
{
    EXPECT_TRUE(j1939ActiveFaults({}, 0U).empty());

    // Collected into a list that is reused: it is replaced and not appended to, and the capacity it
    // grew stays, which is what lets the block do this on every pass without allocating.
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(
            kPgnDm1, kEngine, kRedStop, {dtcBytes(100U, 1U, 3U), dtcBytes(110U, 0U, 1U)}, kSecond),
    };

    std::vector<J1939ActiveFault> faults;
    j1939CollectActiveFaults(diagnostics, kSecond, faults);
    ASSERT_EQ(faults.size(), 2U);

    const std::size_t capacity = faults.capacity();
    j1939CollectActiveFaults(diagnostics, kSecond, faults);

    EXPECT_EQ(faults.size(), 2U);
    EXPECT_EQ(faults.capacity(), capacity);

    j1939CollectActiveFaults({}, kSecond, faults);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(faults.capacity(), capacity);
}

TEST(J1939DiagnosticsTests, TheEcusReportingAreEveryEcuWithACurrentDm1HealthyOnesIncluded)
{
    // The list of codes cannot say which ECUs are still talking: a healthy one has no code to be
    // listed by, and the same empty list is what an ECU that has gone quiet leaves behind. So the
    // block says it separately - here the engine has a code, the body is healthy and talking, the
    // brakes were healthy and have not repeated themselves since the first second.
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(kPgnDm1, kEngine, kRedStop, {dtcBytes(100U, 1U, 3U)}, 9U * kSecond),
        messageFrom(kPgnDm2, kEngine, kRedStop, {dtcBytes(111U, 2U, 1U)}, 9U * kSecond),
        messageFrom(kPgnDm1, kBrakes, 0x00U, {noFaultCode()}, kSecond),
        messageFrom(kPgnDm1, kBody, 0x00U, {noFaultCode()}, 9U * kSecond),
    };

    // Two of the three that spoke, in address order and once each: the engine's DM2 is an answer to
    // a request and is not repeated, so it is not a reason to be listed twice - or at all.
    const std::vector<std::uint8_t> reporting = j1939Reporting(diagnostics, 9U * kSecond);
    ASSERT_EQ(reporting.size(), 2U);
    EXPECT_EQ(reporting[0], kEngine);
    EXPECT_EQ(reporting[1], kBody);

    // And the list of codes is one entry long, which is why it could not have said so.
    EXPECT_EQ(j1939ActiveFaults(diagnostics, 9U * kSecond).size(), 1U);
}

TEST(J1939DiagnosticsTests, AnEcuLeavesTheReportingWhenItsDm1IsOlderThanTheRuleAllows)
{
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(kPgnDm1, kBody, 0x00U, {noFaultCode()}, 10U * kSecond),
    };

    EXPECT_EQ(j1939Reporting(diagnostics, 10U * kSecond + kJ1939DiagnosticCurrentNs).size(), 1U);
    EXPECT_TRUE(
        j1939Reporting(diagnostics, 10U * kSecond + kJ1939DiagnosticCurrentNs + 1U).empty());
}

TEST(J1939DiagnosticsTests, NobodyReportingIsAnEmptyListAndCollectingKeepsItsRoom)
{
    EXPECT_TRUE(j1939Reporting({}, 0U).empty());

    // Replaced and not appended to, with the capacity it grew kept: the block does this on every
    // pass, and a pass that allocates is one that can miss its deadline.
    const std::vector<J1939Diagnostic> diagnostics{
        messageFrom(kPgnDm1, kEngine, 0x00U, {noFaultCode()}, kSecond),
        messageFrom(kPgnDm1, kBody, 0x00U, {noFaultCode()}, kSecond),
    };

    std::vector<std::uint8_t> reporting;
    j1939CollectReporting(diagnostics, kSecond, reporting);
    ASSERT_EQ(reporting.size(), 2U);

    const std::size_t capacity = reporting.capacity();
    j1939CollectReporting(diagnostics, kSecond, reporting);

    EXPECT_EQ(reporting.size(), 2U);
    EXPECT_EQ(reporting.capacity(), capacity);

    j1939CollectReporting({}, kSecond, reporting);
    EXPECT_TRUE(reporting.empty());
    EXPECT_EQ(reporting.capacity(), capacity);
}
