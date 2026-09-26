// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// ISO 15765-2 is a protocol whose failures are silences. A wrong sequence
// number, a flow control ignored, a padding byte missing - none of them produce
// a wrong answer, they produce an ECU that does not reply, which is the hardest
// symptom there is to work backwards from. So most of these cases check bytes
// on the wire rather than the shape of the API.
//
// Time is a parameter here, not a wait: a thousand-millisecond timeout is
// tested by saying it is now a second later.

#include "core/isotp/IsoTpConnection.h"
#include "core/isotp/IsoTpTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint64_t kMillisecond = 1'000'000ULL;

[[nodiscard]] IsoTpConnection tester(IsoTpConfig config = {})
{
    return IsoTpConnection{IsoTpAddress::obd(), config};
}

/// A frame arriving from the ECU: on the connection's receive identifier.
[[nodiscard]] CanFrame incoming(std::initializer_list<std::uint8_t> bytes,
                                std::uint32_t identifier = 0x7E8)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.length = static_cast<std::uint8_t>(bytes.size());
    frame.dlc = frame.length;

    std::size_t index = 0;
    for (const std::uint8_t byte : bytes) {
        frame.data[index++] = byte;
    }

    return frame;
}

/// The payload bytes of a queued frame, without padding.
[[nodiscard]] std::vector<std::uint8_t> bytes(const CanFrame& frame, std::size_t count)
{
    return std::vector<std::uint8_t>{frame.data.begin(),
                                     frame.data.begin() + static_cast<std::ptrdiff_t>(count)};
}

[[nodiscard]] std::vector<std::uint8_t> ramp(std::size_t length)
{
    std::vector<std::uint8_t> result(length);
    for (std::size_t index = 0; index < length; ++index) {
        result[index] = static_cast<std::uint8_t>(index & 0xFFU);
    }
    return result;
}

} // namespace

TEST(IsoTpTests, AShortMessageGoesOutInOneFramePadded)
{
    IsoTpConnection connection = tester();

    const std::vector<std::uint8_t> request{0x22, 0xF1, 0x90};
    ASSERT_TRUE(connection.send(request, 0).succeeded());

    ASSERT_TRUE(connection.pendingFrames().size() == 1);

    const CanFrame& frame = connection.pendingFrames().front();

    // 0x03 is "single frame, three bytes". The rest is padding, and it is there
    // because a great many ECUs ignore an unpadded frame - which is legal of
    // them and is nevertheless a morning lost.
    EXPECT_TRUE((bytes(frame, 4) == std::vector<std::uint8_t>{0x03, 0x22, 0xF1, 0x90}));
    EXPECT_TRUE(frame.length == 8);
    EXPECT_TRUE(frame.data[7] == 0xCC);
    EXPECT_TRUE(frame.identifier == 0x7E0);

    // No handshake to wait for.
    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::SendComplete);
    EXPECT_FALSE(connection.isSending());
}

TEST(IsoTpTests, PaddingCanBeTurnedOffForABusThatDoesNotWantIt)
{
    IsoTpConfig config;
    config.padding = false;

    IsoTpConnection connection = tester(config);
    ASSERT_TRUE(connection.send(std::vector<std::uint8_t>{0x22, 0xF1, 0x90}, 0).succeeded());

    EXPECT_TRUE(connection.pendingFrames().front().length == 4);
}

TEST(IsoTpTests, ALongMessageIsAFirstFrameAndThenABlock)
{
    IsoTpConnection connection = tester();

    const std::vector<std::uint8_t> payload = ramp(20);
    ASSERT_TRUE(connection.send(payload, 0).succeeded());

    // The first frame carries the total length and six bytes of payload; then
    // the sender stops and waits, which is the entire handshake.
    ASSERT_TRUE(connection.pendingFrames().size() == 1);
    EXPECT_TRUE(
        (bytes(connection.pendingFrames().front(), 2) == std::vector<std::uint8_t>{0x10, 0x14}));
    EXPECT_TRUE(connection.isSending());

    connection.clearPendingFrames();

    // The ECU says: send everything, no gap.
    EXPECT_TRUE(connection.onFrame(incoming({0x30, 0x00, 0x00}), kMillisecond));
    connection.poll(kMillisecond);

    // 20 bytes, 6 in the first frame, 7 per consecutive frame: two more.
    ASSERT_TRUE(connection.pendingFrames().size() == 2);
    EXPECT_TRUE(connection.pendingFrames()[0].data[0] == 0x21);
    EXPECT_TRUE(connection.pendingFrames()[1].data[0] == 0x22);

    // And the payload came out in order, across the join.
    EXPECT_TRUE(connection.pendingFrames()[0].data[1] == 6);
    EXPECT_TRUE(connection.pendingFrames()[1].data[1] == 13);

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::SendComplete);
}

TEST(IsoTpTests, TheBlockSizeTheECUAsksForIsHonoured)
{
    // This is the part of the handshake a tester is most tempted to skip, and
    // skipping it is what makes a tester that works on the bench stop working
    // in the vehicle: the ECU has one buffer and an interrupt to get back from.
    IsoTpConnection connection = tester();

    ASSERT_TRUE(connection.send(ramp(40), 0).succeeded());
    connection.clearPendingFrames();

    // Two frames at a time.
    EXPECT_TRUE(connection.onFrame(incoming({0x30, 0x02, 0x00}), 0));
    connection.poll(0);

    EXPECT_TRUE(connection.pendingFrames().size() == 2);
    connection.clearPendingFrames();

    // And it stops there, however long we wait, until it is asked again.
    connection.poll(10 * kMillisecond);
    EXPECT_TRUE(connection.pendingFrames().empty());

    EXPECT_TRUE(connection.onFrame(incoming({0x30, 0x02, 0x00}), 10 * kMillisecond));
    connection.poll(10 * kMillisecond);
    EXPECT_TRUE(connection.pendingFrames().size() == 2);
}

TEST(IsoTpTests, STminPutsAGapBetweenConsecutiveFrames)
{
    IsoTpConnection connection = tester();

    ASSERT_TRUE(connection.send(ramp(30), 0).succeeded());
    connection.clearPendingFrames();

    // 20 ms between frames - a slow ECU, and a legal one.
    EXPECT_TRUE(connection.onFrame(incoming({0x30, 0x00, 0x14}), 0));
    connection.poll(0);

    // One frame now, and no more until the gap has passed.
    EXPECT_TRUE(connection.pendingFrames().size() == 1);

    connection.poll(19 * kMillisecond);
    EXPECT_TRUE(connection.pendingFrames().size() == 1);

    connection.poll(20 * kMillisecond);
    EXPECT_TRUE(connection.pendingFrames().size() == 2);
}

TEST(IsoTpTests, TheMicrosecondHalfOfTheSTminEncodingIsNotMilliseconds)
{
    // 0xF1..0xF9 are 100..900 microseconds, and reading them as milliseconds
    // makes a transfer a thousand times slower than the ECU asked for - which
    // looks like a slow ECU rather than like a bug in the tester.
    EXPECT_TRUE(separationMicroseconds(0x00) == 0);
    EXPECT_TRUE(separationMicroseconds(0x14) == 20'000);
    EXPECT_TRUE(separationMicroseconds(0x7F) == 127'000);
    EXPECT_TRUE(separationMicroseconds(0xF1) == 100);
    EXPECT_TRUE(separationMicroseconds(0xF9) == 900);

    // Reserved values mean the other end speaks a dialect this one does not
    // know. The standard says to use the slowest legal separation rather than
    // to guess.
    EXPECT_TRUE(separationMicroseconds(0x80) == 127'000);
    EXPECT_TRUE(separationMicroseconds(0xFA) == 127'000);
}

TEST(IsoTpTests, AFlowControlThatNeverComesIsReportedNotWaitedOnForever)
{
    IsoTpConnection connection = tester();

    ASSERT_TRUE(connection.send(ramp(20), 0).succeeded());

    connection.poll(999 * kMillisecond);
    EXPECT_TRUE(connection.events().empty());

    connection.poll(1000 * kMillisecond);

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::SendFailed);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::FlowControlTimeout);
    EXPECT_FALSE(connection.isSending());
}

TEST(IsoTpTests, WAITIsHonouredAndThenItIsNot)
{
    IsoTpConfig config;
    config.maximumWaitFrames = 3;

    IsoTpConnection connection = tester(config);
    ASSERT_TRUE(connection.send(ramp(20), 0).succeeded());

    // Each WAIT restarts the clock, so a tester that is being asked to wait
    // does not also time out.
    for (int index = 1; index <= 3; ++index) {
        EXPECT_TRUE(connection.onFrame(incoming({0x31, 0x00, 0x00}),
                                       static_cast<std::uint64_t>(index) * 900 * kMillisecond));
        EXPECT_TRUE(connection.events().empty());
    }

    // But an ECU that says WAIT forever has stopped answering, and waiting
    // forever is not a diagnosis.
    EXPECT_TRUE(connection.onFrame(incoming({0x31, 0x00, 0x00}), 4000 * kMillisecond));

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::TooManyWaitFrames);
}

TEST(IsoTpTests, AnOverflowAnswerStopsTheTransferRatherThanRetryingIt)
{
    IsoTpConnection connection = tester();
    ASSERT_TRUE(connection.send(ramp(20), 0).succeeded());

    EXPECT_TRUE(connection.onFrame(incoming({0x32, 0x00, 0x00}), 0));

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::Overflow);
}

TEST(IsoTpTests, ASingleFrameArrivingIsAMessage)
{
    IsoTpConnection connection = tester();

    EXPECT_TRUE(connection.onFrame(incoming({0x04, 0x62, 0xF1, 0x90, 0x41, 0xCC, 0xCC, 0xCC}), 0));

    ASSERT_TRUE(connection.events().size() == 1);

    const IsoTpEvent& event = connection.events().front();
    EXPECT_TRUE(event.kind == IsoTpEvent::Kind::MessageReceived);

    // The padding is not part of the message - the length byte is what says so.
    EXPECT_TRUE((event.data == std::vector<std::uint8_t>{0x62, 0xF1, 0x90, 0x41}));
}

TEST(IsoTpTests, ALongMessageIsReassembledAndAskedForABlockAtATime)
{
    IsoTpConfig config;
    config.blockSize = 2;
    config.separationTime = 0x0A;

    IsoTpConnection connection = tester(config);

    // 14 bytes: 6 in the first frame, then two consecutive frames.
    EXPECT_TRUE(connection.onFrame(incoming({0x10, 0x0E, 1, 2, 3, 4, 5, 6}), 0));

    // The flow control this end sends carries *its* terms, not the sender's.
    ASSERT_TRUE(connection.pendingFrames().size() == 1);
    EXPECT_TRUE((bytes(connection.pendingFrames().front(), 3)
                 == std::vector<std::uint8_t>{0x30, 0x02, 0x0A}));

    EXPECT_TRUE(connection.onFrame(incoming({0x21, 7, 8, 9, 10, 11, 12, 13}), kMillisecond));
    EXPECT_TRUE(connection.onFrame(incoming({0x22, 14}), 2 * kMillisecond));

    ASSERT_TRUE(connection.events().size() == 1);

    const IsoTpEvent& event = connection.events().front();
    EXPECT_TRUE(event.kind == IsoTpEvent::Kind::MessageReceived);
    EXPECT_TRUE(
        (event.data == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}));
}

TEST(IsoTpTests, AConsecutiveFrameOutOfOrderAbandonsTheMessage)
{
    // Almost always two senders on one identifier, which is a wiring fault
    // rather than a transient - and the reassembled message would be nonsense.
    IsoTpConnection connection = tester();

    EXPECT_TRUE(connection.onFrame(incoming({0x10, 0x0E, 1, 2, 3, 4, 5, 6}), 0));
    connection.clearPendingFrames();

    EXPECT_TRUE(connection.onFrame(incoming({0x23, 7, 8, 9, 10, 11, 12, 13}), 0));

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::ReceiveFailed);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::SequenceError);
    EXPECT_FALSE(connection.isReceiving());
}

TEST(IsoTpTests, AMessageThatStopsHalfWayIsDiscardedNotDeliveredShort)
{
    IsoTpConnection connection = tester();

    EXPECT_TRUE(connection.onFrame(incoming({0x10, 0x14, 1, 2, 3, 4, 5, 6}), 0));
    connection.clearPendingFrames();

    connection.poll(999 * kMillisecond);
    EXPECT_TRUE(connection.events().empty());

    connection.poll(1000 * kMillisecond);

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::ConsecutiveTimeout);

    // And nothing was handed up. Half a diagnostic response decoded as a whole
    // one is worse than none, because the reader cannot tell.
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::ReceiveFailed);
    EXPECT_TRUE(connection.events().front().data.empty());
}

TEST(IsoTpTests, SequenceNumbersWrapAtSixteen)
{
    // The obvious place to be off by one, and the symptom is a message that
    // fails only when it is longer than about a hundred bytes.
    IsoTpConnection connection = tester();

    const std::vector<std::uint8_t> payload = ramp(200);
    ASSERT_TRUE(connection.send(payload, 0).succeeded());
    connection.clearPendingFrames();

    EXPECT_TRUE(connection.onFrame(incoming({0x30, 0x00, 0x00}), 0));
    connection.poll(0);

    const std::span<const CanFrame> frames = connection.pendingFrames();
    ASSERT_TRUE(frames.size() == 28); // (200 - 6) / 7, rounded up.

    EXPECT_TRUE(frames[0].data[0] == 0x21);
    EXPECT_TRUE(frames[14].data[0] == 0x2F);
    EXPECT_TRUE(frames[15].data[0] == 0x20); // Wrapped.
    EXPECT_TRUE(frames[16].data[0] == 0x21);

    // And every byte of the payload came out exactly once, in order.
    std::vector<std::uint8_t> reassembled{frames.begin()->data.begin() + 0,
                                          frames.begin()->data.begin() + 0};
    reassembled.clear();

    for (const CanFrame& frame : frames) {
        const std::size_t count = std::min<std::size_t>(7, frame.length - 1);
        for (std::size_t index = 0; index < count; ++index) {
            reassembled.push_back(frame.data[1 + index]);
        }
    }

    reassembled.resize(payload.size() - 6);

    EXPECT_TRUE((reassembled == std::vector<std::uint8_t>{payload.begin() + 6, payload.end()}));
}

TEST(IsoTpTests, ExtendedAddressingSpendsItsFirstByteOnTheAddress)
{
    IsoTpAddress address = IsoTpAddress::obd();
    address.addressing = IsoTpAddressing::Extended;
    address.transmitExtension = 0xF1;
    address.receiveExtension = 0x10;

    IsoTpConnection connection{address, IsoTpConfig{}};

    ASSERT_TRUE(connection.send(std::vector<std::uint8_t>{0x22, 0xF1, 0x90}, 0).succeeded());

    EXPECT_TRUE((bytes(connection.pendingFrames().front(), 5)
                 == std::vector<std::uint8_t>{0xF1, 0x03, 0x22, 0xF1, 0x90}));

    // The send above completed, and its event is not what the rest of this
    // case is about.
    connection.clearEvents();

    // And a frame on our identifier addressed to somebody else is not ours.
    EXPECT_FALSE(connection.onFrame(incoming({0x99, 0x02, 0x50, 0x01}), 0));
    EXPECT_TRUE(connection.events().empty());

    EXPECT_TRUE(connection.onFrame(incoming({0x10, 0x02, 0x50, 0x01}), 0));
    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE((connection.events().front().data == std::vector<std::uint8_t>{0x50, 0x01}));
}

TEST(IsoTpTests, NormalFixedAddressingBuildsTheIdentifiersTheStandardNames)
{
    // 0x18DA<target><source> out and the same with the addresses swapped back.
    // Heavy vehicles run on this, and getting the two halves the wrong way
    // round produces a tester that talks and is never answered.
    const IsoTpAddress physical = IsoTpAddress::normalFixed(0xF9, 0x00);

    EXPECT_TRUE(physical.transmitId == 0x18DA00F9);
    EXPECT_TRUE(physical.receiveId == 0x18DAF900);
    EXPECT_TRUE(physical.format == CanFrameFormat::Extended);

    const IsoTpAddress functional = IsoTpAddress::normalFixedFunctional(0xF9, 0x33);

    EXPECT_TRUE(functional.transmitId == 0x18DB33F9);

    // The answer to a functional request comes back physically addressed: each
    // ECU that answers does so as itself, which is the point of asking that way.
    EXPECT_TRUE(functional.receiveId == 0x18DAF933);
}

TEST(IsoTpTests, CANFDCarriesALongSingleFrameBehindAnEscape)
{
    IsoTpConfig config;
    config.canFd = true;

    IsoTpConnection connection = tester(config);

    ASSERT_TRUE(connection.send(ramp(40), 0).succeeded());

    ASSERT_TRUE(connection.pendingFrames().size() == 1);

    const CanFrame& frame = connection.pendingFrames().front();

    // A zero length in the nibble, then the real one - rather than widening a
    // nibble that every existing implementation reads as four bits.
    EXPECT_TRUE(frame.data[0] == 0x00);
    EXPECT_TRUE(frame.data[1] == 40);
    EXPECT_TRUE(frame.fd);

    // 42 bytes used, and FD has no 42-byte frame: it goes in a 48-byte one.
    EXPECT_TRUE(frame.length == 48);
    EXPECT_TRUE(frame.data[47] == 0xCC);

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().kind == IsoTpEvent::Kind::SendComplete);
}

TEST(IsoTpTests, ACANFDFrameIsPaddedToALengthThatExists)
{
    // 12, 16, 20, 24, 32, 48, 64 and nothing in between. A frame of 34 bytes
    // travels in one of 48 whether anybody likes it or not.
    EXPECT_TRUE(fdFrameLength(1) == 8);
    EXPECT_TRUE(fdFrameLength(8) == 8);
    EXPECT_TRUE(fdFrameLength(9) == 12);
    EXPECT_TRUE(fdFrameLength(34) == 48);
    EXPECT_TRUE(fdFrameLength(48) == 48);
    EXPECT_TRUE(fdFrameLength(64) == 64);
}

TEST(IsoTpTests, AMessageTooLongForA12BitLengthUsesTheEscapeForm)
{
    IsoTpConnection connection = tester();

    const std::vector<std::uint8_t> payload = ramp(5000);
    ASSERT_TRUE(connection.send(payload, 0).succeeded());

    const CanFrame& frame = connection.pendingFrames().front();

    // 0x10 0x00 says "the length is in the next four bytes".
    EXPECT_TRUE((bytes(frame, 6) == std::vector<std::uint8_t>{0x10, 0x00, 0x00, 0x00, 0x13, 0x88}));
}

TEST(IsoTpTests, AnEnormousLengthFieldIsRefusedRatherThanAllocated)
{
    // Four gigabytes is what the escape field allows and what a fuzzer sends.
    // Answered with an overflow flow control, because the sender is entitled to
    // know why nothing is happening.
    IsoTpConnection connection = tester();

    EXPECT_TRUE(connection.onFrame(incoming({0x10, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02}), 0));

    ASSERT_TRUE(connection.pendingFrames().size() == 1);
    EXPECT_TRUE(connection.pendingFrames().front().data[0] == 0x32);

    ASSERT_TRUE(connection.events().size() == 1);
    EXPECT_TRUE(connection.events().front().error == IsoTpError::TooLong);
}

TEST(IsoTpTests, FramesBelongingToSomebodyElseAreNotOurs)
{
    IsoTpConnection connection = tester();

    EXPECT_FALSE(connection.onFrame(incoming({0x03, 0x22, 0xF1, 0x90}, 0x123), 0));
    EXPECT_TRUE(connection.events().empty());

    // Including our own transmit identifier: a tester that treats its own
    // echoes as answers reassembles its own request.
    EXPECT_FALSE(connection.onFrame(incoming({0x03, 0x22, 0xF1, 0x90}, 0x7E0), 0));
    EXPECT_TRUE(connection.events().empty());
}

TEST(IsoTpTests, ASecondSendWhileOneIsInFlightIsRefused)
{
    IsoTpConnection connection = tester();

    ASSERT_TRUE(connection.send(ramp(20), 0).succeeded());
    EXPECT_TRUE(connection.send(ramp(20), 0).failed());
}

TEST(IsoTpTests, AConsecutiveFrameWithNothingStartedIsIgnored)
{
    // What a tester sees when it joins a bus half way through somebody else's
    // transfer. Reporting it would fire on every such join.
    IsoTpConnection connection = tester();

    EXPECT_TRUE(connection.onFrame(incoming({0x21, 1, 2, 3, 4, 5, 6, 7}), 0));
    EXPECT_TRUE(connection.events().empty());
}
