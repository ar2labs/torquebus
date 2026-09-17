// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/UdsClient.h"

#include "core/diagnostics/DiagnosticEvent.h"

#include <format>

namespace torquebus {
namespace {

[[nodiscard]] std::uint64_t millisecondsToNs(std::uint32_t milliseconds) noexcept
{
    return static_cast<std::uint64_t>(milliseconds) * 1'000'000ULL;
}

} // namespace

std::string UdsExchange::describe() const
{
    const std::uint8_t service = request.empty() ? 0U : request.front();
    const double milliseconds = static_cast<double>(elapsedNs) / 1'000'000.0;

    switch (outcome) {
    case Outcome::Positive:
        return std::format("{}: {} ({:.1f} ms{})", describeService(service),
                           toHexBytes(response), milliseconds,
                           pendingCount > 0 ? std::format(", {} x pending", pendingCount)
                                            : std::string{});

    case Outcome::Negative:
        return std::format("{} refused: {} ({:.1f} ms)", describeService(service),
                           describeNegativeResponse(negativeResponse), milliseconds);

    case Outcome::Timeout:
        return std::format("{}: no answer in {:.0f} ms", describeService(service),
                           milliseconds);

    case Outcome::Mismatch:
        return std::format("{}: the ECU answered something else - {}",
                           describeService(service), toHexBytes(response));
    }

    return {};
}

UdsClient::UdsClient(UdsTiming timing)
    : m_timing{timing}
{
}

Result UdsClient::request(std::vector<std::uint8_t> message, std::uint64_t nowNs)
{
    if (m_busy) {
        // One question at a time on one address. A queue here would hide an ECU
        // that answered none of them, which is the thing worth knowing.
        return Result::error(ErrorCode::InvalidState,
                             "This client is still waiting for an answer");
    }

    if (message.empty()) {
        return Result::error(ErrorCode::InvalidArgument, "A UDS request needs a service byte");
    }

    m_request = std::move(message);
    m_expectedResponse = static_cast<std::uint8_t>(m_request.front() + 0x40U);

    m_startedNs = nowNs;
    m_pendingCount = 0;

    // A request that suppresses its own answer has nothing to wait for. Sending
    // one and then waiting fifty milliseconds for silence would make a
    // heartbeat cost more than the traffic it replaces.
    //
    // Asked through suppressesResponse(), which checks that the service even
    // *has* a sub-function first - 0x22 0xF1 0x90 has bit 7 set in its second
    // byte because 0xF1 is half an identifier, not because anybody asked for
    // silence.
    const bool suppressed = suppressesResponse(m_request);

    m_outgoing.push_back(m_request);

    if (suppressed) {
        m_busy = false;
        return Result::ok();
    }

    m_busy = true;
    m_deadlineNs = nowNs + millisecondsToNs(m_timing.p2Ms);

    return Result::ok();
}

void UdsClient::finish(UdsExchange::Outcome outcome,
                       const std::vector<std::uint8_t>& response,
                       std::uint8_t negativeResponse,
                       std::uint64_t nowNs)
{
    UdsExchange exchange;
    exchange.outcome = outcome;
    exchange.request = m_request;
    exchange.response = response;
    exchange.negativeResponse = negativeResponse;
    exchange.elapsedNs = nowNs > m_startedNs ? nowNs - m_startedNs : 0;
    exchange.pendingCount = m_pendingCount;

    m_busy = false;
    m_pendingCount = 0;

    m_exchanges.push_back(std::move(exchange));
}

void UdsClient::noteSessionChange(const std::vector<std::uint8_t>& response, std::uint64_t nowNs)
{
    if (response.size() < 2
        || response[0] != static_cast<std::uint8_t>(
               static_cast<std::uint8_t>(UdsService::DiagnosticSessionControl) + 0x40U)) {
        return;
    }

    // The session the ECU says it is in, not the one that was asked for: an ECU
    // may answer a request for the programming session with the extended one,
    // and believing the request rather than the answer is how a tester ends up
    // certain of a session it is not in.
    m_session = static_cast<UdsSession>(response[1] & 0x7FU);

    if (m_session == UdsSession::Default || !m_timing.keepSessionAlive) {
        m_nextKeepAliveNs = 0;
        return;
    }

    // At two fifths of S3. Half would be cutting it fine on a busy bus, where a
    // heartbeat can wait behind a long transfer; a fifth doubles the traffic
    // for nothing.
    m_nextKeepAliveNs = nowNs + millisecondsToNs(m_timing.s3Ms * 2U / 5U);
}

bool UdsClient::onMessage(const std::vector<std::uint8_t>& message, std::uint64_t nowNs)
{
    if (!m_busy || message.empty()) {
        // Not waiting for anything. Somebody else's answer on a shared
        // identifier, or one to a request that already timed out - left for
        // whoever can make sense of it rather than reported as ours.
        return false;
    }

    if (isNegativeResponse(message)) {
        if (message[1] != m_request.front()) {
            return false; // A refusal of somebody else's request.
        }

        if (message[2] == static_cast<std::uint8_t>(UdsNegativeResponse::ResponsePending)) {
            // Not a failure: the ECU heard, and needs longer. The clock
            // restarts as P2*, and this can happen many times - a routine that
            // erases a flash sector sends a dozen.
            ++m_pendingCount;
            m_deadlineNs = nowNs + millisecondsToNs(m_timing.p2StarMs);
            return true;
        }

        finish(UdsExchange::Outcome::Negative, message, message[2], nowNs);
        return true;
    }

    if (message[0] != m_expectedResponse) {
        // An answer to something else, on our identifier. Reported rather than
        // ignored: on a bus where this happens, knowing it happened is the
        // whole diagnosis.
        finish(UdsExchange::Outcome::Mismatch, message, 0, nowNs);
        return true;
    }

    noteSessionChange(message, nowNs);
    finish(UdsExchange::Outcome::Positive, message, 0, nowNs);

    return true;
}

void UdsClient::poll(std::uint64_t nowNs)
{
    if (m_busy && nowNs >= m_deadlineNs) {
        finish(UdsExchange::Outcome::Timeout, {}, 0, nowNs);
    }

    if (m_nextKeepAliveNs != 0 && nowNs >= m_nextKeepAliveNs && !m_busy) {
        // Sent straight rather than through request(), because a heartbeat must
        // not take the client's one outstanding slot: a TesterPresent that
        // blocked a real request would be a keep-alive that stops the work it
        // exists to protect.
        m_outgoing.push_back(testerPresent(true));
        m_nextKeepAliveNs = nowNs + millisecondsToNs(m_timing.s3Ms * 2U / 5U);
    }
}

void UdsClient::reset()
{
    m_busy = false;
    m_request.clear();
    m_pendingCount = 0;
    m_session = UdsSession::Default;
    m_nextKeepAliveNs = 0;

    m_outgoing.clear();
    m_exchanges.clear();
}

} // namespace torquebus
