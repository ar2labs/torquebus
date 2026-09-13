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
//   * **ETP** the same idea again for messages too long for the first one (PGNs
//     51200 and 50944). A calibration or a firmware image, not a measurement.
//
// A TP message is 9..1785 bytes: 255 packets of seven, which is all a one-byte
// sequence number can count. ETP moves a **data packet offset** along the
// message and lets the sequence numbers count inside that window, which is how
// a transfer reaches past 1785 - and is the only structural difference between
// the two protocols.
//
// A declared size outside the range of the protocol that announced it is a
// malformed announcement rather than a very small or very large message.
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
#include <string_view>
#include <vector>

namespace torquebus {

// --- Where these numbers came from, and the surprise in it -----------------
//
// The classic transport - the two PGNs, the five control bytes, the layout of
// every message and Table 6 - is SAE J1939-21 MAY2022, section 5.10 and
// Figures 13 and 14, read directly.
//
// **The extended transport is not in J1939-21 at all.** Not in the 2022
// revision, not under that name or any other: section 5.10 ends at TP.DT, and
// the control byte ranges say in as many words that 20 to 31 are "Reserved for
// SAE Assignment" - which is precisely the range ETP uses. That was worth
// finding out, because until somebody looks it reads like a gap in this file
// rather than a different document.
//
// What is SAE is the two PGN *numbers*: the J1939 Digital Annex registers
// 50944 as ETP.DT and 51200 as ETP.CM, and both were checked there.
//
// The *behaviour* behind them - the control bytes, the offset window, the
// four-byte size - belongs to **ISO 11783-3**, the ISOBUS data link layer.
// This project does not have that document, so those specifics rest on the
// Linux kernel implementation (net/can/j1939), which has run against real
// machines for years. Every one of them is stated by extension in
// J1939TransportTests, so checking them against a copy of ISO 11783-3 is
// reading one screen.
//
// This is not a footnote about paperwork. ISOBUS is agricultural, which is
// exactly the fleet this milestone is for, and knowing that ETP arrives from
// there tells somebody debugging a tractor which standard to open.

/// Extended transport, connection management. 51200.
inline constexpr std::uint32_t kPgnExtendedTransportConnection = 0x0'C800U;

/// Extended transport, data transfer. 50944.
inline constexpr std::uint32_t kPgnExtendedTransportData = 0x0'C700U;

/// The TP.CM control byte values, as J1939-21 Figure 14 gives them.
enum class J1939TransportControl : std::uint8_t {
    RequestToSend = 16U,
    ClearToSend = 17U,
    EndOfMessageAck = 19U,
    BroadcastAnnounce = 32U,
    Abort = 255U,
};

/// The ETP.CM control byte values.
///
/// DPO has no equivalent in TP and is the whole reason ETP exists: the sequence
/// number in a data frame is one byte, so it can only count to 255. DPO moves a
/// window along the message and the sequence numbers count inside it, which is
/// how a transfer reaches past 1785 bytes.
enum class J1939ExtendedControl : std::uint8_t {
    RequestToSend = 20U,
    ClearToSend = 21U,
    DataPacketOffset = 22U,
    EndOfMessageAck = 23U,
    Abort = 255U,
};

/// Why a sender gave up, as SAE J1939-21 MAY2022 Table 6 defines it.
///
/// Nine values have meanings, 10 to 249 are reserved for SAE, 250 means "a
/// reason that is not in the table", and 251 to 255 are left to J1939-71. This
/// decodes what the standard decodes and reports the rest as the number it was
/// - inventing a sentence for a reserved code would be putting words in the
/// mouth of an ECU.
///
/// Six of these did not exist in the revision this file was first written
/// against, and were being reported as "reserved for SAE assignment" - which
/// was wrong in the worst way available here, because four of the six name a
/// specific defect in the transfer that just failed.
enum class J1939AbortReason : std::uint8_t {
    /// Reserved by the standard; also what a frame that carried no reason
    /// leaves behind.
    None = 0U,

    /// Already in as many connection managed sessions as it can support.
    AlreadyBusy = 1U,

    /// System resources were needed for another task.
    ResourcesNeeded = 2U,

    /// A timeout occurred and this is the abort that closes the session.
    Timeout = 3U,

    /// A CTS arrived while data transfer was already in progress.
    UnexpectedClearToSend = 4U,

    /// The maximum retransmit request limit was reached.
    RetransmitLimitReached = 5U,

    /// A data transfer packet arrived that was not expected.
    UnexpectedDataPacket = 6U,

    /// Bad sequence number, and the software cannot recover.
    BadSequenceNumber = 7U,

    /// Duplicate sequence number, and the software cannot recover.
    DuplicateSequenceNumber = 8U,

    /// The announced total message size was greater than 1785 bytes.
    SizeTooLarge = 9U,

    /// A reason the sender could not find in the table. The standard reserves
    /// this value for exactly that, which is more useful than it looks: it
    /// distinguishes "something else went wrong" from a code nobody assigned.
    NotListed = 250U,
};

/// Who sent the abort, from bits 1-2 of byte 3 of a TP.Conn_Abort.
///
/// Added to J1939-21 after the revision this file was first written against.
/// It answers a question the reason code does not: a timeout reported by the
/// receiver and a timeout reported by the sender are the same word for two
/// different faults, and which end gave up says which end to look at.
enum class J1939AbortRole : std::uint8_t {
    /// The Controller Application that sent the RTS.
    Originator = 0U,

    /// The Controller Application that sent the CTS.
    Responder = 1U,

    /// Reserved for assignment by SAE.
    Reserved = 2U,

    /// The sender did not say. The standard marks this "not recommended for
    /// new implementations", so seeing it is itself worth knowing.
    Unspecified = 3U,
};

/// The role in words.
[[nodiscard]] std::string_view j1939AbortRoleText(J1939AbortRole role) noexcept;

/// The abort reason in words, or a description of the range it falls in.
[[nodiscard]] std::string_view j1939AbortReasonText(std::uint8_t reason) noexcept;

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

    /// The declared size is outside what the announced protocol carries:
    /// 9..1785 for TP, 1786..kMaximumExtendedMessage for ETP.
    SizeOutOfRange,

    /// The declared packet count does not match the declared size. One of the
    /// two numbers is wrong and there is no way to tell which.
    PacketCountMismatch,

    /// The sender gave up, with a Conn Abort.
    AbortedByPeer,

    /// A data packet arrived for an extended transfer before any offset was
    /// declared. Without one there is nowhere in the message to put it, and
    /// assuming the first window would silently misplace every later one.
    MissingPacketOffset,

    /// The announced protocol is one this build does not implement. Reported
    /// rather than ignored: a message that is never mentioned looks like a bus
    /// that never carried it.
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

    /// Carried by the extended protocol. Worth showing: an ETP transfer is a
    /// firmware image or a calibration, and it takes seconds rather than
    /// milliseconds.
    bool extended{false};

    /// The byte a Conn Abort carried, for error == AbortedByPeer and zero
    /// otherwise. Kept as the raw number rather than only as the decoded enum,
    /// because most of the range is reserved and a reserved code somebody is
    /// really sending is worth seeing exactly as it arrived.
    std::uint8_t abortReason{0U};

    /// Which end gave up. Only a TP abort carries this - the layout is
    /// J1939-21's, and the extended protocol belongs to another standard this
    /// project has not read - so an ETP abort reports Unspecified rather than
    /// a guess dressed up as a fact.
    J1939AbortRole abortRole{J1939AbortRole::Unspecified};

    /// The reassembled message, for MessageReceived only.
    std::vector<std::uint8_t> data;

    /// How far a failed transfer had got. Worth showing: "9 of 20 packets"
    /// names a bus that is dropping traffic, and "0 of 20" names one that never
    /// started.
    /// Wide enough for ETP, where a message can run to millions of packets.
    std::uint32_t packetsReceived{0U};
    std::uint32_t packetsExpected{0U};

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

    /// 255 packets of seven bytes: everything a one-byte sequence can count.
    static constexpr std::size_t kMaximumMessage = 1785U;

    /// Shortest message ETP carries. Anything this protocol could hold is TP's
    /// job, and an ETP announcement below the line is a sender that has the two
    /// protocols confused.
    static constexpr std::size_t kMinimumExtendedMessage = kMaximumMessage + 1U;

    /// Longest extended message this build will assemble.
    ///
    /// The protocol allows 117,440,505 bytes - a 24-bit packet count times
    /// seven - which is a number no ECU on a bench means and every fuzzer
    /// tries. This is a firmware image, which is the largest thing anybody
    /// legitimately sends, and it is the same ceiling IsoTpConnection puts on
    /// the same question. An announcement above it is refused with its size
    /// named, rather than turned into an allocation somebody else chose.
    static constexpr std::size_t kMaximumExtendedMessage = 16U * 1024U * 1024U;

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
        bool extended{false};

        std::uint32_t totalSize{0U};
        std::uint32_t totalPackets{0U};

        /// Absolute index of the packet expected next, counting from zero.
        ///
        /// Absolute rather than the sequence number on the wire, because an ETP
        /// sequence number restarts at one inside every offset window. Keeping
        /// the absolute index means TP and ETP take the same path through
        /// onDataTransfer, and the only difference is how the index is worked
        /// out from the frame.
        std::uint32_t nextPacket{0U};

        /// The offset the last DPO declared, in packets. Always zero for TP,
        /// which has no such message.
        std::uint32_t packetOffset{0U};

        /// An ETP transfer has had a DPO. Data before the first one has nowhere
        /// to go.
        bool offsetDeclared{false};

        std::vector<std::uint8_t> data;
        std::uint64_t lastFrameNs{0U};

        [[nodiscard]] std::uint32_t packetsReceived() const noexcept { return nextPacket; }
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

    void onExtendedConnection(const CanFrame& frame,
                              const J1939Id& id,
                              std::uint64_t nowNs);

    /// `packet` is the absolute index, already worked out for the protocol the
    /// frame belongs to.
    void onDataTransfer(const J1939Id& id,
                        const CanFrame& frame,
                        std::uint32_t packet,
                        std::uint64_t nowNs);

    void beginSession(const CanFrame& frame,
                      const J1939Id& id,
                      bool broadcast,
                      bool extended,
                      std::uint32_t size,
                      std::uint64_t nowNs);

    /// Emits a ReceiveFailed for `session` and forgets it.
    void fail(const Session& session,
              J1939TransportError error,
              std::uint64_t nowNs,
              std::uint8_t abortReason = 0U,
              J1939AbortRole abortRole = J1939AbortRole::Unspecified);

    /// Emits a ReceiveFailed for a transfer that never got a session.
    void failAnnouncement(const J1939Id& id,
                          std::uint32_t pgn,
                          J1939TransportError error,
                          std::uint32_t expectedPackets,
                          std::uint64_t nowNs);

    std::map<Key, Session> m_sessions;
    std::vector<J1939TransportEvent> m_events;
};

} // namespace torquebus
