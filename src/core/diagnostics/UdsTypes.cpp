// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/UdsTypes.h"

#include <format>

namespace torquebus {
namespace {

/// The first byte of a negative response.
constexpr std::uint8_t kNegativeResponse = 0x7FU;

/// Bit 7 of a sub-function: "do not answer this one".
constexpr std::uint8_t kSuppressPositiveResponse = 0x80U;

} // namespace

std::string describeService(std::uint8_t service)
{
    switch (static_cast<UdsService>(service)) {
    case UdsService::DiagnosticSessionControl: return "DiagnosticSessionControl";
    case UdsService::EcuReset: return "ECUReset";
    case UdsService::ClearDiagnosticInformation: return "ClearDiagnosticInformation";
    case UdsService::ReadDtcInformation: return "ReadDTCInformation";
    case UdsService::ReadDataByIdentifier: return "ReadDataByIdentifier";
    case UdsService::ReadMemoryByAddress: return "ReadMemoryByAddress";
    case UdsService::SecurityAccess: return "SecurityAccess";
    case UdsService::CommunicationControl: return "CommunicationControl";
    case UdsService::WriteDataByIdentifier: return "WriteDataByIdentifier";
    case UdsService::InputOutputControlByIdentifier: return "InputOutputControlByIdentifier";
    case UdsService::RoutineControl: return "RoutineControl";
    case UdsService::RequestDownload: return "RequestDownload";
    case UdsService::RequestUpload: return "RequestUpload";
    case UdsService::TransferData: return "TransferData";
    case UdsService::RequestTransferExit: return "RequestTransferExit";
    case UdsService::WriteMemoryByAddress: return "WriteMemoryByAddress";
    case UdsService::TesterPresent: return "TesterPresent";
    case UdsService::ControlDtcSetting: return "ControlDTCSetting";
    }

    // Carried, not refused. A manufacturer-specific service is still a service,
    // and a tool that hides what it cannot name is worse than one that shows a
    // number.
    return std::format("Service 0x{:02X}", service);
}

std::string describeNegativeResponse(std::uint8_t code)
{
    switch (static_cast<UdsNegativeResponse>(code)) {
    case UdsNegativeResponse::None:
        return "no error";
    case UdsNegativeResponse::GeneralReject:
        return "general reject - the ECU refused without saying why";
    case UdsNegativeResponse::ServiceNotSupported:
        return "the ECU does not implement this service";
    case UdsNegativeResponse::SubFunctionNotSupported:
        return "the ECU does not implement this sub-function";
    case UdsNegativeResponse::IncorrectMessageLength:
        return "the request is the wrong length for this service";
    case UdsNegativeResponse::ResponseTooLong:
        return "the answer would not fit in the transport";
    case UdsNegativeResponse::BusyRepeatRequest:
        return "the ECU is busy - ask again";
    case UdsNegativeResponse::ConditionsNotCorrect:
        return "conditions not correct - often the wrong session, or the engine "
               "running when it should not be";
    case UdsNegativeResponse::RequestSequenceError:
        return "out of sequence - something has to come before this";
    case UdsNegativeResponse::RequestOutOfRange:
        return "request out of range - usually an identifier this ECU does not have";
    case UdsNegativeResponse::SecurityAccessDenied:
        return "security access denied - unlock with 0x27 first";
    case UdsNegativeResponse::InvalidKey:
        return "the key did not match the seed";
    case UdsNegativeResponse::ExceedNumberOfAttempts:
        return "too many wrong keys - the ECU has locked out further attempts";
    case UdsNegativeResponse::RequiredTimeDelayNotExpired:
        return "still in the lockout delay after a failed unlock";
    case UdsNegativeResponse::UploadDownloadNotAccepted:
        return "the ECU will not start a transfer now";
    case UdsNegativeResponse::TransferDataSuspended:
        return "the transfer was suspended";
    case UdsNegativeResponse::GeneralProgrammingFailure:
        return "programming failed";
    case UdsNegativeResponse::WrongBlockSequenceCounter:
        return "the block sequence counter is wrong - a transfer block was lost "
               "or repeated";
    case UdsNegativeResponse::ResponsePending:
        return "the ECU is still working on it";
    case UdsNegativeResponse::SubFunctionNotSupportedInActiveSession:
        return "this sub-function needs a different session";
    case UdsNegativeResponse::ServiceNotSupportedInActiveSession:
        return "this service needs a different session - try 0x10 0x03 first";
    case UdsNegativeResponse::RpmTooHigh:
        return "engine speed too high";
    case UdsNegativeResponse::RpmTooLow:
        return "engine speed too low";
    case UdsNegativeResponse::EngineIsRunning:
        return "the engine must be stopped for this";
    case UdsNegativeResponse::EngineIsNotRunning:
        return "the engine must be running for this";
    case UdsNegativeResponse::VoltageTooHigh:
        return "supply voltage too high";
    case UdsNegativeResponse::VoltageTooLow:
        return "supply voltage too low - the commonest reason a programming "
               "session is refused";
    }

    return std::format("negative response 0x{:02X}", code);
}

bool hasSubFunction(std::uint8_t service) noexcept
{
    // The services whose second byte is a sub-function. Everything else puts
    // data there - an identifier, an address, a block counter - and testing bit
    // 7 of *that* is how a request to read the VIN gets sent as one that wants
    // no answer.
    switch (static_cast<UdsService>(service)) {
    case UdsService::DiagnosticSessionControl:
    case UdsService::EcuReset:
    case UdsService::ReadDtcInformation:
    case UdsService::SecurityAccess:
    case UdsService::CommunicationControl:
    case UdsService::RoutineControl:
    case UdsService::TesterPresent:
    case UdsService::ControlDtcSetting:
        return true;

    default:
        return false;
    }
}

bool suppressesResponse(const std::vector<std::uint8_t>& message) noexcept
{
    return message.size() >= 2 && hasSubFunction(message[0])
        && (message[1] & kSuppressPositiveResponse) != 0;
}

bool isNegativeResponse(const std::vector<std::uint8_t>& message) noexcept
{
    return message.size() >= 3 && message[0] == kNegativeResponse;
}

std::vector<std::uint8_t> readDataByIdentifier(std::uint16_t identifier)
{
    // Big-endian, which is the single most common way a hand-built UDS request
    // is wrong - and it fails as "request out of range", which points nowhere
    // near the byte order.
    return {static_cast<std::uint8_t>(UdsService::ReadDataByIdentifier),
            static_cast<std::uint8_t>((identifier >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(identifier & 0xFFU)};
}

std::vector<std::uint8_t> writeDataByIdentifier(std::uint16_t identifier,
                                                const std::vector<std::uint8_t>& value)
{
    std::vector<std::uint8_t> request{
        static_cast<std::uint8_t>(UdsService::WriteDataByIdentifier),
        static_cast<std::uint8_t>((identifier >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(identifier & 0xFFU)};

    request.insert(request.end(), value.begin(), value.end());
    return request;
}

std::vector<std::uint8_t> diagnosticSessionControl(UdsSession session)
{
    return {static_cast<std::uint8_t>(UdsService::DiagnosticSessionControl),
            static_cast<std::uint8_t>(session)};
}

std::vector<std::uint8_t> testerPresent(bool suppressResponse)
{
    // 0x3E 0x80: a heartbeat that is not answered. Sending 0x00 instead doubles
    // the traffic and gives a tester an answer it has nothing to do with.
    return {static_cast<std::uint8_t>(UdsService::TesterPresent),
            static_cast<std::uint8_t>(suppressResponse ? kSuppressPositiveResponse : 0x00U)};
}

std::vector<std::uint8_t> ecuReset(std::uint8_t type)
{
    return {static_cast<std::uint8_t>(UdsService::EcuReset), type};
}

std::vector<std::uint8_t> readDtcByStatusMask(std::uint8_t statusMask)
{
    // 0x02 is reportDTCByStatusMask, which is the one a first look wants.
    return {static_cast<std::uint8_t>(UdsService::ReadDtcInformation), 0x02U, statusMask};
}

std::vector<std::uint8_t> clearDiagnosticInformation(std::uint32_t group)
{
    return {static_cast<std::uint8_t>(UdsService::ClearDiagnosticInformation),
            static_cast<std::uint8_t>((group >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((group >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(group & 0xFFU)};
}

std::vector<std::uint8_t> securityAccessSeed(std::uint8_t level)
{
    return {static_cast<std::uint8_t>(UdsService::SecurityAccess), level};
}

std::vector<std::uint8_t> securityAccessKey(std::uint8_t level,
                                            const std::vector<std::uint8_t>& key)
{
    // The key goes back on the level *after* the one that asked for the seed:
    // seed on 0x01, key on 0x02. Adding one here rather than at the call site,
    // because a key sent on the seed's own level is refused as a sequence error
    // that says nothing about the cause.
    std::vector<std::uint8_t> request{static_cast<std::uint8_t>(UdsService::SecurityAccess),
                                      static_cast<std::uint8_t>(level + 1U)};

    request.insert(request.end(), key.begin(), key.end());
    return request;
}

std::string DiagnosticTroubleCode::name() const
{
    // The first two bits pick the system letter and the next two are the first
    // digit; the rest is hexadecimal. This is the form every scan tool shows
    // and every workshop manual indexes, and it is nothing like the raw number.
    static constexpr char kSystems[] = {'P', 'C', 'B', 'U'};

    const auto high = static_cast<std::uint8_t>((code >> 16U) & 0xFFU);
    const char system = kSystems[(high >> 6U) & 0x03U];
    const auto firstDigit = static_cast<unsigned>((high >> 4U) & 0x03U);

    return std::format("{}{}{:01X}{:02X}", system, firstDigit,
                       static_cast<unsigned>(high & 0x0FU),
                       static_cast<unsigned>((code >> 8U) & 0xFFU));
}

std::vector<DiagnosticTroubleCode> parseDtcResponse(const std::vector<std::uint8_t>& response)
{
    std::vector<DiagnosticTroubleCode> codes;

    // 0x59 0x02 <availability mask> then four bytes per code. Anything else is
    // a different sub-function's answer, and guessing at its layout would
    // invent trouble codes that are not there - which is the worst possible
    // thing for this particular screen to do.
    if (response.size() < 3 || response[0] != 0x59U || response[1] != 0x02U) {
        return codes;
    }

    for (std::size_t index = 3; index + 3 < response.size(); index += 4) {
        DiagnosticTroubleCode dtc;
        dtc.code = (static_cast<std::uint32_t>(response[index]) << 16U)
            | (static_cast<std::uint32_t>(response[index + 1]) << 8U) | response[index + 2];
        dtc.status = response[index + 3];

        codes.push_back(dtc);
    }

    return codes;
}

} // namespace torquebus
