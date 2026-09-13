// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// J1939 transport reassembly.
//
// Most of these are about what must *not* come out. A transport that patches a
// hole produces a message that reassembles, decodes and reads as measured data
// - and there is nothing downstream that can tell it apart from the real thing.
// So every case where a packet is missing, repeated, or announced wrongly ends
// with no message at all, and the tests check the absence as carefully as they
// check the presence.

#include "core/j1939/J1939Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <algorithm>
#include <numeric>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint8_t kEngine = 0x00U;
constexpr std::uint8_t kGearbox = 0x03U;
constexpr std::uint8_t kTester = 0xF9U;

/// The PGN being carried by the transfers below - Engine Configuration, which
/// is a real message that really does need transport.
constexpr std::uint32_t kCarried = 0x0'FEE3U;

/// A TP.CM frame with the given control byte.
[[nodiscard]] CanFrame connectionFrame(J1939TransportControl control,
                                       std::uint32_t pgn,
                                       std::uint16_t size,
                                       std::uint8_t packets,
                                       std::uint8_t source,
                                       std::uint8_t destination)
{
    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnTransportConnection, source, destination, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    frame.data[0] = static_cast<std::uint8_t>(control);
    frame.data[1] = static_cast<std::uint8_t>(size & 0xFFU);
    frame.data[2] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
    frame.data[3] = packets;
    frame.data[4] = 0xFFU;
    frame.data[5] = static_cast<std::uint8_t>(pgn & 0xFFU);
    frame.data[6] = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
    frame.data[7] = static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU);

    return frame;
}

[[nodiscard]] CanFrame bam(std::uint16_t size, std::uint8_t packets, std::uint8_t source)
{
    return connectionFrame(J1939TransportControl::BroadcastAnnounce, kCarried, size, packets,
                           source, kJ1939GlobalAddress);
}

[[nodiscard]] CanFrame requestToSend(std::uint16_t size,
                                     std::uint8_t packets,
                                     std::uint8_t source,
                                     std::uint8_t destination)
{
    return connectionFrame(J1939TransportControl::RequestToSend, kCarried, size, packets,
                           source, destination);
}

/// A TP.DT frame. `payload` is padded to seven bytes with 0xFF, as a real
/// sender pads the last packet of a message.
[[nodiscard]] CanFrame dataFrame(std::uint8_t sequence,
                                 const std::vector<std::uint8_t>& payload,
                                 std::uint8_t source,
                                 std::uint8_t destination = kJ1939GlobalAddress)
{
    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnTransportData, source, destination, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    frame.data[0] = sequence;
    for (std::size_t index = 0U; index < J1939Transport::kBytesPerPacket; ++index) {
        frame.data[index + 1U] = index < payload.size() ? payload[index] : 0xFFU;
    }

    return frame;
}

/// An ETP.RTS: the size in four bytes rather than two.
[[nodiscard]] CanFrame extendedRts(std::uint32_t size,
                                   std::uint8_t source,
                                   std::uint8_t destination)
{
    CanFrame frame;
    frame.identifier =
        j1939Identifier(kPgnExtendedTransportConnection, source, destination, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    frame.data[0] = static_cast<std::uint8_t>(J1939ExtendedControl::RequestToSend);
    frame.data[1] = static_cast<std::uint8_t>(size & 0xFFU);
    frame.data[2] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
    frame.data[3] = static_cast<std::uint8_t>((size >> 16U) & 0xFFU);
    frame.data[4] = static_cast<std::uint8_t>((size >> 24U) & 0xFFU);
    frame.data[5] = static_cast<std::uint8_t>(kCarried & 0xFFU);
    frame.data[6] = static_cast<std::uint8_t>((kCarried >> 8U) & 0xFFU);
    frame.data[7] = static_cast<std::uint8_t>((kCarried >> 16U) & 0xFFU);

    return frame;
}

/// An ETP.DPO: how many packets follow, and where in the message they go.
[[nodiscard]] CanFrame extendedOffset(std::uint32_t packets,
                                      std::uint32_t offset,
                                      std::uint8_t source,
                                      std::uint8_t destination)
{
    CanFrame frame;
    frame.identifier =
        j1939Identifier(kPgnExtendedTransportConnection, source, destination, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    frame.data[0] = static_cast<std::uint8_t>(J1939ExtendedControl::DataPacketOffset);
    frame.data[1] = static_cast<std::uint8_t>(packets);
    frame.data[2] = static_cast<std::uint8_t>(offset & 0xFFU);
    frame.data[3] = static_cast<std::uint8_t>((offset >> 8U) & 0xFFU);
    frame.data[4] = static_cast<std::uint8_t>((offset >> 16U) & 0xFFU);
    frame.data[5] = static_cast<std::uint8_t>(kCarried & 0xFFU);
    frame.data[6] = static_cast<std::uint8_t>((kCarried >> 8U) & 0xFFU);
    frame.data[7] = static_cast<std::uint8_t>((kCarried >> 16U) & 0xFFU);

    return frame;
}

/// An ETP.DT. The sequence number counts inside the current window.
[[nodiscard]] CanFrame extendedData(std::uint8_t sequence,
                                    std::vector<std::uint8_t> seven,
                                    std::uint8_t source,
                                    std::uint8_t destination)
{
    seven.resize(J1939Transport::kBytesPerPacket, 0xFFU);
    seven.insert(seven.begin(), sequence);

    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnExtendedTransportData, source, destination, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;

    for (std::size_t index = 0U; index < seven.size() && index < 8U; ++index) {
        frame.data[index] = seven[index];
    }

    return frame;
}

/// 1, 2, 3 ... `size`. Distinguishable from padding, and from zero.
[[nodiscard]] std::vector<std::uint8_t> countingPayload(std::size_t size)
{
    std::vector<std::uint8_t> payload(size);
    std::iota(payload.begin(), payload.end(), std::uint8_t{1U});

    return payload;
}

/// The `index`-th seven-byte slice of `payload`, short at the end.
[[nodiscard]] std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& payload,
                                              std::size_t index)
{
    const std::size_t begin = index * J1939Transport::kBytesPerPacket;
    const std::size_t end = std::min(begin + J1939Transport::kBytesPerPacket, payload.size());

    return {payload.begin() + static_cast<std::ptrdiff_t>(begin),
            payload.begin() + static_cast<std::ptrdiff_t>(end)};
}

} // namespace

TEST_CASE("A broadcast message reassembles into exactly what was sent",
          "[j1939][transport]")
{
    // Twelve bytes is two packets, and the second one is padded - so this also
    // proves the padding does not reach the message.
    const std::vector<std::uint8_t> payload = countingPayload(12U);

    J1939Transport transport;
    CHECK(transport.onFrame(bam(12U, 2U, kEngine), 0U));
    CHECK(transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine), 1000U));
    CHECK(transport.onFrame(dataFrame(2U, slice(payload, 1U), kEngine), 2000U));

    REQUIRE(transport.events().size() == 1U);

    const J1939TransportEvent& event = transport.events()[0];
    CHECK(event.kind == J1939TransportEvent::Kind::MessageReceived);
    CHECK(event.pgn == kCarried);
    CHECK(event.sourceAddress == kEngine);
    CHECK(event.broadcast);
    CHECK(event.data == payload);

    // And the session is gone, rather than sitting there holding a buffer.
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("A missing packet abandons the message instead of patching it",
          "[j1939][transport]")
{
    // The refusal the whole file exists for. Packet two never arrives and three
    // does; seven bytes of anything in that hole would decode as measured data.
    const std::vector<std::uint8_t> payload = countingPayload(21U);

    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);
    transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine), 1000U);
    transport.onFrame(dataFrame(3U, slice(payload, 2U), kEngine), 2000U);

    REQUIRE(transport.events().size() == 1U);

    const J1939TransportEvent& event = transport.events()[0];
    CHECK(event.kind == J1939TransportEvent::Kind::ReceiveFailed);
    CHECK(event.error == J1939TransportError::SequenceGap);

    // How far it got, because "1 of 3" names a bus dropping traffic.
    CHECK(event.packetsReceived == 1U);
    CHECK(event.packetsExpected == 3U);
    CHECK(event.data.empty());
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("A repeated packet abandons the message", "[j1939][transport]")
{
    const std::vector<std::uint8_t> payload = countingPayload(21U);

    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);
    transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine), 1000U);
    transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine), 2000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::DuplicateSequence);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("A new announcement replaces the transfer already in flight",
          "[j1939][transport]")
{
    // One session per pair of addresses. The announcement that just arrived is
    // the one that means something; keeping the old one would also leak a
    // buffer on a bench where somebody is power-cycling an ECU.
    const std::vector<std::uint8_t> payload = countingPayload(21U);

    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);
    transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine), 1000U);
    transport.onFrame(bam(12U, 2U, kEngine), 2000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::Superseded);
    CHECK(transport.events()[0].packetsReceived == 1U);

    // The replacement is live and completes on its own.
    transport.clearEvents();
    const std::vector<std::uint8_t> second = countingPayload(12U);
    transport.onFrame(dataFrame(1U, slice(second, 0U), kEngine), 3000U);
    transport.onFrame(dataFrame(2U, slice(second, 1U), kEngine), 4000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].kind == J1939TransportEvent::Kind::MessageReceived);
    CHECK(transport.events()[0].data == second);
}

TEST_CASE("A declared size outside 9..1785 is a malformed announcement",
          "[j1939][transport]")
{
    // Eight bytes fit in a frame and do not use transport at all; 1786 is one
    // past what 255 packets of seven can carry. Neither is a message.
    J1939Transport transport;

    transport.onFrame(bam(8U, 2U, kEngine), 0U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::SizeOutOfRange);

    transport.clearEvents();
    transport.onFrame(bam(1786U, 255U, kEngine), 1000U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::SizeOutOfRange);

    CHECK(transport.openSessions() == 0U);

    // The boundaries themselves are accepted.
    transport.clearEvents();
    transport.onFrame(bam(9U, 2U, kEngine), 2000U);
    transport.onFrame(bam(1785U, 255U, kGearbox), 3000U);
    CHECK(transport.events().empty());
    CHECK(transport.openSessions() == 2U);
}

TEST_CASE("A packet count that disagrees with the size is refused",
          "[j1939][transport]")
{
    // One of the two numbers is wrong and there is no way to tell which.
    // Trusting the size would overrun a short transfer; trusting the count
    // would truncate a long one.
    J1939Transport transport;
    transport.onFrame(bam(21U, 2U, kEngine), 0U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::PacketCountMismatch);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("A sender that gives up ends the transfer", "[j1939][transport]")
{
    J1939Transport transport;
    transport.onFrame(requestToSend(21U, 3U, kEngine, kTester), 0U);
    transport.onFrame(dataFrame(1U, countingPayload(7U), kEngine, kTester), 1000U);

    // Byte 1 of a Conn Abort is the reason. connectionFrame puts `size` there,
    // so 3 is "a timeout occurred" - J1939-21 Table 6.
    transport.onFrame(connectionFrame(J1939TransportControl::Abort, kCarried, 3U, 0U,
                                      kEngine, kTester),
                      2000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::AbortedByPeer);
    CHECK(transport.events()[0].abortReason == 3U);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Every abort reason the standard defines is decoded",
          "[j1939][transport]")
{
    // J1939-21 MAY2022 Table 6. Six of these nine did not exist in the revision
    // this file was first written against, and were being reported as
    // "reserved" - wrong in the worst way available here, because four of them
    // name a specific defect in the transfer that just failed.
    CHECK(j1939AbortReasonText(1U).find("as many connections") != std::string_view::npos);
    CHECK(j1939AbortReasonText(2U).find("another task") != std::string_view::npos);
    CHECK(j1939AbortReasonText(3U).find("timeout") != std::string_view::npos);
    CHECK(j1939AbortReasonText(4U).find("CTS") != std::string_view::npos);
    CHECK(j1939AbortReasonText(5U).find("retransmit") != std::string_view::npos);
    CHECK(j1939AbortReasonText(6U).find("unexpected data packet") != std::string_view::npos);
    CHECK(j1939AbortReasonText(7U).find("bad sequence") != std::string_view::npos);
    CHECK(j1939AbortReasonText(8U).find("duplicate sequence") != std::string_view::npos);
    CHECK(j1939AbortReasonText(9U).find("1785") != std::string_view::npos);

    // 250 is not "reserved": the standard sets it aside for a reason that is
    // not in the table, which is a different statement from a code nobody
    // assigned.
    CHECK(j1939AbortReasonText(250U).find("no code for") != std::string_view::npos);

    CHECK(j1939AbortReasonText(10U).find("reserved") != std::string_view::npos);
    CHECK(j1939AbortReasonText(249U).find("reserved") != std::string_view::npos);

    CHECK(j1939AbortReasonText(251U).find("J1939-71") != std::string_view::npos);
    CHECK(j1939AbortReasonText(255U).find("J1939-71") != std::string_view::npos);

    // And the raw byte travels alongside the words, because most of the range
    // is reserved and a reserved code somebody really sends is worth seeing
    // exactly as it arrived.
    J1939Transport transport;
    transport.onFrame(requestToSend(21U, 3U, kEngine, kTester), 0U);
    transport.onFrame(connectionFrame(J1939TransportControl::Abort, kCarried, 200U, 0U,
                                      kEngine, kTester),
                      1000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].abortReason == 200U);
}

TEST_CASE("An abort says which end gave up", "[j1939][transport]")
{
    // A timeout reported by the receiver and a timeout reported by the sender
    // are the same word for two different faults, and the reason code cannot
    // tell them apart. Byte 3 can.
    //
    // connectionFrame puts `size` in bytes 2-3, so a size of 0x0103 leaves
    // reason = 3 (timeout) and role byte = 1 (the responder).
    J1939Transport transport;
    transport.onFrame(requestToSend(21U, 3U, kEngine, kTester), 0U);
    transport.onFrame(connectionFrame(J1939TransportControl::Abort, kCarried, 0x0103U, 0U,
                                      kEngine, kTester),
                      1000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].abortReason == 3U);
    CHECK(transport.events()[0].abortRole == J1939AbortRole::Responder);
    CHECK(j1939AbortRoleText(J1939AbortRole::Responder).find("receiver")
          != std::string_view::npos);
    CHECK(j1939AbortRoleText(J1939AbortRole::Originator).find("sender")
          != std::string_view::npos);

    // An extended abort reports no role at all. The layout of one belongs to
    // ISO 11783-3, which this project has not read, and reading J1939-21 byte
    // there would be a guess wearing the clothes of a fact.
    transport.clearEvents();
    transport.onFrame(extendedRts(1792U, kEngine, kTester), 2000U);

    CanFrame abort;
    abort.identifier =
        j1939Identifier(kPgnExtendedTransportConnection, kEngine, kTester, 7U);
    abort.format = CanFrameFormat::Extended;
    abort.length = 8;
    abort.dlc = 8;
    abort.data[0] = static_cast<std::uint8_t>(J1939ExtendedControl::Abort);
    abort.data[1] = 3U;
    abort.data[2] = 1U;

    transport.onFrame(abort, 3000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].abortReason == 3U);
    CHECK(transport.events()[0].abortRole == J1939AbortRole::Unspecified);
}

TEST_CASE("A negotiated transfer is given the handshake time before its first packet",
          "[j1939][transport]")
{
    // J1939-21 5.10.2(a): T1 is "a gap of more than T1 after receipt of the last
    // packet". Before the first packet there is nothing for it to count from -
    // the sender is waiting on a CTS from the other ECU, and T3 is what the
    // standard gives it for that. Applying T1 there reports a timeout on a
    // transfer that is still perfectly legal.
    constexpr std::uint64_t kT1 = J1939Transport::kPacketTimeoutNs;
    constexpr std::uint64_t kT3 = J1939Transport::kHandshakeTimeoutNs;

    J1939Transport transport;
    transport.onFrame(requestToSend(21U, 3U, kEngine, kTester), 0U);

    // Past T1 and still waiting. This is the case that used to be reported.
    transport.poll(kT1 + 1U);
    CHECK(transport.events().empty());
    CHECK(transport.openSessions() == 1U);

    // The first packet arrives late but in time, and from here T1 applies.
    transport.onFrame(dataFrame(1U, countingPayload(7U), kEngine, kTester), kT1 + 2U);
    CHECK(transport.events().empty());

    transport.poll(kT1 + 2U + kT1 + 1U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::Timeout);
    CHECK(transport.events()[0].packetsReceived == 1U);

    // And a transfer nobody ever answers does end, at T3.
    J1939Transport silent;
    silent.onFrame(requestToSend(21U, 3U, kEngine, kTester), 0U);

    silent.poll(kT3);
    CHECK(silent.events().empty());

    silent.poll(kT3 + 1U);
    REQUIRE(silent.events().size() == 1U);
    CHECK(silent.events()[0].packetsReceived == 0U);
}

TEST_CASE("A broadcast gets no extra patience, because it has no handshake",
          "[j1939][transport]")
{
    // Nobody answers a BAM - the packets follow the announcement directly - so
    // there is no negotiation to wait through and T1 applies from the start.
    constexpr std::uint64_t kT1 = J1939Transport::kPacketTimeoutNs;

    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);

    transport.poll(kT1);
    CHECK(transport.events().empty());

    transport.poll(kT1 + 1U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::Timeout);
    CHECK(transport.events()[0].broadcast);
}

TEST_CASE("A transfer that simply stops is ended by the clock",
          "[j1939][transport]")
{
    // A BAM that stops halfway leaves nothing to react to: no handshake, no
    // abort, just silence. Only T1 ends it.
    constexpr std::uint64_t kT1 = J1939Transport::kPacketTimeoutNs;

    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);
    transport.onFrame(dataFrame(1U, countingPayload(7U), kEngine), 0U);

    transport.poll(kT1);
    CHECK(transport.events().empty());
    CHECK(transport.openSessions() == 1U);

    transport.poll(kT1 + 1U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::Timeout);
    CHECK(transport.events()[0].packetsReceived == 1U);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Two ECUs can be transferring at the same time", "[j1939][transport]")
{
    // Sessions are keyed by the pair of addresses. A single-session
    // reassembler drops one of these on any busy machine, and the message it
    // drops is whichever arrived second - which changes run to run.
    const std::vector<std::uint8_t> first = countingPayload(12U);
    const std::vector<std::uint8_t> second = countingPayload(12U);

    J1939Transport transport;
    transport.onFrame(bam(12U, 2U, kEngine), 0U);
    transport.onFrame(bam(12U, 2U, kGearbox), 100U);
    CHECK(transport.openSessions() == 2U);

    // Interleaved, as they would really arrive.
    transport.onFrame(dataFrame(1U, slice(first, 0U), kEngine), 200U);
    transport.onFrame(dataFrame(1U, slice(second, 0U), kGearbox), 300U);
    transport.onFrame(dataFrame(2U, slice(first, 1U), kEngine), 400U);
    transport.onFrame(dataFrame(2U, slice(second, 1U), kGearbox), 500U);

    REQUIRE(transport.events().size() == 2U);
    CHECK(transport.events()[0].sourceAddress == kEngine);
    CHECK(transport.events()[1].sourceAddress == kGearbox);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("A negotiated transfer between two other ECUs reassembles too",
          "[j1939][transport]")
{
    // TorqueBus is a bystander here: the CTS and the end-of-message
    // acknowledgement belong to the two ECUs negotiating with each other, and
    // are read without being answered or counted as errors.
    const std::vector<std::uint8_t> payload = countingPayload(12U);

    J1939Transport transport;
    transport.onFrame(requestToSend(12U, 2U, kEngine, kTester), 0U);
    transport.onFrame(connectionFrame(J1939TransportControl::ClearToSend, kCarried, 0U, 0U,
                                      kTester, kEngine),
                      100U);
    transport.onFrame(dataFrame(1U, slice(payload, 0U), kEngine, kTester), 200U);
    transport.onFrame(dataFrame(2U, slice(payload, 1U), kEngine, kTester), 300U);
    transport.onFrame(connectionFrame(J1939TransportControl::EndOfMessageAck, kCarried, 12U,
                                      2U, kTester, kEngine),
                      400U);

    REQUIRE(transport.events().size() == 1U);

    const J1939TransportEvent& event = transport.events()[0];
    CHECK(event.kind == J1939TransportEvent::Kind::MessageReceived);
    CHECK_FALSE(event.broadcast);
    CHECK(event.destinationAddress == kTester);
    CHECK(event.data == payload);
}

TEST_CASE("The extended transport PGNs are the ones the standard gives",
          "[j1939][transport][etp]")
{
    // Stated by extension rather than only used. These were wrong once - read
    // out of memory instead of out of the standard - and because they were
    // wrong the reassembler never recognised an ETP frame at all, so every one
    // of them fell through to ordinary decoding. That is exactly the failure
    // this file exists to prevent, hidden behind a feature that looked done.
    CHECK(kPgnExtendedTransportConnection == 51200U);
    CHECK(kPgnExtendedTransportData == 50944U);

    // And the TP ones, which were right and stay checked.
    CHECK(kPgnTransportConnection == 60416U);
    CHECK(kPgnTransportData == 60160U);
    CHECK(static_cast<std::uint8_t>(J1939TransportControl::RequestToSend) == 16U);
    CHECK(static_cast<std::uint8_t>(J1939TransportControl::ClearToSend) == 17U);
    CHECK(static_cast<std::uint8_t>(J1939TransportControl::EndOfMessageAck) == 19U);
    CHECK(static_cast<std::uint8_t>(J1939TransportControl::BroadcastAnnounce) == 32U);

    CHECK(static_cast<std::uint8_t>(J1939ExtendedControl::RequestToSend) == 20U);
    CHECK(static_cast<std::uint8_t>(J1939ExtendedControl::ClearToSend) == 21U);
    CHECK(static_cast<std::uint8_t>(J1939ExtendedControl::DataPacketOffset) == 22U);
    CHECK(static_cast<std::uint8_t>(J1939ExtendedControl::EndOfMessageAck) == 23U);
}

TEST_CASE("An extended message reassembles across its offset windows",
          "[j1939][transport][etp]")
{
    // 1792 bytes is 256 packets - one more than a one-byte sequence number can
    // count, which is the entire reason ETP exists. The second window is where
    // a decoder that ignored the offset would start writing over the first.
    constexpr std::uint32_t kSize = 1792U;
    const std::vector<std::uint8_t> payload = countingPayload(kSize);

    J1939Transport transport;
    REQUIRE(transport.onFrame(extendedRts(kSize, kEngine, kTester), 0U));

    std::uint64_t clock = 1000U;
    std::uint32_t sent = 0U;

    while (sent < 256U) {
        const std::uint32_t window = std::min<std::uint32_t>(255U, 256U - sent);

        transport.onFrame(extendedOffset(window, sent, kEngine, kTester), clock++);

        for (std::uint32_t index = 0U; index < window; ++index) {
            transport.onFrame(extendedData(static_cast<std::uint8_t>(index + 1U),
                                           slice(payload, sent + index), kEngine, kTester),
                              clock++);
        }

        sent += window;
    }

    REQUIRE(transport.events().size() == 1U);

    const J1939TransportEvent& event = transport.events()[0];
    CHECK(event.kind == J1939TransportEvent::Kind::MessageReceived);
    CHECK(event.extended);
    CHECK_FALSE(event.broadcast);
    CHECK(event.pgn == kCarried);
    CHECK(event.data.size() == kSize);
    CHECK(event.data == payload);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("An extended sequence number restarts inside every window",
          "[j1939][transport][etp]")
{
    // Packet 256 arrives as sequence 1 with an offset of 255. Read without the
    // offset it is the first packet of the message, and the transfer quietly
    // overwrites its own beginning.
    constexpr std::uint32_t kSize = 1792U;
    const std::vector<std::uint8_t> payload = countingPayload(kSize);

    J1939Transport transport;
    transport.onFrame(extendedRts(kSize, kEngine, kTester), 0U);
    transport.onFrame(extendedOffset(255U, 0U, kEngine, kTester), 1U);

    for (std::uint32_t index = 0U; index < 255U; ++index) {
        transport.onFrame(extendedData(static_cast<std::uint8_t>(index + 1U),
                                       slice(payload, index), kEngine, kTester),
                          2U + index);
    }

    CHECK(transport.events().empty());

    transport.onFrame(extendedOffset(1U, 255U, kEngine, kTester), 300U);
    transport.onFrame(extendedData(1U, slice(payload, 255U), kEngine, kTester), 301U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].data == payload);
}

TEST_CASE("An offset that jumps forward is a hole, and is refused",
          "[j1939][transport][etp]")
{
    // The sender skipping a stretch it believes was delivered. Accepting it
    // would leave a gap that reassembles into a message nothing downstream can
    // tell apart from a measured one.
    constexpr std::uint32_t kSize = 1792U;
    const std::vector<std::uint8_t> payload = countingPayload(kSize);

    J1939Transport transport;
    transport.onFrame(extendedRts(kSize, kEngine, kTester), 0U);
    transport.onFrame(extendedOffset(255U, 0U, kEngine, kTester), 1U);
    transport.onFrame(extendedData(1U, slice(payload, 0U), kEngine, kTester), 2U);

    // The message is one packet in; an offset of 100 skips 99 of them.
    transport.onFrame(extendedOffset(100U, 100U, kEngine, kTester), 3U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::SequenceGap);
    CHECK(transport.events()[0].packetsReceived == 1U);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Extended data before any offset has nowhere to go",
          "[j1939][transport][etp]")
{
    // Assuming the first window would put these bytes at the start of the
    // message and silently misplace every window after it.
    constexpr std::uint32_t kSize = 1792U;

    J1939Transport transport;
    transport.onFrame(extendedRts(kSize, kEngine, kTester), 0U);
    transport.onFrame(extendedData(1U, countingPayload(7U), kEngine, kTester), 1U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::MissingPacketOffset);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Neither transport takes in the other laundry",
          "[j1939][transport][etp]")
{
    J1939Transport transport;

    // Anything the first protocol can carry belongs to it. An extended
    // announcement below the line is a sender with the two confused.
    transport.onFrame(extendedRts(1785U, kEngine, kTester), 0U);
    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::SizeOutOfRange);
    CHECK(transport.events()[0].extended);

    // And the first size above it is accepted.
    transport.clearEvents();
    transport.onFrame(extendedRts(1786U, kEngine, kTester), 1U);
    CHECK(transport.events().empty());
    CHECK(transport.openSessions() == 1U);
}

TEST_CASE("A size no ECU means is refused rather than allocated",
          "[j1939][transport][etp]")
{
    // The protocol allows 117,440,505 bytes. That is a number every fuzzer
    // tries and no ECU on a bench means, and honouring it would turn a
    // four-byte field into an allocation somebody else chose.
    J1939Transport transport;

    transport.onFrame(
        extendedRts(static_cast<std::uint32_t>(J1939Transport::kMaximumExtendedMessage) + 1U,
                    kEngine, kTester),
        0U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::SizeOutOfRange);
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Data with no announcement is left alone", "[j1939][transport]")
{
    // A transfer that began before the measurement did. There is nothing to
    // reassemble it into, and guessing a length from the packets that happen to
    // arrive would produce a message of unknown size.
    J1939Transport transport;
    CHECK(transport.onFrame(dataFrame(5U, countingPayload(7U), kEngine), 0U));

    CHECK(transport.events().empty());
    CHECK(transport.openSessions() == 0U);
}

TEST_CASE("Only transport frames are claimed", "[j1939][transport]")
{
    // The caller uses this answer to keep TP.DT out of ordinary decoding: seven
    // bytes of somebody else payload under a sequence number will decode as
    // signals, and every one of them will be wrong.
    J1939Transport transport;

    CanFrame ordinary;
    ordinary.identifier = j1939Identifier(kPgnDm1, kEngine);
    ordinary.format = CanFrameFormat::Extended;
    ordinary.length = 8;
    ordinary.dlc = 8;
    CHECK_FALSE(transport.onFrame(ordinary, 0U));

    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8;
    standard.dlc = 8;
    CHECK_FALSE(transport.onFrame(standard, 0U));

    CHECK(transport.onFrame(bam(12U, 2U, kEngine), 0U));
    CHECK(transport.onFrame(dataFrame(1U, countingPayload(7U), kEngine), 0U));
}

TEST_CASE("Stopping a measurement drops transfers without reporting them",
          "[j1939][transport]")
{
    // reset() is for a measurement ending, which is not a fault of the bus. A
    // half-finished transfer at Stop is not news.
    J1939Transport transport;
    transport.onFrame(bam(21U, 3U, kEngine), 0U);
    transport.onFrame(dataFrame(1U, countingPayload(7U), kEngine), 1000U);

    transport.reset();

    CHECK(transport.openSessions() == 0U);
    CHECK(transport.events().empty());
}
