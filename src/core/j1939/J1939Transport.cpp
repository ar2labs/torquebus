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

namespace {

/// ISO 11783-3:2018 Table 9. Only the values that differ from the ordinary
/// table, plus the ones that exist only here.
[[nodiscard]] std::string_view extendedAbortReasonText(std::uint8_t reason) noexcept
{
    switch (static_cast<J1939ExtendedAbortReason>(reason)) {
    case J1939ExtendedAbortReason::AlreadyBusy:
        return "it is already in as many connections as it can manage";
    case J1939ExtendedAbortReason::ResourcesNeeded:
        return "its resources were needed for another task";
    case J1939ExtendedAbortReason::Timeout:
        return "a timeout closed the session";
    case J1939ExtendedAbortReason::UnexpectedClearToSend:
        return "a CTS arrived while data transfer was already in progress";
    case J1939ExtendedAbortReason::RetransmitLimitReached:
        return "the retransmit request limit was reached";
    case J1939ExtendedAbortReason::UnexpectedDataPacket:
        return "an unexpected data packet arrived";
    case J1939ExtendedAbortReason::BadSequenceNumber:
        return "a bad sequence number it could not recover from";
    case J1939ExtendedAbortReason::DuplicateSequenceNumber:
        return "a duplicate sequence number it could not recover from";
    case J1939ExtendedAbortReason::UnexpectedOffsetPacket:
        return "an unexpected offset packet arrived";
    case J1939ExtendedAbortReason::BadOffsetPgn:
        return "the offset packet named the wrong PGN";
    case J1939ExtendedAbortReason::OffsetPacketCountAboveClearToSend:
        return "the offset declared more packets than the CTS allowed";
    case J1939ExtendedAbortReason::BadOffset:
        return "the offset itself was wrong";
    case J1939ExtendedAbortReason::Deprecated:
        return "a reason the standard deprecated in favour of 250";
    case J1939ExtendedAbortReason::BadClearToSendPgn:
        return "the CTS named the wrong PGN";
    case J1939ExtendedAbortReason::ClearToSendExceedsMessage:
        return "the CTS asked for more packets than the message holds";
    case J1939ExtendedAbortReason::NotListed:
        return "a reason the standard has no code for";
    case J1939ExtendedAbortReason::None:
        break;
    }

    // 251 to 255 belong to ISO 11783-7 here rather than to J1939-71. Same
    // shape of answer, different document to open.
    //
    // The prose above Table 9 says to use 254 for a reason that is not listed
    // while the table itself says 250, and row 13 says 250 too. Two of three
    // say 250, so 250 is what this treats as the catch-all - and a 254 falls
    // into the reserved range below, which is honest about not knowing.
    if (reason >= 251U) {
        return "a reason ISO 11783-7 defines";
    }

    return "a reason reserved for future assignment";
}

} // namespace

std::string_view j1939AbortReasonText(std::uint8_t reason, bool extended) noexcept
{
    if (extended) {
        return extendedAbortReasonText(reason);
    }

    switch (static_cast<J1939AbortReason>(reason)) {
    case J1939AbortReason::AlreadyBusy:
        return "it is already in as many connections as it can manage";
    case J1939AbortReason::ResourcesNeeded:
        return "its resources were needed for another task";
    case J1939AbortReason::Timeout:
        return "a timeout closed the session";
    case J1939AbortReason::UnexpectedClearToSend:
        return "a CTS arrived while data transfer was already in progress";
    case J1939AbortReason::RetransmitLimitReached:
        return "the retransmit request limit was reached";
    case J1939AbortReason::UnexpectedDataPacket:
        return "an unexpected data packet arrived";
    case J1939AbortReason::BadSequenceNumber:
        return "a bad sequence number it could not recover from";
    case J1939AbortReason::DuplicateSequenceNumber:
        return "a duplicate sequence number it could not recover from";
    case J1939AbortReason::SizeTooLarge:
        return "the announced size was greater than 1785 bytes";
    case J1939AbortReason::NotListed:
        return "a reason the standard has no code for";
    case J1939AbortReason::None:
        break;
    }

    // Saying which range the rest falls in tells somebody where to look -
    // J1939-71, or nowhere, in which case an ECU is sending a code nobody
    // assigned. Saying more than that would be inventing it.
    if (reason >= 251U) {
        return "a reason J1939-71 defines";
    }

    return "a reason reserved for SAE assignment";
}

std::string_view j1939AbortRoleText(J1939AbortRole role) noexcept
{
    switch (role) {
    case J1939AbortRole::Originator:
        return "the sender of the data";
    case J1939AbortRole::Responder:
        return "the receiver of the data";
    case J1939AbortRole::Reserved:
        return "an end the standard has not defined";
    case J1939AbortRole::Unspecified:
        break;
    }

    return "an end that did not say which it was";
}

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
            // Byte 2 of the message is the reason and byte 3 carries the role
            // in its low two bits - J1939-21 Figure 14 and Table 6, counting
            // the control byte as byte 1.
            const auto role = static_cast<J1939AbortRole>(frame.data[2] & 0x03U);

            fail(existing->second, J1939TransportError::AbortedByPeer, nowNs,
                 frame.data[1], role);
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
            // No role: the layout of an extended abort belongs to ISO 11783-3,
            // which this project has not read. Reading J1939-21's byte here
            // would be a guess wearing the clothes of a fact.
            fail(existing->second, J1939TransportError::AbortedByPeer, nowNs, frame.data[1]);
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
        // T1 counts from a packet, so before the first one there is nothing for
        // it to count from. A negotiated transfer is waiting on a handshake
        // between two other ECUs at that point, and T3 is what the standard
        // gives the sender to wait for it. A broadcast has no handshake - the
        // packets follow the announcement directly - so T1 applies from the
        // start and a dead BAM is reported half a second sooner.
        const bool started = it->second.packetsReceived() > 0U;
        const std::uint64_t patience =
            (started || it->second.broadcast) ? kPacketTimeoutNs : kHandshakeTimeoutNs;

        if (nowNs >= it->second.lastFrameNs && nowNs - it->second.lastFrameNs > patience) {
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
                          std::uint64_t nowNs,
                          std::uint8_t abortReason,
                          J1939AbortRole abortRole)
{
    J1939TransportEvent event;
    event.kind = J1939TransportEvent::Kind::ReceiveFailed;
    event.error = error;
    event.abortReason = abortReason;
    event.abortRole = abortRole;
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
