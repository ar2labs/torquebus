// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Node.h"

#include <algorithm>
#include <format>

namespace torquebus {
namespace {

/// Signals in the widest message of the database, which is how much room one
/// decoded message can need.
[[nodiscard]] std::size_t widestMessage(const CanDatabase& database)
{
    std::size_t widest = 0;
    for (const CanMessage& message : database.messages()) {
        widest = std::max(widest, message.signalList.size());
    }

    return widest;
}

} // namespace

Result J1939Node::prepare(std::size_t maximumBatchSize)
{
    m_buffer.clear();
    m_byPgn.clear();
    m_transport.reset();
    m_addresses.reset();
    m_diagnostics.clear();

    if (!m_database || m_database->empty()) {
        // A block with no database is what a freshly dropped one looks like,
        // and refusing to build the graph would mean a project can only be
        // assembled in one order.
        return Result::ok();
    }

    for (const CanMessage& message : m_database->messages()) {
        if (message.format != CanFrameFormat::Extended) {
            // An 11-bit message is not J1939 and has no PGN. Indexing one would
            // invent a group number out of its top byte.
            continue;
        }

        // Last definition wins, which is the rule CanDatabase already applies to
        // duplicate identifiers. Two messages sharing a PGN is the ordinary
        // case here rather than a conflict: a database that names the same
        // group for several source addresses collapses to one entry, which is
        // exactly what matching by PGN is for.
        m_byPgn[j1939Decompose(message.identifier).pgn()] = &message;
    }

    const std::size_t widest = widestMessage(*m_database);
    const std::size_t worstCase = maximumBatchSize * widest;

    constexpr std::size_t kMaximumBufferedSignals = 8U * 1024U * 1024U;
    if (worstCase > kMaximumBufferedSignals) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("J1939 block '{}' would need room for {} signals per pass "
                        "({} frames times {} signals in the widest message), "
                        "which is more than any real bus produces",
                        m_label, worstCase, maximumBatchSize, widest));
    }

    m_buffer.resize(worstCase);
    return Result::ok();
}

std::size_t J1939Node::decodeInto(std::uint32_t pgn,
                                  std::uint32_t identifier,
                                  std::uint8_t channel,
                                  std::uint64_t timestampNs,
                                  const std::uint8_t* payload,
                                  std::size_t length,
                                  std::size_t at)
{
    const auto found = m_byPgn.find(pgn);
    if (found == m_byPgn.end()) {
        ++m_unknownPgns;
        return 0;
    }

    const CanMessage* message = found->second;
    ++m_decodedMessages;

    std::size_t written = 0;

    for (const CanSignal* signal : message->signalsIn(payload, length)) {
        if (at + written >= m_buffer.size()) {
            // Unreachable: prepare() sized for the worst case. Kept because
            // "unreachable" is a claim about code somewhere else.
            break;
        }

        DecodedSignal& decoded = m_buffer[at + written];
        decoded.timestampNs = timestampNs;
        decoded.message = message;
        decoded.signal = signal;
        decoded.raw = signal->rawValue(payload, length);
        decoded.value = static_cast<double>(decoded.raw) * signal->factor + signal->offset;
        decoded.identifier = identifier;
        decoded.channel = channel;
        decoded.truncated = !signal->fitsIn(length);

        ++written;
    }

    return written;
}

void J1939Node::process(NodeContext& context)
{
    const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
    if (incoming.empty()) {
        return;
    }

    std::size_t count = 0;
    std::uint64_t latestNs = 0;

    for (const CanFrame& frame : incoming) {
        if (frame.error || frame.rtr) {
            // No payload to read. A remote frame has a length and no bytes, and
            // decoding it would produce a full set of plausible zeroes.
            continue;
        }

        ++m_frames;
        latestNs = frame.timestampNs;

        const std::optional<J1939Id> id = j1939Decompose(frame);
        if (!id.has_value()) {
            // An 11-bit frame sharing the wire. Not ours, and not an error.
            continue;
        }

        m_addresses.onFrame(frame, frame.timestampNs);

        if (m_transport.onFrame(frame, frame.timestampNs)) {
            // Claimed by the reassembler. It must not also be decoded as a
            // message: a TP.DT is seven bytes of payload under a sequence
            // number, and every signal read out of it would be wrong.
            continue;
        }

        if (j1939IsDiagnosticPgn(id->pgn())) {
            if (std::optional<J1939Diagnostic> message = j1939DecodeDiagnostic(
                    id->pgn(), id->sourceAddress,
                    std::span<const std::uint8_t>{frame.data.data(), frame.length},
                    m_spnReading, frame.timestampNs);
                message.has_value()) {
                ++m_diagnosticMessages;
                recordDiagnostic(std::move(*message));
            }

            // A DM1 may also be described in the database, and somebody who
            // went to the trouble of writing the lamp bits as signals should
            // get them - so this falls through rather than returning.
        }

        if (m_buffer.empty()) {
            continue;
        }

        count += decodeInto(id->pgn(), frame.identifier, frame.channel, frame.timestampNs,
                            frame.data.data(), frame.length, count);
    }

    // Anything the reassembler finished during this batch, plus the sessions
    // whose clock ran out while these frames were arriving.
    m_transport.poll(latestNs);

    for (const J1939TransportEvent& event : m_transport.events()) {
        if (event.kind != J1939TransportEvent::Kind::MessageReceived) {
            ++m_transportFailures;
            continue;
        }

        ++m_transportMessages;

        if (j1939IsDiagnosticPgn(event.pgn)) {
            if (std::optional<J1939Diagnostic> message = j1939DecodeDiagnostic(
                    event.pgn, event.sourceAddress, event.data, m_spnReading,
                    event.timestampNs);
                message.has_value()) {
                ++m_diagnosticMessages;
                recordDiagnostic(std::move(*message));
            }
        }

        if (m_buffer.empty()) {
            continue;
        }

        count += decodeInto(event.pgn,
                            j1939Identifier(event.pgn, event.sourceAddress,
                                            event.destinationAddress, event.priority),
                            0U, event.timestampNs, event.data.data(), event.data.size(),
                            count);
    }

    m_transport.clearEvents();
    m_addresses.clearEvents();

    if (count == 0) {
        return;
    }

    m_emitted += count;
    context.publish<DecodedSignal>(0, std::span<const DecodedSignal>{m_buffer.data(), count});
}

void J1939Node::recordDiagnostic(J1939Diagnostic message)
{
    const auto found = std::find_if(m_diagnostics.begin(), m_diagnostics.end(),
                                    [&message](const J1939Diagnostic& existing) {
                                        return existing.sourceAddress == message.sourceAddress
                                               && existing.active == message.active;
                                    });

    if (found != m_diagnostics.end()) {
        // A fault list is a statement about now. An older copy of the same ECU
        // list is not history, it is a stale answer to the same question.
        *found = std::move(message);
        return;
    }

    const auto position =
        std::lower_bound(m_diagnostics.begin(), m_diagnostics.end(), message,
                         [](const J1939Diagnostic& left, const J1939Diagnostic& right) {
                             if (left.sourceAddress != right.sourceAddress) {
                                 return left.sourceAddress < right.sourceAddress;
                             }

                             // DM1 before DM2 for the same ECU: what is wrong
                             // now is read before what used to be.
                             return left.active && !right.active;
                         });

    m_diagnostics.insert(position, std::move(message));
}

void J1939Node::finish()
{
    // A transfer still in flight when the measurement stops is not news, and
    // the address table belongs to the run that just ended.
    m_transport.reset();
}

std::vector<NodeStatistic> J1939Node::statistics() const
{
    std::size_t activeFaults = 0;
    for (const J1939Diagnostic& message : m_diagnostics) {
        if (message.active) {
            activeFaults += message.faults.size();
        }
    }

    return {
        NodeStatistic{"Frames", m_frames},
        NodeStatistic{"Messages decoded", m_decodedMessages},
        NodeStatistic{"Signals emitted", m_emitted},

        // Worth a glance next to the decoded count: a bus where most PGNs are
        // unknown is a bus with the wrong database loaded, and it looks quiet
        // rather than broken.
        NodeStatistic{"PGNs not in the database", m_unknownPgns},

        NodeStatistic{"Transport messages", m_transportMessages},

        // And this one next to that: transfers that started and did not finish
        // name a bus dropping traffic.
        NodeStatistic{"Transport failures", m_transportFailures},

        NodeStatistic{"ECUs seen", m_addresses.nodes().size()},
        NodeStatistic{"Active faults", activeFaults},
    };
}

} // namespace torquebus
