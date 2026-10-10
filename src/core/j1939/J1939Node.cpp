// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Node.h"

#include "core/j1939/J1939Id.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>

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

/// Whether `raw` is one of the values J1939 sets aside for "error" and "not available" in this
/// signal - and not something a number of that width is simply allowed to be.
///
/// isJ1939SpecialValue() is the standard's table, and applied to every signal it is wrong in two
/// places that a database can see and a table cannot:
///
///   * A signed signal has no such values. Its raw reads as a negative number, which as unsigned is
///     the biggest there is, and every negative reading would be "not available".
///   * A value the database has given a name to is a value it has given a meaning. "Right" for a
///     turn stalk, "Headlamps" for a light switch: an enumeration that uses 2 in a field of two
///     bits is not reporting an error, and the .dbc says so in the one place it can, a VAL_ entry.
///
/// What it does not do is read the declared range. A .dbc declares 0 to 3 for a two-bit status and
/// 0 to 255 for a byte as a matter of course - the whole field, and no statement about which of its
/// values mean something - so a range that reaches the raw value says nothing about it.
[[nodiscard]] bool isReserved(const CanSignal& signal, std::int64_t raw) noexcept
{
    return !signal.isSigned && raw >= 0
           && isJ1939SpecialValue(static_cast<std::uint64_t>(raw), signal.bitLength)
           && signal.nameForValue(raw).empty();
}

} // namespace

Result J1939Node::prepare(std::size_t maximumBatchSize)
{
    m_buffer.clear();
    m_byPgn.clear();
    m_transport.reset();
    m_addresses.reset();
    m_diagnostics.clear();
    m_newestNs = 0;

    // Before the database is looked at: the lamps of the bus come from the diagnostic messages,
    // which need no database, and a block that has not been given one yet must still publish them.
    m_publishedLamps = {-1, -1, -1, -1};
    if (m_variables != nullptr) {
        const std::array<std::string_view, 4> names{kJ1939LampStopVariable,
                                                    kJ1939LampWarningVariable,
                                                    kJ1939LampMilVariable,
                                                    kJ1939LampProtectVariable};
        for (std::size_t lamp = 0; lamp < names.size(); ++lamp) {
            m_lampVariables[lamp] = m_variables->resolve(std::string{names[lamp]});
        }
    }
    publishLamps(0);

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
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("J1939 block '{}' would need room for {} signals per pass "
                                         "({} frames times {} signals in the widest message), "
                                         "which is more than any real bus produces",
                                         m_label,
                                         worstCase,
                                         maximumBatchSize,
                                         widest));
    }

    m_buffer.resize(worstCase);
    m_present.reserve(widest);

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

    message->signalsIn(payload, length, m_present);

    for (const CanSignal* signal : m_present) {
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
        if (isReserved(*signal, decoded.raw)) {
            decoded.value = std::numeric_limits<double>::quiet_NaN();
        } else {
            decoded.value = static_cast<double>(decoded.raw) * signal->factor + signal->offset;
        }
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
        m_newestNs = std::max(m_newestNs, frame.timestampNs);

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
                    id->pgn(),
                    id->sourceAddress,
                    std::span<const std::uint8_t>{frame.data.data(), frame.length},
                    m_spnReading,
                    frame.timestampNs);
                message.has_value()) {
                ++m_diagnosticMessages;
                recordDiagnostic(std::move(*message));
                m_networkDirty = true;
            }

            // A DM1 may also be described in the database, and somebody who
            // went to the trouble of writing the lamp bits as signals should
            // get them - so this falls through rather than returning.
        }

        if (m_buffer.empty()) {
            continue;
        }

        count += decodeInto(id->pgn(),
                            frame.identifier,
                            frame.channel,
                            frame.timestampNs,
                            frame.data.data(),
                            frame.length,
                            count);
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
                    event.pgn, event.sourceAddress, event.data, m_spnReading, event.timestampNs);
                message.has_value()) {
                ++m_diagnosticMessages;
                recordDiagnostic(std::move(*message));
                m_networkDirty = true;
            }
        }

        if (m_buffer.empty()) {
            continue;
        }

        count += decodeInto(
            event.pgn,
            j1939Identifier(
                event.pgn, event.sourceAddress, event.destinationAddress, event.priority),
            0U,
            event.timestampNs,
            event.data.data(),
            event.data.size(),
            count);
    }

    // Anything the address table noticed - a claim, a contest, an ECU heard
    // from for the first time - is a change a panel would want to show.
    m_networkDirty = m_networkDirty || !m_addresses.events().empty();

    m_transport.clearEvents();
    m_addresses.clearEvents();

    if (m_networkDirty && m_network != nullptr) {
        // One lock and one set of copies per pass, never per frame. See
        // J1939Network.h for why that distinction is the whole design.
        m_network->publish({m_addresses.nodes().begin(), m_addresses.nodes().end()},
                           {m_addresses.defeated().begin(), m_addresses.defeated().end()},
                           m_diagnostics);
        m_networkDirty = false;
    }

    // Every pass, and not only when a DM1 came: a lamp goes out when its ECU falls silent, which is
    // the one change that arrives as the absence of a frame.
    publishLamps(m_newestNs);

    if (count == 0) {
        return;
    }

    m_emitted += count;
    context.publish<DecodedSignal>(0, std::span<const DecodedSignal>{m_buffer.data(), count});
}

void J1939Node::recordDiagnostic(J1939Diagnostic message)
{
    const auto found = std::find_if(
        m_diagnostics.begin(), m_diagnostics.end(), [&message](const J1939Diagnostic& existing) {
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
        std::lower_bound(m_diagnostics.begin(),
                         m_diagnostics.end(),
                         message,
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

void J1939Node::publishLamps(std::uint64_t nowNs) noexcept
{
    if (m_variables == nullptr) {
        return;
    }

    // Stop, warning, malfunction, protect: the order of the variables, and of nothing else.
    std::array<std::int8_t, 4> lit{0, 0, 0, 0};

    for (const J1939Diagnostic& message : m_diagnostics) {
        // A DM2 is what *was* wrong, and a lamp lit by it would be a repaired fault on the dash.
        if (!message.active) {
            continue;
        }

        // Not current: its ECU has gone quiet. A timestamp from the future, which a replay that
        // looped makes, is as current as it gets.
        if (nowNs > message.timestampNs
            && nowNs - message.timestampNs > kJ1939DiagnosticCurrentNs) {
            continue;
        }

        const auto on = [](J1939LampState state) {
            return state == J1939LampState::On ? std::int8_t{1} : std::int8_t{0};
        };

        lit[0] = std::max(lit[0], on(message.lamps.redStop));
        lit[1] = std::max(lit[1], on(message.lamps.amberWarning));
        lit[2] = std::max(lit[2], on(message.lamps.malfunction));
        lit[3] = std::max(lit[3], on(message.lamps.protect));
    }

    for (std::size_t lamp = 0; lamp < lit.size(); ++lamp) {
        if (lit[lamp] == m_publishedLamps[lamp]) {
            continue;
        }

        m_publishedLamps[lamp] = lit[lamp];
        m_variables->set(m_lampVariables[lamp], static_cast<double>(lit[lamp]));
    }
}

void J1939Node::finish()
{
    // A transfer still in flight when the measurement stops is not news, and
    // the address table belongs to the run that just ended.
    m_transport.reset();

    // The lamps are a statement about a bus that is no longer there. The cluster's signals go to
    // dashes after Stop; a variable would otherwise hold its last value for ever, and a stopped
    // measurement would show a red lamp for a fault nobody can still be reporting. The diagnostics
    // themselves stay: they are what a panel reads afterwards.
    if (m_variables != nullptr) {
        for (std::size_t lamp = 0; lamp < m_lampVariables.size(); ++lamp) {
            m_publishedLamps[lamp] = 0;
            m_variables->set(m_lampVariables[lamp], 0.0);
        }
    }
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
