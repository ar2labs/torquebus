// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/UdsClientNode.h"

#include <algorithm>
#include <utility>

namespace torquebus {
namespace {

/// How many requests will be held while the client is busy.
///
/// A console user pressing Send four times at an ECU that is not answering has
/// made one mistake, not four; holding the whole lot and playing them out over
/// the next twenty seconds would turn that into a mess nobody asked for.
constexpr std::size_t kMaximumQueued = 8;

} // namespace

UdsClientNode::UdsClientNode(IsoTpAddress address,
                             IsoTpConfig transport,
                             UdsTiming timing,
                             DiagnosticSession* session,
                             std::string label)
    : m_transport{address, transport}
    , m_client{timing}
    , m_session{session}
    , m_label{std::move(label)}
{ }

Result UdsClientNode::prepare(std::size_t maximumBatchSize)
{
    m_transport.reset();
    m_client.reset();

    m_started = std::chrono::steady_clock::now();
    m_queued.clear();

    m_sent = 0;
    m_answered = 0;
    m_refused = 0;
    m_timedOut = 0;

    m_outgoingFrames.clear();
    m_outgoingFrames.reserve(std::max<std::size_t>(maximumBatchSize, 8));

    m_outgoingEvents.clear();
    m_outgoingEvents.reserve(8);

    if (m_session != nullptr) {
        m_session->resetForRun();
        m_session->setActive(true);
    }

    return Result::ok();
}

void UdsClientNode::publish(const UdsExchange& exchange, std::uint64_t nowNs)
{
    switch (exchange.outcome) {
    case UdsExchange::Outcome::Positive:
        ++m_answered;
        break;
    case UdsExchange::Outcome::Negative:
        ++m_refused;
        break;
    case UdsExchange::Outcome::Timeout:
        ++m_timedOut;
        break;
    case UdsExchange::Outcome::Mismatch:
        break;
    }

    if (m_session != nullptr) {
        m_session->publishExchange(exchange);
    }

    // Downstream gets the response itself rather than the exchange: a block
    // that reacts to an answer wants the bytes, and the timing and the refusal
    // reason are the console's business.
    DiagnosticEvent event;
    event.channel = m_transport.address().channel;
    event.identifier = m_transport.address().receiveId;
    event.timestampNs = nowNs;

    if (exchange.outcome == UdsExchange::Outcome::Positive) {
        event.kind = DiagnosticEvent::Kind::MessageReceived;
        event.data = exchange.response;
    } else {
        event.kind = DiagnosticEvent::Kind::TransferFailed;
    }

    m_outgoingEvents.push_back(std::move(event));
}

void UdsClientNode::process(NodeContext& context)
{
    m_outgoingFrames.clear();
    m_outgoingEvents.clear();

    const auto nowNs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - m_started)
                                       .count());

    // --- Frames from the bus, up through the transport ---------------------
    for (const CanFrame& frame : context.in<CanFrame>(0)) {
        static_cast<void>(m_transport.onFrame(frame, nowNs));
    }

    m_transport.poll(nowNs);

    for (const IsoTpEvent& event : m_transport.events()) {
        if (event.kind == IsoTpEvent::Kind::MessageReceived) {
            static_cast<void>(m_client.onMessage(event.data, nowNs));
        }
        // A transport failure is left to the ISO-TP layer's own counters. The
        // UDS client will time out on its own, which is the report that means
        // something to somebody reading a console: "no answer" rather than
        // "N_Cr expired".
    }

    m_transport.clearEvents();

    // --- Requests, from wherever they come ---------------------------------
    if (m_session != nullptr) {
        static_cast<void>(m_session->takeRequests(m_queued));
    }

    for (const DiagnosticEvent& request : context.in<DiagnosticEvent>(1)) {
        if (!request.data.empty()) {
            m_queued.push_back(request.data);
        }
    }

    if (m_queued.size() > kMaximumQueued) {
        // The oldest go, not the newest: somebody who pressed Send five times
        // wants the last thing they asked, not the first.
        m_queued.erase(m_queued.begin(),
                       m_queued.end() - static_cast<std::ptrdiff_t>(kMaximumQueued));
    }

    if (!m_queued.empty() && !m_client.isBusy()) {
        if (m_client.request(m_queued.front(), nowNs).succeeded()) {
            ++m_sent;
            m_queued.erase(m_queued.begin());
        }
    }

    m_client.poll(nowNs);

    // --- Down through the transport, onto the bus --------------------------
    for (const std::vector<std::uint8_t>& message : m_client.pendingRequests()) {
        // Refused only when the transport is mid-transfer, which for a
        // request-response protocol means the previous answer is still
        // arriving. Putting it back at the front of the queue keeps the order
        // somebody typed them in.
        if (m_transport.send(message, nowNs).failed()) {
            m_queued.insert(m_queued.begin(), message);
        }
    }

    m_client.clearPendingRequests();

    for (const UdsExchange& exchange : m_client.exchanges()) {
        publish(exchange, nowNs);
    }

    m_client.clearExchanges();

    // Polled again after sending: the transport's first frame is queued by
    // send() and there is no reason to make it wait a pass.
    m_transport.poll(nowNs);

    for (const CanFrame& frame : m_transport.pendingFrames()) {
        m_outgoingFrames.push_back(frame);
    }

    m_transport.clearPendingFrames();

    if (m_session != nullptr) {
        m_session->setBusy(m_client.isBusy());
        m_session->setSessionType(static_cast<std::uint8_t>(m_client.session()));
    }

    if (!m_outgoingFrames.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoingFrames});
    }

    if (!m_outgoingEvents.empty()) {
        context.publish<DiagnosticEvent>(1, std::span<const DiagnosticEvent>{m_outgoingEvents});
    }
}

} // namespace torquebus
