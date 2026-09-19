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

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("The virtual backend advertises two channels", "[virtual][enumerate]")
{
    VirtualCanBackend backend;

    CHECK(backend.name() == "virtual");
    CHECK(backend.isAvailable());

    const CanDeviceInfoList devices = backend.enumerate();
    REQUIRE(devices.size() == VirtualCanBackend::kChannelCount);

    CHECK(devices[0].handle == "virtual:0");
    CHECK(devices[1].handle == "virtual:1");
    CHECK(devices[0].capabilities.virtualDevice);
    CHECK(devices[0].capabilities.canFd);
    CHECK_FALSE(devices[0].capabilities.hardwareTimestamp);
}

TEST_CASE("Opening rejects handles that are not ours", "[virtual][open]")
{
    VirtualCanBackend backend;

    CHECK(backend.open(configFor("kvaser:0", 0)).code() == ErrorCode::DeviceNotFound);
    CHECK(backend.open(configFor("virtual:99", 0)).code() == ErrorCode::DeviceNotFound);
    CHECK(backend.open(configFor("", 0)).code() == ErrorCode::DeviceNotFound);

    CHECK(backend.open(configFor("virtual:0", 0)).succeeded());
    CHECK(backend.isOpen());

    // A second open on the same instance is a programming error, not a retry.
    CHECK(backend.open(configFor("virtual:0", 0)).code() == ErrorCode::InvalidState);
}

TEST_CASE("Transmitting before start fails instead of vanishing", "[virtual][transmit]")
{
    VirtualCanBackend backend;
    REQUIRE(backend.open(configFor("virtual:0", 0)).succeeded());

    CHECK(backend.transmit(makeFrame(0x100, 8)).code() == ErrorCode::ChannelNotOpen);

    REQUIRE(backend.start().succeeded());
    CHECK(backend.transmit(makeFrame(0x100, 8)).succeeded());
}

TEST_CASE("Virtual channel 0 reaches virtual channel 1", "[virtual][loopback]")
{
    // This is the v0.3 acceptance test, rehearsed without hardware.
    FrameCollector sender;
    FrameCollector receiver;

    VirtualCanBackend nodeA;
    VirtualCanBackend nodeB;

    nodeA.setFrameHandler(sender.handler());
    nodeB.setFrameHandler(receiver.handler());

    REQUIRE(nodeA.open(configFor("virtual:0", 0)).succeeded());
    REQUIRE(nodeB.open(configFor("virtual:0", 1)).succeeded());

    REQUIRE(nodeA.start().succeeded());
    REQUIRE(nodeB.start().succeeded());

    REQUIRE(nodeA.transmit(makeFrame(0x18FF50E5, 8)).succeeded());

    SECTION("the receiver sees it as an Rx frame on its own application channel")
    {
        REQUIRE(receiver.waitFor(1));
        REQUIRE(receiver.count() == 1);

        const CanFrame& received = receiver.frames().front();
        CHECK(received.identifier == 0x18FF50E5);
        CHECK(received.direction == CanDirection::Rx);
        CHECK(received.channel == 1);
        CHECK(received.length == 8);
        CHECK(received.data[0] == 0x10);
        CHECK(received.timestampNs > 0);
    }

    SECTION("the sender sees its own frame echoed back as Tx")
    {
        // Rule: the trace shows what actually reached the bus, not what was
        // requested - so a successful transmission comes back through the same
        // receive path as everything else.
        //
        // The echo travels the same asynchronous path as any other frame, so
        // it needs the same wait. Catch2 runs each SECTION as a separate pass
        // through the whole test body, which means nothing in this one has
        // waited for anything.
        REQUIRE(sender.waitFor(1));
        REQUIRE(sender.count() == 1);

        const CanFrame& echoed = sender.frames().front();
        CHECK(echoed.direction == CanDirection::Tx);
        CHECK(echoed.channel == 0);
        CHECK(echoed.identifier == 0x18FF50E5);
    }
}

TEST_CASE("Separate virtual channels are separate buses", "[virtual][loopback]")
{
    FrameCollector onChannel0;
    FrameCollector onChannel1;

    VirtualCanBackend nodeA;
    VirtualCanBackend nodeB;

    nodeA.setFrameHandler(onChannel0.handler());
    nodeB.setFrameHandler(onChannel1.handler());

    REQUIRE(nodeA.open(configFor("virtual:0", 0)).succeeded());
    REQUIRE(nodeB.open(configFor("virtual:1", 1)).succeeded());
    REQUIRE(nodeA.start().succeeded());
    REQUIRE(nodeB.start().succeeded());

    REQUIRE(nodeA.transmit(makeFrame(0x200, 4)).succeeded());

    REQUIRE(onChannel0.waitFor(1));
    CHECK(onChannel0.count() == 1); // its own echo

    // The echo has landed, so anything that was going to cross buses has had
    // at least as long to do it. Checking a zero straight after a transmit
    // would pass on a bus that leaks, just slowly.
    CHECK(onChannel1.count() == 0);
}

TEST_CASE("Stopping detaches the node from the bus", "[virtual][lifecycle]")
{
    FrameCollector listener;

    VirtualCanBackend sender;
    VirtualCanBackend receiver;

    receiver.setFrameHandler(listener.handler());

    REQUIRE(sender.open(configFor("virtual:0", 0)).succeeded());
    REQUIRE(receiver.open(configFor("virtual:0", 1)).succeeded());
    REQUIRE(sender.start().succeeded());
    REQUIRE(receiver.start().succeeded());

    REQUIRE(sender.transmit(makeFrame(0x300, 2)).succeeded());
    REQUIRE(listener.waitFor(1));

    receiver.stop();
    REQUIRE(sender.transmit(makeFrame(0x301, 2)).succeeded());

    // Proving an absence, so waiting for an arrival is the wrong tool: the
    // grace period is what makes the check mean anything. Without it the frame
    // simply would not have arrived yet either way, and the test would pass
    // whether or not stop() worked.
    REQUIRE_FALSE(listener.waitFor(2, std::chrono::milliseconds{150}));
    CHECK(listener.count() == 1);

    REQUIRE(receiver.start().succeeded());
    REQUIRE(sender.transmit(makeFrame(0x302, 2)).succeeded());
    REQUIRE(listener.waitFor(2));
    CHECK(listener.count() == 2);
}

TEST_CASE("Listen-only channels never transmit", "[virtual][listenonly]")
{
    CanChannelConfig config = configFor("virtual:0", 0);
    config.listenOnly = true;

    VirtualCanBackend backend;
    REQUIRE(backend.open(config).succeeded());
    REQUIRE(backend.start().succeeded());

    CHECK(backend.transmit(makeFrame(0x100, 1)).code() == ErrorCode::InvalidState);
}

TEST_CASE("An out-of-range identifier is refused, not truncated", "[virtual][transmit]")
{
    VirtualCanBackend backend;
    REQUIRE(backend.open(configFor("virtual:0", 0)).succeeded());
    REQUIRE(backend.start().succeeded());

    CanFrame frame = makeFrame(0x800, 1);
    frame.format = CanFrameFormat::Standard;

    const Result result = backend.transmit(frame);
    CHECK(result.code() == ErrorCode::InvalidArgument);
    CHECK_FALSE(result.message().empty());
}

TEST_CASE("CAN FD frames need a channel opened in FD mode", "[virtual][canfd]")
{
    CanFrame frame = makeFrame(0x100, 8);
    frame.fd = true;
    frame.dlc = 15;

    SECTION("refused on a classic channel")
    {
        VirtualCanBackend backend;
        REQUIRE(backend.open(configFor("virtual:0", 0)).succeeded());
        REQUIRE(backend.start().succeeded());

        CHECK(backend.transmit(frame).code() == ErrorCode::UnsupportedFeature);
    }

    SECTION("accepted on an FD channel, with the DLC expanded on the way out")
    {
        FrameCollector collector;

        CanChannelConfig config = configFor("virtual:0", 0);
        config.canFdEnabled = true;

        VirtualCanBackend backend;
        backend.setFrameHandler(collector.handler());
        REQUIRE(backend.open(config).succeeded());
        REQUIRE(backend.start().succeeded());

        REQUIRE(backend.transmit(frame).succeeded());
        REQUIRE(collector.waitFor(1));
        CHECK(collector.frames().front().length == 64);
    }
}

TEST_CASE("The registry exposes the virtual backend to the application", "[registry]")
{
    CanBackendRegistry& registry = CanBackendRegistry::instance();
    registry.registerBuiltins();

    const std::unique_ptr<ICanBackend> backend = registry.create("virtual");
    REQUIRE(backend != nullptr);
    CHECK(backend->isAvailable());

    CHECK(registry.create("no-such-backend") == nullptr);

    const CanDeviceInfoList devices = registry.enumerateAll();
    CHECK(devices.size() >= VirtualCanBackend::kChannelCount);
}
