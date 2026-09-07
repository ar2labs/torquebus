// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// ISO 14229 (UDS): the language spoken over ISO-TP.
//
// A UDS message is a service byte followed by whatever that service takes. The
// protocol is small; what is not small is the vocabulary, and that is what this
// file is for. A tool that shows `7F 22 31` has told the user nothing they
// could not read off the bus themselves. A tool that shows
// "ReadDataByIdentifier: request out of range - the ECU does not have that
// identifier" has done the job.
//
// Two conventions in the protocol are worth knowing before reading any of this:
//
//   * **A positive response is the service byte plus 0x40.** A request of 0x22
//     is answered by 0x62. That is the whole of the success case, and it is why
//     the code below can check an answer against a request without a table.
//
//   * **A negative response is always three bytes**: 0x7F, the service that
//     failed, and a reason. The reasons are shared across services, and the one
//     that matters most is 0x78 - "I am still working on it" - which is not a
//     failure at all: it restarts the clock and can arrive many times before
//     the real answer. A tester that treats 0x78 as an error gives up on every
//     ECU that takes more than fifty milliseconds to think.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// The services this build knows the names of. Others are carried perfectly
/// well - the client is not a whitelist - they are simply reported by number.
enum class UdsService : std::uint8_t {
    DiagnosticSessionControl = 0x10,
    EcuReset = 0x11,
    ClearDiagnosticInformation = 0x14,
    ReadDtcInformation = 0x19,
    ReadDataByIdentifier = 0x22,
    ReadMemoryByAddress = 0x23,
    SecurityAccess = 0x27,
    CommunicationControl = 0x28,
    WriteDataByIdentifier = 0x2E,
    InputOutputControlByIdentifier = 0x2F,
    RoutineControl = 0x31,
    RequestDownload = 0x34,
    RequestUpload = 0x35,
    TransferData = 0x36,
    RequestTransferExit = 0x37,
    WriteMemoryByAddress = 0x3D,
    TesterPresent = 0x3E,
    ControlDtcSetting = 0x85,
};

/// The negative response codes worth naming, which is most of the ones an
/// engineer meets.
enum class UdsNegativeResponse : std::uint8_t {
    None = 0x00,
    GeneralReject = 0x10,
    ServiceNotSupported = 0x11,
    SubFunctionNotSupported = 0x12,
    IncorrectMessageLength = 0x13,
    ResponseTooLong = 0x14,
    BusyRepeatRequest = 0x21,
    ConditionsNotCorrect = 0x22,
    RequestSequenceError = 0x24,
    RequestOutOfRange = 0x31,
    SecurityAccessDenied = 0x33,
    InvalidKey = 0x35,
    ExceedNumberOfAttempts = 0x36,
    RequiredTimeDelayNotExpired = 0x37,
    UploadDownloadNotAccepted = 0x70,
    TransferDataSuspended = 0x71,
    GeneralProgrammingFailure = 0x72,
    WrongBlockSequenceCounter = 0x73,

    /// **Not an error.** The ECU is still working; the answer is coming. It
    /// restarts the response clock and may arrive repeatedly.
    ResponsePending = 0x78,

    SubFunctionNotSupportedInActiveSession = 0x7E,
    ServiceNotSupportedInActiveSession = 0x7F,
    RpmTooHigh = 0x81,
    RpmTooLow = 0x82,
    EngineIsRunning = 0x83,
    EngineIsNotRunning = 0x84,
    VoltageTooHigh = 0x92,
    VoltageTooLow = 0x93,
};

/// The sessions of DiagnosticSessionControl.
enum class UdsSession : std::uint8_t {
    Default = 0x01,
    Programming = 0x02,
    Extended = 0x03,
    SafetySystem = 0x04,
};

/// What service 0x%02X is called, or "Service 0x%02X" when this build has no
/// name for it. Never empty: an unknown service is still worth showing.
[[nodiscard]] std::string describeService(std::uint8_t service);

/// What a negative response code means, in a sentence an engineer can act on.
[[nodiscard]] std::string describeNegativeResponse(std::uint8_t code);

/// True when this response byte is the positive answer to `service`.
///
/// The +0x40 rule, in one place, because getting it wrong turns every
/// successful exchange into a mismatch.
[[nodiscard]] constexpr bool isPositiveResponseTo(std::uint8_t response,
                                                  std::uint8_t service) noexcept
{
    return response == static_cast<std::uint8_t>(service + 0x40U);
}

/// True when this service's second byte is a sub-function rather than data.
///
/// This matters more than it looks. The suppress-positive-response flag is bit
/// 7 of a *sub-function* byte - and 0x22 0xF1 0x90, the commonest request in
/// the protocol, has bit 7 set in its second byte because 0xF1 is half of an
/// identifier. A client that tests that bit without asking this question first
/// decides that a read of the VIN wants no answer, sends it, and waits forever
/// for a response it has told itself is not coming.
[[nodiscard]] bool hasSubFunction(std::uint8_t service) noexcept;

/// True when the message asks the ECU not to answer: a sub-function service
/// with bit 7 set.
[[nodiscard]] bool suppressesResponse(const std::vector<std::uint8_t>& message) noexcept;

/// True when the message is a negative response: 0x7F, service, reason.
[[nodiscard]] bool isNegativeResponse(const std::vector<std::uint8_t>& message) noexcept;

// --- Request builders -----------------------------------------------------
//
// One per service that takes something more structured than a byte. They exist
// so that the identifier of ReadDataByIdentifier is written big-endian in one
// place rather than at every call site - which is the single most common way a
// hand-built UDS request is wrong, and it fails as "request out of range"
// rather than as anything that points at the byte order.

/// 0x22, big-endian identifier. 0xF190 is the VIN on almost every ECU.
[[nodiscard]] std::vector<std::uint8_t> readDataByIdentifier(std::uint16_t identifier);

/// 0x2E: the identifier, then the value.
[[nodiscard]] std::vector<std::uint8_t> writeDataByIdentifier(
    std::uint16_t identifier,
    const std::vector<std::uint8_t>& value);

/// 0x10, with the suppress-positive-response bit clear: a session change whose
/// answer nobody waits for is a session change nobody can be sure of.
[[nodiscard]] std::vector<std::uint8_t> diagnosticSessionControl(UdsSession session);

/// 0x3E. `suppressResponse` sets bit 7 of the sub-function, which is the
/// ordinary way tester present is sent: it is a heartbeat, and answering every
/// one of them is traffic for nothing.
[[nodiscard]] std::vector<std::uint8_t> testerPresent(bool suppressResponse = true);

/// 0x11: 0x01 hard reset, 0x02 key off/on, 0x03 soft reset.
[[nodiscard]] std::vector<std::uint8_t> ecuReset(std::uint8_t type = 0x01);

/// 0x19 0x02: every DTC whose status matches `statusMask`. 0xFF is "all of
/// them", which is what a first look wants.
[[nodiscard]] std::vector<std::uint8_t> readDtcByStatusMask(std::uint8_t statusMask = 0xFF);

/// 0x14 with a three-byte group. 0xFFFFFF clears everything the ECU stores.
[[nodiscard]] std::vector<std::uint8_t> clearDiagnosticInformation(
    std::uint32_t group = 0xFF'FFFFU);

/// 0x27: the odd sub-function requests a seed, the even one that follows sends
/// the key back.
[[nodiscard]] std::vector<std::uint8_t> securityAccessSeed(std::uint8_t level = 0x01);
[[nodiscard]] std::vector<std::uint8_t> securityAccessKey(std::uint8_t level,
                                                          const std::vector<std::uint8_t>& key);

/// One trouble code as 0x19 0x02 reports it: three bytes of code, one of
/// status.
struct DiagnosticTroubleCode final {
    /// The 24-bit code, as it appears on the wire.
    ///
    /// The first two bytes are the code a workshop manual indexes; the third is
    /// the **failure type** - 0x35 is "signal above range", and so on. It is
    /// carried here and deliberately not folded into name(), because P0128 and
    /// P0128-35 are the same fault described at two levels of detail, and the
    /// short form is the one people say out loud.
    std::uint32_t code{};

    /// Bit 0 is "test failed", bit 3 "confirmed", bit 8 would be... see the
    /// standard. Carried whole rather than decoded here, because which bits an
    /// ECU actually maintains varies more than the standard suggests.
    std::uint8_t status{};

    /// "P0128", "C0035", "B1234", "U0100" - the form every scan tool shows and
    /// every workshop manual indexes.
    [[nodiscard]] std::string name() const;
};

/// The trouble codes in a 0x59 0x02 response, or an empty list when it is not
/// one.
[[nodiscard]] std::vector<DiagnosticTroubleCode> parseDtcResponse(
    const std::vector<std::uint8_t>& response);

} // namespace torquebus
