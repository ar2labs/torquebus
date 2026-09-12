// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// J1939 transport: how a message longer than eight bytes crosses the bus.
//
// Above eight bytes J1939 has two mechanisms, and **neither of them is
// ISO-TP**. The temptation to reuse IsoTpConnection is real and it is wrong:
// another header, another state machine, another numbering, another handshake.
// A transport that is *almost* another one is the most expensive way to share
// code, because every later fix has to be read twice to find out which of the
// two protocols it was for.
//
//   * **TP** point to point - RTS and CTS (PGN 60416) with the data packets
//     (60160). The receiver says how much it will take and when.
//   * **BAM** broadcast - announced and then poured out, no handshake, at least
//     50 ms between packets. Nobody acknowledges anything, and nobody can ask
//     for a packet again.
//
// A message is 9..1785 bytes: 255 packets of seven. Eight or fewer does not use
// transport at all, and a declared size outside that range is a malformed
// announcement rather than a very small or very large message.
//
// --- This reassembles; it does not negotiate ---------------------------------
//
// TorqueBus watches a bus it is not part of, which is the same stance address
// claiming takes: the two ECUs in a TP conversation are talking to each other,
// and a tool that injected a CTS into their handshake would change the traffic
// it exists to observe. So the CTS and the end-of-message acknowledgement are
// read and not answered, and both sides of somebody else's transfer reassemble
// the same way a broadcast does.
//
// Sending a transport message - which the rest bus and a Lua ECU will both want
// - is a separate piece, and it is deliberately not in this file.
//
// --- The refusal this file exists for ----------------------------------------
//
// **A session that loses a packet is abandoned, never patched.** Seven bytes of
// zero in a hole produce a message that reassembles, decodes, and lies - and an
// oil pressure of zero read out of a hole is indistinguishable from one that
// was measured. Losing the message is recoverable. Believing it is not.

#pragma once

#include "core/can/CanFrame.h"
#include "core/j1939/J1939Id.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace torquebus {

/// Extended transport, connection management. 52736.
///
/// Recognised and declined; see J1939TransportError::Unsupported.
inline constexpr std::uint32_t kPgnExtendedTransportConnection = 0x0'CE00U;

/// Extended transport, data transfer. 51712.
inline constexpr std::uint32_t kPgnExtendedTransportData = 0x0'CA00U;

/// The TP.CM control byte values this file acts on.
enum class J1939TransportControl : std::uint8_t {
    RequestToSend = 16U,
    ClearToSend = 17U,
    EndOfMessageAck = 19U,
    BroadcastAnnounce = 32U,
    Abort = 255U,
};

/// Why a transfer ended without a message.
enum class J1939TransportError : std::uint8_t {
    None,

    /// Too long since the previous data packet. T1 in the standard, 750 ms.
    Timeout,

    /// A packet was skipped. On BAM this is terminal by construction: there is
    /// no handshake to ask for it again.
    SequenceGap,

    /// The same packet number arrived twice.
    DuplicateSequence,

    /// A new announcement arrived from the same source before this one
    /// finished. The standard allows one session per pair of addresses, and the
    /// new announcement is the one that means something.
    Superseded,

    /// The declared size is not 9..1785 bytes.
    SizeOutOfRange,

    /// The declared packet count does not match the declared size. One of the
    /// two numbers is wrong and there is no way to tell which.
    PacketCountMismatch,

    /// The sender gave up, with a Conn Abort.
    AbortedByPeer,

    /// Extended transport, or another transport this build does not implement.
    /// Reported rather than ignored: a message that is never mentioned looks
    /// like a bus that never carried it.
    Unsupported,
};

/// Something the caller has to know about.
struct J1939TransportEvent final {
    enum class Kind : std::uint8_t {
        /// A complete message was reassembled. `data` is it.
        MessageReceived,

        /// A transfer stopped. What had arrived is discarded.
        ReceiveFailed,
    };

    Kind kind{Kind::MessageReceived};
    J1939TransportError error{J1939TransportError::None};

    /// The PGN being carried, which is *not* the transport PGN: it comes from
    /// the last three bytes of the announcement.
    std::uint32_t pgn{0U};

    std::uint8_t sourceAddress{0U};
    std::uint8_t destinationAddress{kJ1939GlobalAddress};
    std::uint8_t priority{7U};

    /// Arrived by BAM rather than by a negotiated transfer.
    bool broadcast{false};

    /// The reassembled message, for MessageReceived only.
    std::vector<std::uint8_t> data;

    /// How far a failed transfer had got. Worth showing: "9 of 20 packets"
    /// names a bus that is dropping traffic, and "0 of 20" names one that never
    /// started.
    std::uint8_t packetsReceived{0U};
    std::uint8_t packetsExpected{0U};

    /// On the same clock the caller passes in.
    std::uint64_t timestampNs{0U};
};

/// Reassembles every transport session on one bus at once.
///
/// Sessions are keyed by the pair of addresses, because that is the scope the
/// standard gives them: two ECUs can each be running a transfer to a third at
/// the same time, and a single-session reassembler would silently drop one of
/// them on a busy machine.
class J1939Transport final {
public:
    /// Shortest message that uses transport at all. Eight bytes fit in a frame.
    static constexpr std::size_t kMinimumMessage = 9U;

    /// 255 packets of seven bytes.
    static constexpr std::size_t kMaximumMessage = 1785U;

    /// Bytes of payload in each data packet; the first is the sequence number.
    static constexpr std::size_t kBytesPerPacket = 7U;

    /// T1: longest gap between data packets before the receiver gives up.
    static constexpr std::uint64_t kPacketTimeoutNs = 750ULL * 1000ULL * 1000ULL;

    /// Offers a frame that arrived on the bus.
    ///
    /// Returns true when the frame was a transport frame - which is what a
    /// caller needs in order to keep it out of ordinary decoding, since a
    /// TP.DT decoded as if it were a message is seven bytes of somebody else
    /// payload under a sequence number.
    bool onFrame(const CanFrame& frame, std::uint64_t nowNs);

    /// Lets time pass and fires the timeouts that have come due.
    ///
    /// A BAM that stops halfway leaves nothing else to react to: there is no
    /// handshake and no abort, just silence, so the only thing that ends the
    /// session is the clock.
    void poll(std::uint64_t nowNs);

    [[nodiscard]] std::span<const J1939TransportEvent> events() const noexcept
    {
        return m_events;
    }

    void clearEvents() { m_events.clear(); }

    /// How many transfers are in flight. For a statistics panel, and for a test
    /// to prove that a finished session does not linger.
    [[nodiscard]] std::size_t openSessions() const noexcept { return m_sessions.size(); }

    /// Drops everything in flight, quietly. For a measurement stopping - not
    /// for an error, which produces an event.
    void reset();

private:
    struct Session final {
        std::uint32_t pgn{0U};
        std::uint8_t priority{7U};
        std::uint8_t sourceAddress{0U};
        std::uint8_t destinationAddress{kJ1939GlobalAddress};
        bool broadcast{false};

        std::uint16_t totalSize{0U};
        std::uint8_t totalPackets{0U};

        /// The packet number expected next, 1-based as the standard numbers it.
        std::uint8_t nextSequence{1U};

        std::vector<std::uint8_t> data;
        std::uint64_t lastFrameNs{0U};

        [[nodiscard]] std::uint8_t packetsReceived() const noexcept
        {
            return static_cast<std::uint8_t>(nextSequence - 1U);
        }
    };

    /// Source address in the high byte, destination in the low one.
    using Key = std::uint16_t;

    [[nodiscard]] static Key keyFor(std::uint8_t source, std::uint8_t destination) noexcept
    {
        return static_cast<Key>((static_cast<Key>(source) << 8U) | destination);
    }

    void onConnectionManagement(const CanFrame& frame,
                                const J1939Id& id,
                                std::uint64_t nowNs);

    void onDataTransfer(const CanFrame& frame, const J1939Id& id, std::uint64_t nowNs);

    void beginSession(const CanFrame& frame,
                      const J1939Id& id,
                      bool broadcast,
                      std::uint64_t nowNs);

    /// Emits a ReceiveFailed for `session` and forgets it.
    void fail(const Session& session, J1939TransportError error, std::uint64_t nowNs);

    /// Emits a ReceiveFailed for a transfer that never got a session.
    void failAnnouncement(const J1939Id& id,
                          std::uint32_t pgn,
                          J1939TransportError error,
                          std::uint8_t expectedPackets,
                          std::uint64_t nowNs);

    std::map<Key, Session> m_sessions;
    std::vector<J1939TransportEvent> m_events;
};

} // namespace torquebus
