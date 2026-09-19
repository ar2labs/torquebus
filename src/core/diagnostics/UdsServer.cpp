// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/UdsServer.h"

#include <algorithm>
#include <utility>

namespace torquebus {
namespace {

constexpr std::uint8_t kNegativeResponse = 0x7FU;
constexpr std::uint8_t kSuppressPositiveResponse = 0x80U;

/// The identifier in the two bytes after the service.
[[nodiscard]] std::uint16_t identifierOf(std::span<const std::uint8_t> request) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(request[1]) << 8U) | request[2]);
}

} // namespace

void UdsServer::setIdentifier(std::uint16_t identifier, UdsIdentifier value)
{
    m_identifiers[identifier] = std::move(value);
}

void UdsServer::addTroubleCode(std::uint32_t code, std::uint8_t status)
{
    for (DiagnosticTroubleCode& existing : m_troubleCodes) {
        if (existing.code == code) {
            // Setting a fault that is already set updates its status rather
            // than storing it twice - which is what an ECU does, and what keeps
            // a script that calls this from a timer from filling memory.
            existing.status = status;
            return;
        }
    }

    m_troubleCodes.push_back(DiagnosticTroubleCode{code, status});
}

void UdsServer::clearTroubleCodes()
{
    m_troubleCodes.clear();
}

std::vector<std::uint8_t> UdsServer::refuse(std::uint8_t service, UdsNegativeResponse reason) const
{
    return {kNegativeResponse, service, static_cast<std::uint8_t>(reason)};
}

bool UdsServer::sessionAllows(UdsSession wanted) const noexcept
{
    // Default means "any session will do". Anything else has to match exactly:
    // an ECU that accepted a programming-session DID in the extended session
    // would be teaching a tester something that fails on the bench.
    return wanted == UdsSession::Default || wanted == m_session;
}

std::optional<std::vector<std::uint8_t>>
UdsServer::handleSessionControl(std::span<const std::uint8_t> request, std::uint64_t nowNs)
{
    if (request.size() < 2) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const auto wanted = static_cast<UdsSession>(request[1] & 0x7FU);

    switch (wanted) {
    case UdsSession::Default:
    case UdsSession::Programming:
    case UdsSession::Extended:
    case UdsSession::SafetySystem:
        break;
    default:
        return refuse(request[0], UdsNegativeResponse::SubFunctionNotSupported);
    }

    m_session = wanted;

    // Leaving a session locks the ECU again. Security is granted inside a
    // session and does not survive it, which is the rule a tester most often
    // gets wrong - and the one that makes an unlock-then-reset sequence fail.
    if (wanted == UdsSession::Default) {
        m_unlocked = false;
        m_sessionExpiresNs = 0;
    } else {
        m_sessionExpiresNs = nowNs + static_cast<std::uint64_t>(m_sessionTimeoutMs) * 1'000'000ULL;
    }

    m_seedGivenFor = 0;

    if ((request[1] & kSuppressPositiveResponse) != 0) {
        return std::nullopt;
    }

    // The P2 and P2* the ECU promises, in the answer, as the standard defines
    // it: 50 ms and 5000 ms in the units the message uses (P2* is in tens).
    return std::vector<std::uint8_t>{static_cast<std::uint8_t>(request[0] + 0x40U),
                                     static_cast<std::uint8_t>(wanted),
                                     0x00,
                                     0x32,
                                     0x01,
                                     0xF4};
}

std::optional<std::vector<std::uint8_t>>
UdsServer::handleReadDid(std::span<const std::uint8_t> request)
{
    if (request.size() < 3) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const std::uint16_t identifier = identifierOf(request);
    const auto found = m_identifiers.find(identifier);

    if (found == m_identifiers.end()) {
        // The commonest answer a real ECU gives a tester that guessed, and the
        // reason the console spells this code out in words.
        return refuse(request[0], UdsNegativeResponse::RequestOutOfRange);
    }

    if (!sessionAllows(found->second.requiredSession)) {
        return refuse(request[0], UdsNegativeResponse::ServiceNotSupportedInActiveSession);
    }

    if (found->second.requiresSecurity && !m_unlocked) {
        return refuse(request[0], UdsNegativeResponse::SecurityAccessDenied);
    }

    std::vector<std::uint8_t> response{
        static_cast<std::uint8_t>(request[0] + 0x40U), request[1], request[2]};

    response.insert(response.end(), found->second.value.begin(), found->second.value.end());
    return response;
}

std::optional<std::vector<std::uint8_t>>
UdsServer::handleWriteDid(std::span<const std::uint8_t> request)
{
    if (request.size() < 4) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const std::uint16_t identifier = identifierOf(request);
    const auto found = m_identifiers.find(identifier);

    if (found == m_identifiers.end()) {
        return refuse(request[0], UdsNegativeResponse::RequestOutOfRange);
    }

    if (!found->second.writable) {
        // A read-only DID refuses the write rather than accepting it and
        // changing nothing, which is what a part number does on a real ECU.
        return refuse(request[0], UdsNegativeResponse::RequestOutOfRange);
    }

    if (!sessionAllows(found->second.requiredSession)) {
        return refuse(request[0], UdsNegativeResponse::ServiceNotSupportedInActiveSession);
    }

    if (found->second.requiresSecurity && !m_unlocked) {
        return refuse(request[0], UdsNegativeResponse::SecurityAccessDenied);
    }

    found->second.value.assign(request.begin() + 3, request.end());

    return std::vector<std::uint8_t>{
        static_cast<std::uint8_t>(request[0] + 0x40U), request[1], request[2]};
}

std::optional<std::vector<std::uint8_t>>
UdsServer::handleReadDtc(std::span<const std::uint8_t> request)
{
    if (request.size() < 2) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const std::uint8_t report = request[1];

    // 0x01 counts them, 0x02 lists them. Anything else is a sub-function this
    // ECU does not implement - said rather than guessed at, because a tester
    // reading a made-up layout invents faults that are not there.
    if (report != 0x01 && report != 0x02) {
        return refuse(request[0], UdsNegativeResponse::SubFunctionNotSupported);
    }

    if (request.size() < 3) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const std::uint8_t mask = request[2];

    std::vector<const DiagnosticTroubleCode*> matching;

    for (const DiagnosticTroubleCode& dtc : m_troubleCodes) {
        if ((dtc.status & mask) != 0) {
            matching.push_back(&dtc);
        }
    }

    std::vector<std::uint8_t> response{static_cast<std::uint8_t>(request[0] + 0x40U), report};

    // The availability mask: which status bits this ECU maintains at all. 0xFF
    // is "all of them", which is the honest answer for a simulation.
    response.push_back(0xFF);

    if (report == 0x01) {
        // Format identifier, then a 16-bit count.
        response.push_back(0x01);
        response.push_back(static_cast<std::uint8_t>((matching.size() >> 8U) & 0xFFU));
        response.push_back(static_cast<std::uint8_t>(matching.size() & 0xFFU));
        return response;
    }

    for (const DiagnosticTroubleCode* dtc : matching) {
        response.push_back(static_cast<std::uint8_t>((dtc->code >> 16U) & 0xFFU));
        response.push_back(static_cast<std::uint8_t>((dtc->code >> 8U) & 0xFFU));
        response.push_back(static_cast<std::uint8_t>(dtc->code & 0xFFU));
        response.push_back(dtc->status);
    }

    return response;
}

std::optional<std::vector<std::uint8_t>>
UdsServer::handleSecurityAccess(std::span<const std::uint8_t> request)
{
    if (request.size() < 2) {
        return refuse(request[0], UdsNegativeResponse::IncorrectMessageLength);
    }

    const std::uint8_t level = request[1] & 0x7FU;

    if (level == 0) {
        return refuse(request[0], UdsNegativeResponse::SubFunctionNotSupported);
    }

    // Odd levels ask for a seed, even ones send the key back.
    if ((level % 2U) == 1U) {
        if (!m_algorithm) {
            // An ECU with no algorithm is locked, not open. Answering a seed
            // that no key can ever match would leave a tester trying.
            return refuse(request[0], UdsNegativeResponse::ConditionsNotCorrect);
        }

        m_seedGivenFor = level;

        std::vector<std::uint8_t> response{static_cast<std::uint8_t>(request[0] + 0x40U),
                                           request[1]};

        if (m_unlocked) {
            // Already unlocked: a seed of zeroes, which is how an ECU says "you
            // do not need to do this again" without refusing.
            response.insert(response.end(), m_seed.size(), 0x00);
            return response;
        }

        response.insert(response.end(), m_seed.begin(), m_seed.end());
        return response;
    }

    if (m_seedGivenFor + 1U != level) {
        // A key with no seed behind it. A real ECU calls this a sequence error
        // and so does this one - it is a mistake a tester makes, and being told
        // is the point.
        return refuse(request[0], UdsNegativeResponse::RequestSequenceError);
    }

    if (!m_algorithm) {
        return refuse(request[0], UdsNegativeResponse::ConditionsNotCorrect);
    }

    const std::vector<std::uint8_t> expected = m_algorithm(m_seed);
    const std::span<const std::uint8_t> given = request.subspan(2);

    if (expected.size() != given.size()
        || !std::equal(expected.begin(), expected.end(), given.begin())) {
        m_seedGivenFor = 0;
        return refuse(request[0], UdsNegativeResponse::InvalidKey);
    }

    m_unlocked = true;
    m_seedGivenFor = 0;

    return std::vector<std::uint8_t>{static_cast<std::uint8_t>(request[0] + 0x40U), request[1]};
}

std::optional<std::vector<std::uint8_t>> UdsServer::handle(std::span<const std::uint8_t> request,
                                                           std::uint64_t nowNs)
{
    if (request.empty()) {
        return std::nullopt;
    }

    // Any request at all keeps the session alive, not only TesterPresent. That
    // is what the standard says and what a real ECU does: S3 is a silence
    // timer, not a heartbeat counter.
    if (m_session != UdsSession::Default) {
        m_sessionExpiresNs = nowNs + static_cast<std::uint64_t>(m_sessionTimeoutMs) * 1'000'000ULL;
    }

    // The script first, always. Anything this class does not implement, and
    // anything a particular ECU does differently, belongs there rather than
    // here.
    if (m_handler) {
        std::vector<std::uint8_t> response;

        switch (m_handler(request, response)) {
        case Verdict::Answered:
            return response;
        case Verdict::Silent:
            return std::nullopt;
        case Verdict::NotHandled:
            break;
        }
    }

    const std::uint8_t service = request[0];

    switch (static_cast<UdsService>(service)) {
    case UdsService::DiagnosticSessionControl:
        return handleSessionControl(request, nowNs);

    case UdsService::ReadDataByIdentifier:
        return handleReadDid(request);

    case UdsService::WriteDataByIdentifier:
        return handleWriteDid(request);

    case UdsService::ReadDtcInformation:
        return handleReadDtc(request);

    case UdsService::ClearDiagnosticInformation:
        clearTroubleCodes();
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(service + 0x40U)};

    case UdsService::SecurityAccess:
        return handleSecurityAccess(request);

    case UdsService::TesterPresent:
        if (request.size() >= 2 && (request[1] & kSuppressPositiveResponse) != 0) {
            // The ordinary form: heard, and not answered. The session was
            // already extended above, which is the whole point of the message.
            return std::nullopt;
        }
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(service + 0x40U), 0x00};

    case UdsService::EcuReset:
        if (request.size() < 2) {
            return refuse(service, UdsNegativeResponse::IncorrectMessageLength);
        }

        // Answered first, then reset - which is the order a real ECU does it
        // in, and the reason a tester sees the answer and then loses the
        // session.
        {
            std::vector<std::uint8_t> response{static_cast<std::uint8_t>(service + 0x40U),
                                               request[1]};
            reset();
            return response;
        }

    default:
        break;
    }

    // A service this ECU does not implement. Said, rather than met with
    // silence: a tester cannot tell silence from a broken wire.
    return refuse(service, UdsNegativeResponse::ServiceNotSupported);
}

void UdsServer::poll(std::uint64_t nowNs)
{
    if (m_sessionExpiresNs == 0 || nowNs < m_sessionExpiresNs) {
        return;
    }

    // S3 expired. Back to the default session with security locked - which is
    // what makes a tester that forgot its heartbeat fail here rather than in a
    // vehicle.
    m_session = UdsSession::Default;
    m_unlocked = false;
    m_seedGivenFor = 0;
    m_sessionExpiresNs = 0;
}

void UdsServer::reset()
{
    m_session = UdsSession::Default;
    m_unlocked = false;
    m_seedGivenFor = 0;
    m_sessionExpiresNs = 0;

    // Faults are not cleared: stored means stored, and a reset that lost them
    // would make the simulation useless for exactly the workflow it is for.
}

} // namespace torquebus
