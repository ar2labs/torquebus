// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Transport.h"

#include <algorithm>

namespace torquebus {
namespace {

/// A TP.CM is always eight bytes. Fewer is a malformed announcement, and
/// reading past what arrived would build a session out of whatever was left in
/// the frame buffer.
constexpr std::uint8_t kConnectionBytes = 8U;

/// A TP.DT is always eight: one sequence number and seven of payload. The last
/// packet of a message is padded, which is why the total size is carried in the
/// announcement rather than inferred from the packet count.
constexpr std::uint8_t kDataBytes = 8U;

/// The three PGN bytes at the end of a TP.CM, least significant first.
[[nodiscard]] std::uint32_t carriedPgn(const CanFrame& frame) noexcept
{
    return static_cast<std::uint32_t>(frame.data[5])
           | (static_cast<std::uint32_t>(frame.data[6]) << 8U)
           | (static_cast<std::uint32_t>(frame.data[7]) << 16U);
}

/// How many packets a message of `size` bytes needs.
[[nodiscard]] constexpr std::size_t packetsFor(std::size_t size) noexcept
{
    return (size + J1939Transport::kBytesPerPacket - 1U) / J1939Transport::kBytesPerPacket;
}

} // namespace

bool J1939Transport::onFrame(const CanFrame& frame, std::uint64_t nowNs)
{
    const std::optional<J1939Id> id = j1939Decompose(frame);
    if (!id.has_value()) {
        return false;
    }

    const std::uint32_t pgn = id->pgn();

    if (pgn == kPgnTransportConnection) {
        onConnectionManagement(frame, *id, nowNs);
        return true;
    }

    if (pgn == kPgnTransportData) {
        onDataTransfer(frame, *id, nowNs);
        return true;
    }

    if (pgn == kPgnExtendedTransportConnection) {
        // Announced rather than ignored. A message nobody ever mentions looks
        // like a bus that never carried it, and somebody then goes looking for
        // a wiring fault that is not there.
        failAnnouncement(*id, carriedPgn(frame), J1939TransportError::Unsupported, 0U, nowNs);
        return true;
    }

    if (pgn == kPgnExtendedTransportData) {
        // Its announcement has already been reported; saying so again once per
        // packet would bury the panel under the same sentence.
        return true;
    }

    return false;
}

void J1939Transport::onConnectionManagement(const CanFrame& frame,
                                            const J1939Id& id,
                                            std::uint64_t nowNs)
{
    if (frame.length < kConnectionBytes) {
        return;
    }

    const auto control = static_cast<J1939TransportControl>(frame.data[0]);

    switch (control) {
    case J1939TransportControl::BroadcastAnnounce:
        beginSession(frame, id, true, nowNs);
        return;

    case J1939TransportControl::RequestToSend:
        beginSession(frame, id, false, nowNs);
        return;

    case J1939TransportControl::Abort: {
        const auto existing = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
        if (existing != m_sessions.end()) {
            fail(existing->second, J1939TransportError::AbortedByPeer, nowNs);
        }
        return;
    }

    case J1939TransportControl::ClearToSend:
    case J1939TransportControl::EndOfMessageAck:
        // Read and not answered: these belong to the two ECUs negotiating with
        // each other, and this is a bystander.
        return;
    }

    // An unknown control byte belongs to a revision of the protocol this build
    // does not know. Silence is right here - inventing a session from a byte
    // whose meaning is unknown is worse than missing the message.
}

void J1939Transport::beginSession(const CanFrame& frame,
                                  const J1939Id& id,
                                  bool broadcast,
                                  std::uint64_t nowNs)
{
    const std::uint32_t pgn = carriedPgn(frame);
    const auto size = static_cast<std::uint16_t>(frame.data[1]
                                                 | (static_cast<std::uint16_t>(frame.data[2])
                                                    << 8U));
    const std::uint8_t packets = frame.data[3];

    // A new announcement from the same pair replaces whatever was in flight.
    // The standard allows one session between two addresses at a time, and the
    // announcement that just arrived is the one that means something - keeping
    // the old one would also leak a buffer on a bench where somebody is
    // power-cycling an ECU over and over.
    if (const auto existing = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
        existing != m_sessions.end()) {
        fail(existing->second, J1939TransportError::Superseded, nowNs);
    }

    if (size < kMinimumMessage || size > kMaximumMessage) {
        failAnnouncement(id, pgn, J1939TransportError::SizeOutOfRange, packets, nowNs);
        return;
    }

    if (packets != packetsFor(size)) {
        // One of the two numbers is wrong and there is no way to tell which, so
        // neither is trusted: trusting the size would overrun a short transfer,
        // and trusting the count would truncate a long one.
        failAnnouncement(id, pgn, J1939TransportError::PacketCountMismatch, packets, nowNs);
        return;
    }

    Session session;
    session.pgn = pgn;
    session.priority = id.priority;
    session.sourceAddress = id.sourceAddress;
    session.destinationAddress = id.destinationAddress();
    session.broadcast = broadcast;
    session.totalSize = size;
    session.totalPackets = packets;
    session.nextSequence = 1U;
    session.lastFrameNs = nowNs;
    session.data.reserve(size);

    m_sessions.insert_or_assign(keyFor(id.sourceAddress, id.destinationAddress()),
                                std::move(session));
}

void J1939Transport::onDataTransfer(const CanFrame& frame,
                                    const J1939Id& id,
                                    std::uint64_t nowNs)
{
    if (frame.length < kDataBytes) {
        return;
    }

    const auto found = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
    if (found == m_sessions.end()) {
        // Data with no announcement: a transfer that began before the
        // measurement did, or one whose announcement was missed. There is
        // nothing to reassemble it into, and guessing a size from the packets
        // that happen to arrive would produce a message of unknown length.
        return;
    }

    Session& session = found->second;
    const std::uint8_t sequence = frame.data[0];

    if (sequence < session.nextSequence) {
        fail(session, J1939TransportError::DuplicateSequence, nowNs);
        return;
    }

    if (sequence > session.nextSequence) {
        // The refusal this file exists for. Seven bytes of zero in the hole
        // would produce a message that reassembles, decodes and lies.
        fail(session, J1939TransportError::SequenceGap, nowNs);
        return;
    }

    // The last packet is padded out to seven bytes, so the declared size is
    // what decides where the message ends.
    const std::size_t remaining = session.totalSize - session.data.size();
    const std::size_t take = std::min<std::size_t>(kBytesPerPacket, remaining);

    session.data.insert(session.data.end(),
                        frame.data.begin() + 1,
                        frame.data.begin() + 1 + static_cast<std::ptrdiff_t>(take));

    session.nextSequence = static_cast<std::uint8_t>(session.nextSequence + 1U);
    session.lastFrameNs = nowNs;

    if (session.packetsReceived() < session.totalPackets) {
        return;
    }

    J1939TransportEvent event;
    event.kind = J1939TransportEvent::Kind::MessageReceived;
    event.pgn = session.pgn;
    event.sourceAddress = session.sourceAddress;
    event.destinationAddress = session.destinationAddress;
    event.priority = session.priority;
    event.broadcast = session.broadcast;
    event.data = std::move(session.data);
    event.packetsReceived = session.totalPackets;
    event.packetsExpected = session.totalPackets;
    event.timestampNs = nowNs;

    m_events.push_back(std::move(event));
    m_sessions.erase(found);
}

void J1939Transport::poll(std::uint64_t nowNs)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (nowNs >= it->second.lastFrameNs
            && nowNs - it->second.lastFrameNs > kPacketTimeoutNs) {
            // fail() does not erase, so that this loop owns the iterator.
            J1939TransportEvent event;
            event.kind = J1939TransportEvent::Kind::ReceiveFailed;
            event.error = J1939TransportError::Timeout;
            event.pgn = it->second.pgn;
            event.sourceAddress = it->second.sourceAddress;
            event.destinationAddress = it->second.destinationAddress;
            event.priority = it->second.priority;
            event.broadcast = it->second.broadcast;
            event.packetsReceived = it->second.packetsReceived();
            event.packetsExpected = it->second.totalPackets;
            event.timestampNs = nowNs;

            m_events.push_back(std::move(event));
            it = m_sessions.erase(it);
            continue;
        }

        ++it;
    }
}

void J1939Transport::fail(const Session& session,
                          J1939TransportError error,
                          std::uint64_t nowNs)
{
    J1939TransportEvent event;
    event.kind = J1939TransportEvent::Kind::ReceiveFailed;
    event.error = error;
    event.pgn = session.pgn;
    event.sourceAddress = session.sourceAddress;
    event.destinationAddress = session.destinationAddress;
    event.priority = session.priority;
    event.broadcast = session.broadcast;
    event.packetsReceived = session.packetsReceived();
    event.packetsExpected = session.totalPackets;
    event.timestampNs = nowNs;

    m_events.push_back(std::move(event));
    m_sessions.erase(keyFor(session.sourceAddress, session.destinationAddress));
}

void J1939Transport::failAnnouncement(const J1939Id& id,
                                      std::uint32_t pgn,
                                      J1939TransportError error,
                                      std::uint8_t expectedPackets,
                                      std::uint64_t nowNs)
{
    J1939TransportEvent event;
    event.kind = J1939TransportEvent::Kind::ReceiveFailed;
    event.error = error;
    event.pgn = pgn;
    event.sourceAddress = id.sourceAddress;
    event.destinationAddress = id.destinationAddress();
    event.priority = id.priority;
    event.broadcast = id.isBroadcast();
    event.packetsReceived = 0U;
    event.packetsExpected = expectedPackets;
    event.timestampNs = nowNs;

    m_events.push_back(std::move(event));
}

void J1939Transport::reset()
{
    m_sessions.clear();
    m_events.clear();
}

} // namespace torquebus
