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
    transport.onFrame(connectionFrame(J1939TransportControl::Abort, kCarried, 0U, 0U,
                                      kEngine, kTester),
                      2000U);

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::AbortedByPeer);
    CHECK(transport.openSessions() == 0U);
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

TEST_CASE("Extended transport is declined out loud, not ignored",
          "[j1939][transport]")
{
    // A message nobody ever mentions looks like a bus that never carried it,
    // and somebody then goes looking for a wiring fault that is not there.
    //
    // The two numbers are stated rather than only used, so that checking them
    // against J1939-21 is reading one line instead of tracing a constant.
    CHECK(kPgnExtendedTransportConnection == 52736U);
    CHECK(kPgnExtendedTransportData == 51712U);

    CanFrame frame;
    frame.identifier = j1939Identifier(kPgnExtendedTransportConnection, kEngine, kTester, 7U);
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;
    frame.data[0] = 20U;
    frame.data[5] = static_cast<std::uint8_t>(kCarried & 0xFFU);
    frame.data[6] = static_cast<std::uint8_t>((kCarried >> 8U) & 0xFFU);
    frame.data[7] = 0U;

    J1939Transport transport;
    CHECK(transport.onFrame(frame, 0U));

    REQUIRE(transport.events().size() == 1U);
    CHECK(transport.events()[0].error == J1939TransportError::Unsupported);
    CHECK(transport.events()[0].pgn == kCarried);
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
