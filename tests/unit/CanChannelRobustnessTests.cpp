// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What reaches the pipeline from a driver, and what leaves it for one.
//
// A backend is a thin layer over a vendor SDK, and everything downstream of the channel
// trusts what the channel hands it: a frame's length is how many bytes of its payload
// array to read. If a driver fills that byte wrong - a status word read into the wrong
// field, a malformed frame an adapter did not reject - the length can be 255 on an
// array of 64, and every reader of the payload then reads past the end of it.
//
// These tests stand in for that driver. A backend that delivers whatever the test says,
// and records whatever the channel sends it, shows what the channel does at the boundary:
// it makes the claim true on the way in, and refuses to pass a claim that cannot be true
// on the way out.

#include "core/can/CanChannel.h"
#include "core/trace/TraceStore.h"
#include "drivers/api/ICanBackend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// A backend that delivers what it is told to and keeps what it is sent.
class ScriptedBackend final : public ICanBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "scripted"; }
    [[nodiscard]] bool isAvailable() const noexcept override { return true; }
    [[nodiscard]] CanDeviceInfoList enumerate() override { return {}; }

    [[nodiscard]] Result open(const CanChannelConfig&) override
    {
        m_open = true;
        return Result::ok();
    }

    [[nodiscard]] Result start() override { return Result::ok(); }
    void stop() override { }
    void close() override { m_open = false; }
    [[nodiscard]] bool isOpen() const noexcept override { return m_open; }

    [[nodiscard]] Result transmit(const CanFrame& frame) override
    {
        sent.push_back(frame);
        return Result::ok();
    }

    [[nodiscard]] CanBusStatus status() const override { return {}; }
    [[nodiscard]] CanCapabilities capabilities() const override { return {}; }

    void setFrameHandler(FrameHandler handler) override { m_handler = std::move(handler); }
    void setStatusHandler(StatusHandler) override { }

    /// The driver delivers.
    void deliver(std::span<const CanFrame> frames)
    {
        if (m_handler) {
            m_handler(frames);
        }
    }

    std::vector<CanFrame> sent;

private:
    bool m_open{false};
    FrameHandler m_handler;
};

struct Rig final {
    Rig()
    {
        auto scripted = std::make_unique<ScriptedBackend>();
        backend = scripted.get();

        CanChannelConfig config;
        config.deviceHandle = "scripted:0";
        config.timing.bitrate = 500'000;
        config.canFdEnabled = true;

        channel = std::make_unique<CanChannel>(std::uint8_t{0}, std::move(scripted), config);
        EXPECT_TRUE(channel->start().succeeded());
    }

    ScriptedBackend* backend{nullptr};
    std::unique_ptr<CanChannel> channel;
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::uint8_t length)
{
    CanFrame result;
    result.identifier = identifier;
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    result.length = length;
    result.dlc = length;
    return result;
}

} // namespace

TEST(CanChannelRobustnessTests, AReceivedFrameThatClaimsMoreBytesThanItCanHoldIsHeldToTheArray)
{
    Rig rig;

    CanFrame liar = frame(0x123, 255);
    liar.fd = true;
    liar.dlc = 15;
    liar.data.fill(0xAB);

    const std::array<CanFrame, 2> delivered{frame(0x100, 8), liar};
    rig.backend->deliver(delivered);

    const std::size_t passed = rig.channel->beginPass(16);
    ASSERT_EQ(passed, 2U);

    const std::span<const CanFrame> frames = rig.channel->passFrames();
    EXPECT_EQ(frames[0].length, 8);
    EXPECT_EQ(frames[1].length, 64) << "a length past the payload array reached the pipeline";
    EXPECT_EQ(frames[1].payloadSize(), 64U);
}

TEST(CanChannelRobustnessTests, APayloadSizeIsNeverMoreThanThePayloadHolds)
{
    CanFrame frame;

    for (const std::uint8_t length : std::array<std::uint8_t, 8>{0, 1, 8, 12, 64, 65, 128, 255}) {
        frame.length = length;
        EXPECT_LE(frame.payloadSize(), kMaxCanPayload) << int{length};
        EXPECT_EQ(frame.payloadSize(), std::min<std::size_t>(length, 64)) << int{length};
    }
}

TEST(CanChannelRobustnessTests, AFrameThatCannotBeOnABusNeverReachesTheDriver)
{
    Rig rig;

    CanFrame wideNumber = frame(0x100, 8);
    wideNumber.identifier = 0x1000; // 29 bits of number in an 11-bit frame
    EXPECT_TRUE(rig.channel->transmit(wideNumber).failed());

    CanFrame beyondTheArray = frame(0x100, 8);
    beyondTheArray.fd = true;
    beyondTheArray.length = 200;
    EXPECT_TRUE(rig.channel->transmit(beyondTheArray).failed());

    CanFrame noSuchCode = frame(0x100, 8);
    noSuchCode.dlc = 16;
    EXPECT_TRUE(rig.channel->transmit(noSuchCode).failed());

    CanFrame twelveClassic = frame(0x100, 12);
    EXPECT_TRUE(rig.channel->transmit(twelveClassic).failed());

    EXPECT_TRUE(rig.backend->sent.empty()) << "the driver was handed a frame that cannot exist";

    // And a frame that can exist still goes through.
    EXPECT_TRUE(rig.channel->transmit(frame(0x100, 8)).succeeded());

    CanFrame fd = frame(0x18FEEE00, 24);
    fd.fd = true;
    fd.dlc = dlcFromPayloadLength(fd.length, true); // 24 bytes is the code 12
    EXPECT_TRUE(rig.channel->transmit(fd).succeeded());

    EXPECT_EQ(rig.backend->sent.size(), 2U);
}

TEST(CanChannelRobustnessTests, ARemoteFrameAndACodeAboveEightOnAClassicFrameAreStillSent)
{
    // The checks are about what cannot exist. A remote request has a code and no payload, and
    // a code of 9 to 15 on a classic frame means eight bytes - both are legal.
    Rig rig;

    CanFrame remote = frame(0x200, 0);
    remote.rtr = true;
    remote.dlc = 4;
    EXPECT_TRUE(rig.channel->transmit(remote).succeeded());

    CanFrame nine = frame(0x201, 8);
    nine.dlc = 9;
    EXPECT_TRUE(rig.channel->transmit(nine).succeeded());
}

TEST(CanChannelRobustnessTests, AFrameKeptInTheTraceIsKeptAsTheArrayItIs)
{
    // What the trace keeps is read by the trace view, the signal tree, the exporters and the
    // canvas cards, each of which indexes the payload with the length. A frame a plugin's block
    // built with a length past the array is held to it on the way in.
    TraceStore store{16};

    CanFrame liar = frame(0x123, 255);
    liar.data.fill(0x5A);

    const std::array<CanFrame, 2> frames{frame(0x100, 8), liar};
    store.append(frames);

    ASSERT_EQ(store.size(), 2U);
    EXPECT_EQ(store.row(0).frame.length, 8);
    EXPECT_EQ(store.row(1).frame.length, 64);
    EXPECT_EQ(store.identifiers().back().lastFrame.length, 64);
}
