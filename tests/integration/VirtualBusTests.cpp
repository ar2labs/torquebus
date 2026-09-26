// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The rehearsal for the first real hardware test.
//
// PLAN.md section 30 defines the v0.3 acceptance test as
// "Kvaser Virtual 0 -> TorqueBus -> Kvaser Virtual 1". These tests are that
// same shape, driven by the built-in virtual backend, so the assertions are
// already written and green before any vendor SDK is involved.

#include "core/can/CanFrame.h"
#include "drivers/api/CanBackendRegistry.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// Collects everything a backend delivers, the way the CAN engine's queue will.
class FrameCollector final {
public:
    [[nodiscard]] FrameHandler handler()
    {
        return [this](std::span<const CanFrame> batch) {
            m_frames.insert(m_frames.end(), batch.begin(), batch.end());
        };
    }

    [[nodiscard]] const std::vector<CanFrame>& frames() const noexcept { return m_frames; }
    [[nodiscard]] std::size_t count() const noexcept { return m_frames.size(); }

    /// Waits until at least `expected` frames have arrived, or gives up.
    ///
    /// The virtual bus posts into an inbox and a receive thread delivers from
    /// it, so a frame is not there the instant transmit() returns. Every count
    /// check in this file used to run immediately after a transmit and lose
    /// that race. Polling rather than sleeping a fixed time keeps a passing
    /// test fast and an honest failure bounded.
    [[nodiscard]] bool waitFor(std::size_t expected,
                               std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            if (count() >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        return count() >= expected;
    }

    [[nodiscard]] std::size_t countByDirection(CanDirection direction) const
    {
        return static_cast<std::size_t>(
            std::count_if(m_frames.begin(), m_frames.end(), [direction](const CanFrame& frame) {
                return frame.direction == direction;
            }));
    }

    void clear() { m_frames.clear(); }

private:
    std::vector<CanFrame> m_frames;
};

CanChannelConfig configFor(const std::string& handle, std::uint8_t applicationChannel)
{
    CanChannelConfig config;
    config.deviceHandle = handle;
    config.applicationChannel = applicationChannel;
    config.timing.bitrate = 500'000;
    return config;
}

CanFrame makeFrame(std::uint32_t identifier, std::uint8_t length)
{
    CanFrame frame;
    frame.identifier = identifier;

    // The format follows the identifier. Leaving it at the default meant a
    // 29-bit identifier was declared as a standard frame, and the backend
    // rejected the transmit outright - correctly, and the test read as though
    // the bus had failed. The same defect was fixed once in CanEngineTests'
    // helper and missed here.
    frame.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    frame.dlc = length;
    frame.length = length;
    for (std::uint8_t index = 0; index < length; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(0x10 + index);
    }
    return frame;
}

} // namespace

TEST(VirtualBusTests, TheVirtualBackendAdvertisesTwoChannels)
{
    VirtualCanBackend backend;

    EXPECT_TRUE(backend.name() == "virtual");
    EXPECT_TRUE(backend.isAvailable());

    const CanDeviceInfoList devices = backend.enumerate();
    ASSERT_TRUE(devices.size() == VirtualCanBackend::kChannelCount);

    EXPECT_TRUE(devices[0].handle == "virtual:0");
    EXPECT_TRUE(devices[1].handle == "virtual:1");
    EXPECT_TRUE(devices[0].capabilities.virtualDevice);
    EXPECT_TRUE(devices[0].capabilities.canFd);
    EXPECT_FALSE(devices[0].capabilities.hardwareTimestamp);
}

TEST(VirtualBusTests, OpeningRejectsHandlesThatAreNotOurs)
{
    VirtualCanBackend backend;

    EXPECT_TRUE(backend.open(configFor("kvaser:0", 0)).code() == ErrorCode::DeviceNotFound);
    EXPECT_TRUE(backend.open(configFor("virtual:99", 0)).code() == ErrorCode::DeviceNotFound);
    EXPECT_TRUE(backend.open(configFor("", 0)).code() == ErrorCode::DeviceNotFound);

    EXPECT_TRUE(backend.open(configFor("virtual:0", 0)).succeeded());
    EXPECT_TRUE(backend.isOpen());

    // A second open on the same instance is a programming error, not a retry.
    EXPECT_TRUE(backend.open(configFor("virtual:0", 0)).code() == ErrorCode::InvalidState);
}

TEST(VirtualBusTests, TransmittingBeforeStartFailsInsteadOfVanishing)
{
    VirtualCanBackend backend;
    ASSERT_TRUE(backend.open(configFor("virtual:0", 0)).succeeded());

    EXPECT_TRUE(backend.transmit(makeFrame(0x100, 8)).code() == ErrorCode::ChannelNotOpen);

    ASSERT_TRUE(backend.start().succeeded());
    EXPECT_TRUE(backend.transmit(makeFrame(0x100, 8)).succeeded());
}

TEST(VirtualBusTests, VirtualChannel0ReachesVirtualChannel1)
{
    // This is the v0.3 acceptance test, rehearsed without hardware.
    FrameCollector sender;
    FrameCollector receiver;

    VirtualCanBackend nodeA;
    VirtualCanBackend nodeB;

    nodeA.setFrameHandler(sender.handler());
    nodeB.setFrameHandler(receiver.handler());

    ASSERT_TRUE(nodeA.open(configFor("virtual:0", 0)).succeeded());
    ASSERT_TRUE(nodeB.open(configFor("virtual:0", 1)).succeeded());

    ASSERT_TRUE(nodeA.start().succeeded());
    ASSERT_TRUE(nodeB.start().succeeded());

    ASSERT_TRUE(nodeA.transmit(makeFrame(0x18FF50E5, 8)).succeeded());
    {
        ASSERT_TRUE(receiver.waitFor(1));
        ASSERT_TRUE(receiver.count() == 1);

        const CanFrame& received = receiver.frames().front();
        EXPECT_TRUE(received.identifier == 0x18FF50E5);
        EXPECT_TRUE(received.direction == CanDirection::Rx);
        EXPECT_TRUE(received.channel == 1);
        EXPECT_TRUE(received.length == 8);
        EXPECT_TRUE(received.data[0] == 0x10);
        EXPECT_TRUE(received.timestampNs > 0);
    }
    {
        // Rule: the trace shows what actually reached the bus, not what was
        // requested - so a successful transmission comes back through the same
        // receive path as everything else.
        ASSERT_TRUE(sender.waitFor(1));
        ASSERT_TRUE(sender.count() == 1);

        const CanFrame& echoed = sender.frames().front();
        EXPECT_TRUE(echoed.direction == CanDirection::Tx);
        EXPECT_TRUE(echoed.channel == 0);
        EXPECT_TRUE(echoed.identifier == 0x18FF50E5);
    }
}

TEST(VirtualBusTests, SeparateVirtualChannelsAreSeparateBuses)
{
    FrameCollector onChannel0;
    FrameCollector onChannel1;

    VirtualCanBackend nodeA;
    VirtualCanBackend nodeB;

    nodeA.setFrameHandler(onChannel0.handler());
    nodeB.setFrameHandler(onChannel1.handler());

    ASSERT_TRUE(nodeA.open(configFor("virtual:0", 0)).succeeded());
    ASSERT_TRUE(nodeB.open(configFor("virtual:1", 1)).succeeded());
    ASSERT_TRUE(nodeA.start().succeeded());
    ASSERT_TRUE(nodeB.start().succeeded());

    ASSERT_TRUE(nodeA.transmit(makeFrame(0x200, 4)).succeeded());

    ASSERT_TRUE(onChannel0.waitFor(1));
    EXPECT_TRUE(onChannel0.count() == 1); // its own echo

    // The echo has landed, so anything that was going to cross buses has had
    // at least as long to do it. Checking a zero straight after a transmit
    // would pass on a bus that leaks, just slowly.
    EXPECT_TRUE(onChannel1.count() == 0);
}

TEST(VirtualBusTests, StoppingDetachesTheNodeFromTheBus)
{
    FrameCollector listener;

    VirtualCanBackend sender;
    VirtualCanBackend receiver;

    receiver.setFrameHandler(listener.handler());

    ASSERT_TRUE(sender.open(configFor("virtual:0", 0)).succeeded());
    ASSERT_TRUE(receiver.open(configFor("virtual:0", 1)).succeeded());
    ASSERT_TRUE(sender.start().succeeded());
    ASSERT_TRUE(receiver.start().succeeded());

    ASSERT_TRUE(sender.transmit(makeFrame(0x300, 2)).succeeded());
    ASSERT_TRUE(listener.waitFor(1));

    receiver.stop();
    ASSERT_TRUE(sender.transmit(makeFrame(0x301, 2)).succeeded());

    // Proving an absence, so waiting for an arrival is the wrong tool: the
    // grace period is what makes the check mean anything. Without it the frame
    // simply would not have arrived yet either way, and the test would pass
    // whether or not stop() worked.
    ASSERT_FALSE(listener.waitFor(2, std::chrono::milliseconds{150}));
    EXPECT_TRUE(listener.count() == 1);

    ASSERT_TRUE(receiver.start().succeeded());
    ASSERT_TRUE(sender.transmit(makeFrame(0x302, 2)).succeeded());
    ASSERT_TRUE(listener.waitFor(2));
    EXPECT_TRUE(listener.count() == 2);
}

TEST(VirtualBusTests, ListenOnlyChannelsNeverTransmit)
{
    CanChannelConfig config = configFor("virtual:0", 0);
    config.listenOnly = true;

    VirtualCanBackend backend;
    ASSERT_TRUE(backend.open(config).succeeded());
    ASSERT_TRUE(backend.start().succeeded());

    EXPECT_TRUE(backend.transmit(makeFrame(0x100, 1)).code() == ErrorCode::InvalidState);
}

TEST(VirtualBusTests, AnOutOfRangeIdentifierIsRefusedNotTruncated)
{
    VirtualCanBackend backend;
    ASSERT_TRUE(backend.open(configFor("virtual:0", 0)).succeeded());
    ASSERT_TRUE(backend.start().succeeded());

    CanFrame frame = makeFrame(0x800, 1);
    frame.format = CanFrameFormat::Standard;

    const Result result = backend.transmit(frame);
    EXPECT_TRUE(result.code() == ErrorCode::InvalidArgument);
    EXPECT_FALSE(result.message().empty());
}

TEST(VirtualBusTests, CANFDFramesNeedAChannelOpenedInFDMode)
{
    CanFrame frame = makeFrame(0x100, 8);
    frame.fd = true;
    frame.dlc = 15;
    {
        VirtualCanBackend backend;
        ASSERT_TRUE(backend.open(configFor("virtual:0", 0)).succeeded());
        ASSERT_TRUE(backend.start().succeeded());

        EXPECT_TRUE(backend.transmit(frame).code() == ErrorCode::UnsupportedFeature);
    }
    {
        FrameCollector collector;

        CanChannelConfig config = configFor("virtual:0", 0);
        config.canFdEnabled = true;

        VirtualCanBackend backend;
        backend.setFrameHandler(collector.handler());
        ASSERT_TRUE(backend.open(config).succeeded());
        ASSERT_TRUE(backend.start().succeeded());

        ASSERT_TRUE(backend.transmit(frame).succeeded());
        ASSERT_TRUE(collector.waitFor(1));
        EXPECT_TRUE(collector.frames().front().length == 64);
    }
}

TEST(VirtualBusTests, TheRegistryExposesTheVirtualBackendToTheApplication)
{
    CanBackendRegistry& registry = CanBackendRegistry::instance();
    registry.registerBuiltins();

    const std::unique_ptr<ICanBackend> backend = registry.create("virtual");
    ASSERT_TRUE(backend != nullptr);
    EXPECT_TRUE(backend->isAvailable());

    EXPECT_TRUE(registry.create("no-such-backend") == nullptr);

    const CanDeviceInfoList devices = registry.enumerateAll();
    EXPECT_TRUE(devices.size() >= VirtualCanBackend::kChannelCount);
}
