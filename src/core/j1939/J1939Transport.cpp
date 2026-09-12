// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Transport.h"

#include <algorithm>

namespace torquebus {
namespace {

/// A TP.CM or ETP.CM is always eight bytes. Fewer is a malformed announcement,
/// and reading past what arrived would build a session out of whatever was left
/// in the frame buffer.
constexpr std::uint8_t kConnectionBytes = 8U;

/// A data frame is always eight: one sequence number and seven of payload. The
/// last packet of a message is padded, which is why the total size is carried
/// in the announcement rather than inferred from the packet count.
constexpr std::uint8_t kDataBytes = 8U;

/// The three PGN bytes at the end of every connection-management message,
/// least significant first. Same position in TP and ETP.
[[nodiscard]] std::uint32_t carriedPgn(const CanFrame& frame) noexcept
{
    return static_cast<std::uint32_t>(frame.data[5])
           | (static_cast<std::uint32_t>(frame.data[6]) << 8U)
           | (static_cast<std::uint32_t>(frame.data[7]) << 16U);
}

/// TP announces its size in two bytes; ETP in four.
[[nodiscard]] std::uint32_t announcedSize(const CanFrame& frame) noexcept
{
    return static_cast<std::uint32_t>(frame.data[1])
           | (static_cast<std::uint32_t>(frame.data[2]) << 8U);
}

[[nodiscard]] std::uint32_t announcedExtendedSize(const CanFrame& frame) noexcept
{
    return static_cast<std::uint32_t>(frame.data[1])
           | (static_cast<std::uint32_t>(frame.data[2]) << 8U)
           | (static_cast<std::uint32_t>(frame.data[3]) << 16U)
           | (static_cast<std::uint32_t>(frame.data[4]) << 24U);
}

/// The packet offset an ETP.DPO declares: three bytes, least significant first.
[[nodiscard]] std::uint32_t announcedOffset(const CanFrame& frame) noexcept
{
    return static_cast<std::uint32_t>(frame.data[2])
           | (static_cast<std::uint32_t>(frame.data[3]) << 8U)
           | (static_cast<std::uint32_t>(frame.data[4]) << 16U);
}

/// How many packets a message of `size` bytes needs.
[[nodiscard]] constexpr std::uint32_t packetsFor(std::uint32_t size) noexcept
{
    return static_cast<std::uint32_t>((size + J1939Transport::kBytesPerPacket - 1U)
                                      / J1939Transport::kBytesPerPacket);
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
        if (frame.length >= kDataBytes) {
            // TP numbers its packets from one, straight through.
            onDataTransfer(*id, frame, static_cast<std::uint32_t>(frame.data[0]) - 1U, nowNs);
        }

        return true;
    }

    if (pgn == kPgnExtendedTransportConnection) {
        onExtendedConnection(frame, *id, nowNs);
        return true;
    }

    if (pgn == kPgnExtendedTransportData) {
        if (frame.length < kDataBytes) {
            return true;
        }

        const auto found = m_sessions.find(keyFor(id->sourceAddress, id->destinationAddress()));
        if (found == m_sessions.end()) {
            return true;
        }

        if (!found->second.offsetDeclared) {
            // Data before any offset was declared. Assuming the first window
            // would put these bytes at the start of the message and silently
            // misplace every window after it.
            fail(found->second, J1939TransportError::MissingPacketOffset, nowNs);
            return true;
        }

        // An ETP sequence number restarts at one inside every offset window, so
        // the absolute position is the window plus the number within it.
        onDataTransfer(*id, frame,
                       found->second.packetOffset
                           + static_cast<std::uint32_t>(frame.data[0]) - 1U,
                       nowNs);

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

    switch (static_cast<J1939TransportControl>(frame.data[0])) {
    case J1939TransportControl::BroadcastAnnounce:
        beginSession(frame, id, true, false, announcedSize(frame), nowNs);
        return;

    case J1939TransportControl::RequestToSend:
        beginSession(frame, id, false, false, announcedSize(frame), nowNs);
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

void J1939Transport::onExtendedConnection(const CanFrame& frame,
                                          const J1939Id& id,
                                          std::uint64_t nowNs)
{
    if (frame.length < kConnectionBytes) {
        return;
    }

    switch (static_cast<J1939ExtendedControl>(frame.data[0])) {
    case J1939ExtendedControl::RequestToSend:
        // ETP is always destination specific; there is no broadcast form.
        beginSession(frame, id, false, true, announcedExtendedSize(frame), nowNs);
        return;

    case J1939ExtendedControl::DataPacketOffset: {
        const auto existing = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
        if (existing == m_sessions.end() || !existing->second.extended) {
            return;
        }

        Session& session = existing->second;
        const std::uint32_t offset = announcedOffset(frame);

        // The window has to start where the message has got to. A DPO that
        // jumps forward is the sender skipping a stretch it believes was
        // delivered, and accepting it would leave a hole that reassembles.
        if (offset != session.nextPacket) {
            fail(session, J1939TransportError::SequenceGap, nowNs);
            return;
        }

        session.packetOffset = offset;
        session.offsetDeclared = true;
        session.lastFrameNs = nowNs;
        return;
    }

    case J1939ExtendedControl::Abort: {
        const auto existing = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
        if (existing != m_sessions.end()) {
            fail(existing->second, J1939TransportError::AbortedByPeer, nowNs);
        }
        return;
    }

    case J1939ExtendedControl::ClearToSend:
    case J1939ExtendedControl::EndOfMessageAck:
        // The receiver's half of somebody else's handshake.
        return;
    }
}

void J1939Transport::beginSession(const CanFrame& frame,
                                  const J1939Id& id,
                                  bool broadcast,
                                  bool extended,
                                  std::uint32_t size,
                                  std::uint64_t nowNs)
{
    const std::uint32_t pgn = carriedPgn(frame);
    const std::uint32_t packets = extended ? packetsFor(size) : frame.data[3];

    // A new announcement from the same pair replaces whatever was in flight.
    // The standard allows one session between two addresses at a time, and the
    // announcement that just arrived is the one that means something - keeping
    // the old one would also leak a buffer on a bench where somebody is
    // power-cycling an ECU over and over.
    if (const auto existing = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
        existing != m_sessions.end()) {
        fail(existing->second, J1939TransportError::Superseded, nowNs);
    }

    const std::size_t lowest = extended ? kMinimumExtendedMessage : kMinimumMessage;
    const std::size_t highest = extended ? kMaximumExtendedMessage : kMaximumMessage;

    if (size < lowest || size > highest) {
        failAnnouncement(id, pgn, J1939TransportError::SizeOutOfRange, packets, nowNs);
        return;
    }

    // ETP derives its packet count from the size and carries no separate
    // number, so there is nothing to disagree with.
    if (!extended && packets != packetsFor(size)) {
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
    session.extended = extended;
    session.totalSize = size;
    session.totalPackets = packets;
    session.nextPacket = 0U;
    session.packetOffset = 0U;

    // TP has no offset message, so its window is declared by the announcement
    // itself and never moves.
    session.offsetDeclared = !extended;
    session.lastFrameNs = nowNs;
    session.data.reserve(size);

    m_sessions.insert_or_assign(keyFor(id.sourceAddress, id.destinationAddress()),
                                std::move(session));
}

void J1939Transport::onDataTransfer(const J1939Id& id,
                                    const CanFrame& frame,
                                    std::uint32_t packet,
                                    std::uint64_t nowNs)
{
    const auto found = m_sessions.find(keyFor(id.sourceAddress, id.destinationAddress()));
    if (found == m_sessions.end()) {
        // Data with no announcement: a transfer that began before the
        // measurement did, or one whose announcement was missed. There is
        // nothing to reassemble it into, and guessing a size from the packets
        // that happen to arrive would produce a message of unknown length.
        return;
    }

    Session& session = found->second;

    if (packet < session.nextPacket) {
        fail(session, J1939TransportError::DuplicateSequence, nowNs);
        return;
    }

    if (packet > session.nextPacket) {
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

    ++session.nextPacket;
    session.lastFrameNs = nowNs;

    if (session.nextPacket < session.totalPackets) {
        return;
    }

    J1939TransportEvent event;
    event.kind = J1939TransportEvent::Kind::MessageReceived;
    event.pgn = session.pgn;
    event.sourceAddress = session.sourceAddress;
    event.destinationAddress = session.destinationAddress;
    event.priority = session.priority;
    event.broadcast = session.broadcast;
    event.extended = session.extended;
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
            event.extended = it->second.extended;
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
    event.extended = session.extended;
    event.packetsReceived = session.packetsReceived();
    event.packetsExpected = session.totalPackets;
    event.timestampNs = nowNs;

    m_events.push_back(std::move(event));
    m_sessions.erase(keyFor(session.sourceAddress, session.destinationAddress));
}

void J1939Transport::failAnnouncement(const J1939Id& id,
                                      std::uint32_t pgn,
                                      J1939TransportError error,
                                      std::uint32_t expectedPackets,
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
    event.extended = id.pgn() == kPgnExtendedTransportConnection;
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
