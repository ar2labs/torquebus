// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Node.h"

#include "core/j1939/J1939Id.h"

#include <algorithm>
#include <chrono>
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
    return !signal.isSigned
           && isJ1939SpecialValue(static_cast<std::uint64_t>(raw), signal.bitLength)
           && signal.nameForValue(raw).empty();
}

/// The table of a bus nothing has been heard from: nobody on it. What the accessors answer for a
/// channel the block has no frames from, so that asking is never a question about a null pointer.
[[nodiscard]] const J1939AddressTable& nobody() noexcept
{
    static const J1939AddressTable table;
    return table;
}

} // namespace

Result J1939Node::prepare(std::size_t maximumBatchSize)
{
    m_buffer.clear();
    m_byPgn.clear();
    m_buses.clear();

    // Before the database is looked at: the lamps of the bus come from the diagnostic messages,
    // which need no database, and a block that has not been given one yet must still publish them.
    m_shownLamps = {-1, -1, -1, -1};
    if (m_variables != nullptr) {
        const std::array<std::string_view, 4> names{kJ1939LampStopVariable,
                                                    kJ1939LampWarningVariable,
                                                    kJ1939LampMilVariable,
                                                    kJ1939LampProtectVariable};
        for (std::size_t lamp = 0; lamp < names.size(); ++lamp) {
            m_lampVariables[lamp] = m_variables->resolve(std::string{names[lamp]});
        }

        // No bus yet, so nothing lit: the lamps start dark whatever the last measurement left.
        showLamps();
    }

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

J1939Node::Bus& J1939Node::busOn(std::uint8_t channel)
{
    const auto position = std::lower_bound(
        m_buses.begin(),
        m_buses.end(),
        channel,
        [](const std::unique_ptr<Bus>& bus, std::uint8_t wanted) { return bus->channel < wanted; });

    if (position != m_buses.end() && (*position)->channel == channel) {
        return **position;
    }

    // The first frame of a bus: the one place a bus is made, and a bus is made once in a
    // measurement. Kept in channel order, which is the order the hand-over lists them in.
    return **m_buses.insert(position, std::make_unique<Bus>(channel));
}

const J1939Node::Bus* J1939Node::findBus(std::uint8_t channel) const noexcept
{
    const auto position = std::lower_bound(
        m_buses.begin(),
        m_buses.end(),
        channel,
        [](const std::unique_ptr<Bus>& bus, std::uint8_t wanted) { return bus->channel < wanted; });

    return position != m_buses.end() && (*position)->channel == channel ? position->get() : nullptr;
}

std::vector<std::uint8_t> J1939Node::channels() const
{
    std::vector<std::uint8_t> channels;
    channels.reserve(m_buses.size());

    for (const std::unique_ptr<Bus>& bus : m_buses) {
        channels.push_back(bus->channel);
    }

    return channels;
}

const J1939AddressTable& J1939Node::addresses(std::uint8_t channel) const noexcept
{
    const Bus* bus = findBus(channel);

    return bus != nullptr ? bus->addresses : nobody();
}

std::span<const J1939Diagnostic> J1939Node::diagnostics(std::uint8_t channel) const noexcept
{
    const Bus* bus = findBus(channel);

    return bus != nullptr ? std::span<const J1939Diagnostic>{bus->diagnostics}
                          : std::span<const J1939Diagnostic>{};
}

const J1939AddressTable& J1939Node::addresses() const noexcept
{
    return m_buses.empty() ? nobody() : m_buses.front()->addresses;
}

std::span<const J1939Diagnostic> J1939Node::diagnostics() const noexcept
{
    return m_buses.empty() ? std::span<const J1939Diagnostic>{}
                           : std::span<const J1939Diagnostic>{m_buses.front()->diagnostics};
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
            const double unscaled =
                (!signal->isSigned && signal->bitLength == 64U)
                    ? static_cast<double>(static_cast<std::uint64_t>(decoded.raw))
                    : static_cast<double>(decoded.raw);
            decoded.value = unscaled * signal->factor + signal->offset;
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

    std::size_t count = 0;

    for (const std::unique_ptr<Bus>& bus : m_buses) {
        bus->passFrames = 0U;
        bus->passLatestNs = 0U;
    }

    Bus* current = nullptr;

    for (const CanFrame& frame : incoming) {
        if (frame.error || frame.rtr) {
            // No payload to read. A remote frame has a length and no bytes, and
            // decoding it would produce a full set of plausible zeroes.
            continue;
        }

        ++m_frames;

        // The bus a frame is on is the channel it arrived on, and everything the block learns from
        // it is learnt about that bus. Frames come in runs from one channel, so the bus of the last
        // one is tried first.
        if (current == nullptr || current->channel != frame.channel) {
            current = &busOn(frame.channel);
        }

        Bus& bus = *current;

        ++bus.passFrames;
        bus.passLatestNs = frame.timestampNs;
        bus.newestNs = std::max(bus.newestNs, frame.timestampNs);

        const std::optional<J1939Id> id = j1939Decompose(frame);
        if (!id.has_value()) {
            // An 11-bit frame sharing the wire. Not ours, and not an error.
            continue;
        }

        bus.addresses.onFrame(frame, frame.timestampNs);

        if (bus.transport.onFrame(frame, frame.timestampNs)) {
            // Claimed by the reassembler. It must not also be decoded as a
            // message: a TP.DT is seven bytes of payload under a sequence
            // number, and every signal read out of it would be wrong.
            continue;
        }

        if (j1939IsDiagnosticPgn(id->pgn())) {
            if (std::optional<J1939Diagnostic> message = j1939DecodeDiagnostic(
                    id->pgn(),
                    id->sourceAddress,
                    std::span<const std::uint8_t>{frame.data.data(), frame.payloadSize()},
                    m_spnReading,
                    frame.timestampNs);
                message.has_value()) {
                ++m_diagnosticMessages;
                if (recordDiagnostic(bus, std::move(*message))) {
                    bus.networkDirty = true;
                }
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
                            frame.payloadSize(),
                            count);
    }

    // Each bus on its own account: what came from it this pass, or that nothing did.
    for (const std::unique_ptr<Bus>& owned : m_buses) {
        Bus& bus = *owned;

        if (bus.passFrames == 0U) {
            // Nothing to read from this bus in this pass: an idle bus, or one that carries only
            // error and remote frames, which say nothing a DM1 could be repeated by. An idle bus is
            // the normal state of one between its messages and the state of one that has fallen
            // silent altogether; the two look alike from here, and the difference is how long it
            // goes on. Another bus talking says nothing about this one.
            ageOnAnIdlePass(bus);
            continue;
        }

        // Frames again: the bus has a clock of its own once more, and it is theirs.
        bus.idleSinceNs.reset();

        // Anything the reassembler finished during this batch, plus the sessions
        // whose clock ran out while these frames were arriving.
        bus.transport.poll(bus.passLatestNs);

        for (const J1939TransportEvent& event : bus.transport.events()) {
            if (event.kind != J1939TransportEvent::Kind::MessageReceived) {
                ++m_transportFailures;
                continue;
            }

            ++m_transportMessages;

            if (j1939IsDiagnosticPgn(event.pgn)) {
                if (std::optional<J1939Diagnostic> message =
                        j1939DecodeDiagnostic(event.pgn,
                                              event.sourceAddress,
                                              event.data,
                                              m_spnReading,
                                              event.timestampNs);
                    message.has_value()) {
                    ++m_diagnosticMessages;
                    if (recordDiagnostic(bus, std::move(*message))) {
                        bus.networkDirty = true;
                    }
                }
            }

            if (m_buffer.empty()) {
                continue;
            }

            count += decodeInto(
                event.pgn,
                j1939Identifier(
                    event.pgn, event.sourceAddress, event.destinationAddress, event.priority),
                event.channel,
                event.timestampNs,
                event.data.data(),
                event.data.size(),
                count);
        }

        // Anything the address table noticed - a claim, a contest, an ECU heard
        // from for the first time - is a change a panel would want to show.
        bus.networkDirty = bus.networkDirty || !bus.addresses.events().empty();

        bus.transport.clearEvents();
        bus.addresses.clearEvents();

        // Every pass, and not only when a DM1 came: a lamp goes out and a code leaves the list when
        // its ECU falls silent, which is the one change that arrives as the absence of a frame.
        settle(bus, bus.newestNs);
    }

    if (count == 0) {
        return;
    }

    m_emitted += count;
    context.publish<DecodedSignal>(0, std::span<const DecodedSignal>{m_buffer.data(), count});
}

void J1939Node::settle(Bus& bus, std::uint64_t busNowNs)
{
    // What the bus is reporting now, and who is repeating a DM1, into lists that keep their
    // capacity: a bus with a fault on it costs no allocation per pass, and a steady one no
    // publication at all.
    j1939CollectActiveFaults(bus.diagnostics, busNowNs, bus.scratchFaults);
    j1939CollectReporting(bus.diagnostics, busNowNs, bus.scratchReporting);

    if (bus.scratchFaults != bus.activeFaults) {
        bus.activeFaults.swap(bus.scratchFaults);
        bus.networkDirty = true;
    }

    // The ECU that has gone quiet with nothing to report is a change nothing else in the pass
    // notices - no code left the list - and is the one a panel needs to be told of.
    if (bus.scratchReporting != bus.reporting) {
        bus.reporting.swap(bus.scratchReporting);
        bus.networkDirty = true;
    }

    if (bus.networkDirty && m_network != nullptr) {
        // One lock and one set of copies per bus per pass, never per frame. See
        // J1939Network.h for why that distinction is the whole design.
        publishView(bus);
        bus.networkDirty = false;
    }

    publishLamps(bus, busNowNs);
}

void J1939Node::publishView(const Bus& bus)
{
    J1939BusView view;
    view.channel = bus.channel;
    view.nodes.assign(bus.addresses.nodes().begin(), bus.addresses.nodes().end());
    view.defeated.assign(bus.addresses.defeated().begin(), bus.addresses.defeated().end());
    view.diagnostics = bus.diagnostics;
    view.activeFaults = bus.activeFaults;
    view.reporting = bus.reporting;

    m_network->publish(std::move(view));
}

void J1939Node::ageOnAnIdlePass(Bus& bus)
{
    // No ECU of this bus is repeating a DM1: a bus that has said nothing yet, or has only said what
    // is over. A code on the list and a lamp that is lit both come from a DM1 that is being
    // repeated, so nothing can go out of date - ageing only ever takes away. An ECU repeating a DM1
    // with nothing in it is something to take away as well, and the clock is read for it.
    if (bus.reporting.empty()) {
        return;
    }

    const std::uint64_t hostNowNs = m_hostClock();

    if (!bus.idleSinceNs.has_value()) {
        // The first pass without a frame: the bus has been quiet for no time at all yet.
        bus.idleSinceNs = hostNowNs;
        return;
    }

    // The clock of the bus is the newest frame it carried, and it stopped when the frames did. The
    // machine's did not: a bus that has been silent for three seconds is silent in the way an ECU
    // that stopped repeating its DM1 is, and is aged by the same rule - from where its clock stood.
    // Only how long it has been quiet is taken from the machine, never what time it is, so the two
    // clocks are not compared: a replayed log, or a device that stamps its own, has a bus time of
    // its own.
    const std::uint64_t quietNs = hostNowNs > *bus.idleSinceNs ? hostNowNs - *bus.idleSinceNs : 0U;

    settle(bus, bus.newestNs + quietNs);
}

std::uint64_t J1939Node::steadyNanoseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool J1939Node::recordDiagnostic(Bus& bus, J1939Diagnostic message)
{
    const auto found = std::find_if(bus.diagnostics.begin(),
                                    bus.diagnostics.end(),
                                    [&message](const J1939Diagnostic& existing) {
                                        return existing.sourceAddress == message.sourceAddress
                                               && existing.active == message.active;
                                    });

    if (found != bus.diagnostics.end()) {
        const bool changed = found->lamps != message.lamps || found->flash != message.flash
                             || found->faults != message.faults;
        // A fault list is a statement about now. An older copy of the same ECU
        // list is not history, it is a stale answer to the same question.
        *found = std::move(message);
        return changed;
    }

    const auto position =
        std::lower_bound(bus.diagnostics.begin(),
                         bus.diagnostics.end(),
                         message,
                         [](const J1939Diagnostic& left, const J1939Diagnostic& right) {
                             if (left.sourceAddress != right.sourceAddress) {
                                 return left.sourceAddress < right.sourceAddress;
                             }

                             // DM1 before DM2 for the same ECU: what is wrong
                             // now is read before what used to be.
                             return left.active && !right.active;
                         });

    bus.diagnostics.insert(position, std::move(message));
    return true;
}

void J1939Node::publishLamps(Bus& bus, std::uint64_t nowNs)
{
    if (m_variables == nullptr) {
        return;
    }

    // Stop, warning, malfunction, protect: the order of the variables, and of nothing else.
    std::array<std::int8_t, 4> lit{0, 0, 0, 0};

    for (const J1939Diagnostic& message : bus.diagnostics) {
        // A DM2 is what *was* wrong, and a lamp lit by it would be a repaired fault on the dash;
        // and a DM1 whose ECU has gone quiet is not still lighting anything.
        if (!j1939DiagnosticIsCurrent(message, nowNs)) {
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

    bus.lamps = lit;
    showLamps();
}

void J1939Node::showLamps()
{
    // What the buses of this block light together: a cluster has one stop lamp, and the stop lamp
    // is lit when any bus says so.
    std::array<std::int8_t, 4> lit{0, 0, 0, 0};

    for (const std::unique_ptr<Bus>& bus : m_buses) {
        for (std::size_t lamp = 0; lamp < lit.size(); ++lamp) {
            lit[lamp] = std::max(lit[lamp], bus->lamps[lamp]);
        }
    }

    // Nothing moved on any of them, so nothing this block shows moved either: a steady bus writes
    // no variable and takes no lock.
    if (lit == m_shownLamps) {
        return;
    }

    m_shownLamps = lit;

    // With a network to share them through, the lamps are the whole vehicle's: what every J1939
    // block says its own buses light, together. Another block's fault must not go dark because
    // this block's went out. Without one, they are this block's.
    const std::array<std::int8_t, 4> shown =
        m_network != nullptr ? m_network->combineLamps(this, lit) : lit;

    for (std::size_t lamp = 0; lamp < shown.size(); ++lamp) {
        // Against what the variable says, and not against what this block wrote last: another
        // block may have written it since, and a copy of its own would be out of date.
        const auto value = static_cast<double>(shown[lamp]);

        if (m_variables->value(m_lampVariables[lamp]) != value) {
            m_variables->set(m_lampVariables[lamp], value);
        }
    }
}

void J1939Node::finish()
{
    // The lamps are a statement about buses that are no longer there. The cluster's signals go to
    // dashes after Stop; a variable would otherwise hold its last value for ever, and a stopped
    // measurement would show a red lamp for a fault nobody can still be reporting. The diagnostics
    // themselves stay: they are what a panel reads afterwards. Put out through the door they are
    // lit through: a J1939 block still to finish keeps the lamps of its buses lit until it does.
    for (const std::unique_ptr<Bus>& owned : m_buses) {
        Bus& bus = *owned;

        // A transfer still in flight when the measurement stops is not news.
        bus.transport.reset();
        bus.lamps = {0, 0, 0, 0};

        // And for the same reason the codes, and the ECUs that were repeating a DM1: a stopped
        // measurement is not reporting any. Published empty, with everything else as it was, so
        // that the panel that reads what the ECUs last said still can - and says that it is the
        // last thing they said - while a dashboard that lists what the bus is saying goes quiet
        // with the lamps.
        if (!bus.activeFaults.empty() || !bus.reporting.empty()) {
            bus.activeFaults.clear();
            bus.reporting.clear();

            if (m_network != nullptr) {
                publishView(bus);
            }
        }
    }

    if (m_variables != nullptr) {
        showLamps();
    }
}

std::vector<NodeStatistic> J1939Node::statistics() const
{
    std::size_t ecus = 0;
    std::size_t faults = 0;

    for (const std::unique_ptr<Bus>& bus : m_buses) {
        ecus += bus->addresses.nodes().size();
        faults += bus->activeFaults.size();
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

        // On every bus the block reads: an address is a seat on one bus, so the same number on two
        // is two ECUs.
        NodeStatistic{"ECUs seen", ecus},

        // What the buses are reporting now: the codes of every DM1 that is still being repeated,
        // which is not what an ECU last said - one that has gone quiet has stopped reporting its
        // faults.
        NodeStatistic{"Active faults", faults},
    };
}

} // namespace torquebus
