// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/isotp/IsoTpNode.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace torquebus {

IsoTpNode::IsoTpNode(IsoTpAddress address,
                     IsoTpConfig config,
                     std::vector<std::uint8_t> periodicRequest,
                     std::uint32_t requestIntervalMs,
                     std::string label)
    : m_connection{address, config}
    , m_label{std::move(label)}
    , m_periodicRequest{std::move(periodicRequest)}
    , m_intervalNs{static_cast<std::uint64_t>(requestIntervalMs) * 1'000'000ULL}
{ }

Result IsoTpNode::prepare(std::size_t maximumBatchSize)
{
    m_connection.reset();

    m_started = std::chrono::steady_clock::now();
    m_nextRequestNs = 0;
    m_sentFirstRequest = false;

    m_received = 0;
    m_sent = 0;
    m_failed = 0;
    m_refused = 0;

    // Sized for the worst case, like every other source: a pass that had to
    // grow either of these would reallocate the storage a published span points
    // into.
    m_outgoingFrames.clear();
    m_outgoingFrames.reserve(std::max<std::size_t>(maximumBatchSize, 8));

    m_outgoingEvents.clear();
    m_outgoingEvents.reserve(16);

    return Result::ok();
}

void IsoTpNode::drain(std::uint64_t nowNs)
{
    for (const CanFrame& frame : m_connection.pendingFrames()) {
        m_outgoingFrames.push_back(frame);
    }
    m_connection.clearPendingFrames();

    for (const IsoTpEvent& event : m_connection.events()) {
        DiagnosticEvent out;
        out.channel = m_connection.address().channel;
        out.timestampNs = event.timestampNs != 0 ? event.timestampNs : nowNs;
        out.error = event.error;

        switch (event.kind) {
        case IsoTpEvent::Kind::MessageReceived:
            out.kind = DiagnosticEvent::Kind::MessageReceived;
            out.identifier = m_connection.address().receiveId;
            out.data = event.data;
            ++m_received;
            break;

        case IsoTpEvent::Kind::SendComplete:
            out.kind = DiagnosticEvent::Kind::MessageSent;
            out.identifier = m_connection.address().transmitId;
            ++m_sent;
            break;

        case IsoTpEvent::Kind::SendFailed:
        case IsoTpEvent::Kind::ReceiveFailed:
            out.kind = DiagnosticEvent::Kind::TransferFailed;
            out.identifier = event.kind == IsoTpEvent::Kind::SendFailed
                                 ? m_connection.address().transmitId
                                 : m_connection.address().receiveId;
            ++m_failed;
            break;
        }

        m_outgoingEvents.push_back(std::move(out));
    }

    m_connection.clearEvents();
}

void IsoTpNode::process(NodeContext& context)
{
    m_outgoingFrames.clear();
    m_outgoingEvents.clear();

    // One clock for the whole pass. The connection's timeouts, the periodic
    // request and the timestamps on what comes out all have to agree, and
    // reading the clock three times is three chances for them not to.
    const auto nowNs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - m_started)
                                       .count());

    // --- Frames from the bus ---------------------------------------------
    for (const CanFrame& frame : context.in<CanFrame>(0)) {
        static_cast<void>(m_connection.onFrame(frame, nowNs));
    }

    // --- Requests from upstream ------------------------------------------
    for (const DiagnosticEvent& request : context.in<DiagnosticEvent>(1)) {
        if (request.data.empty()) {
            continue;
        }

        if (m_connection.send(request.data, nowNs).failed()) {
            // Refused because the last one is still in flight. Counted rather
            // than queued: a queue would hide the fact that the previous
            // request was never answered, which is the thing worth knowing.
            ++m_refused;
        }
    }

    // --- The block's own periodic request ---------------------------------
    if (!m_periodicRequest.empty() && nowNs >= m_nextRequestNs) {
        const bool due = m_intervalNs > 0 || !m_sentFirstRequest;

        if (due) {
            if (m_connection.send(m_periodicRequest, nowNs).failed()) {
                ++m_refused;
            }

            m_sentFirstRequest = true;

            // An interval of zero means once, at Start - a question asked when
            // the measurement begins and not again. Pushed out of reach rather
            // than special-cased on every pass.
            m_nextRequestNs =
                m_intervalNs > 0 ? nowNs + m_intervalNs : std::numeric_limits<std::uint64_t>::max();
        }
    }

    // --- Let time pass ----------------------------------------------------
    m_connection.poll(nowNs);

    drain(nowNs);

    if (!m_outgoingFrames.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoingFrames});
    }

    if (!m_outgoingEvents.empty()) {
        context.publish<DiagnosticEvent>(1, std::span<const DiagnosticEvent>{m_outgoingEvents});
    }
}

} // namespace torquebus
