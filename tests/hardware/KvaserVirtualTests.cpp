// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The v0.3 acceptance test, on real Kvaser channels:
//
//     Kvaser Virtual 0  ->  TorqueBus  ->  Kvaser Virtual 1
//
// These are the same assertions VirtualBusTests.cpp already makes against the
// built-in virtual bus. That is deliberate and is the point of ICanBackend: if
// the seam works, the same test reads the same way on either side of it, and a
// difference in outcome is a difference in the *driver*, not in what TorqueBus
// expects of one.
//
// Requires the Kvaser drivers to be installed. The Windows installer creates
// two virtual channels, so no physical adapter is needed - but a machine with
// no Kvaser software at all skips every test here rather than failing.

#include "core/can/CanFrame.h"
#include "plugins/driver-kvaser/KvaserCanBackend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

/// Collects frames from the backend's receive thread.
class FrameCollector final {
public:
    [[nodiscard]] FrameHandler handler()
    {
        return [this](std::span<const CanFrame> batch) {
            const std::lock_guard lock{m_mutex};
            m_frames.insert(m_frames.end(), batch.begin(), batch.end());
        };
    }

    [[nodiscard]] std::size_t count() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames.size();
    }

    [[nodiscard]] std::vector<CanFrame> frames() const
    {
        const std::lock_guard lock{m_mutex};
        return m_frames;
    }

    bool waitFor(std::size_t target, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (count() >= target) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return count() >= target;
    }

private:
    mutable std::mutex m_mutex;
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

CanFrame frame(std::uint32_t identifier, std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = identifier;
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    result.dlc = length;
    result.length = length;
    for (std::uint8_t index = 0; index < length; ++index) {
        result.data[index] = static_cast<std::uint8_t>(0xA0 + index);
    }
    return result;
}

/// The two virtual channels the Kvaser Windows installer creates, if present.
struct VirtualPair final {
    bool found{false};
    std::string first;
    std::string second;
};

[[nodiscard]] VirtualPair findVirtualPair()
{
    VirtualPair pair;

    KvaserCanBackend probe;
    if (!probe.isAvailable()) {
        return pair;
    }

    std::vector<std::string> handles;
    for (const CanDeviceInfo& device : probe.enumerate()) {
        if (device.capabilities.virtualDevice) {
            handles.push_back(device.handle);
        }
    }

    if (handles.size() >= 2) {
        pair.found = true;
        pair.first = handles[0];
        pair.second = handles[1];
    }

    return pair;
}

} // namespace

TEST_CASE("Kvaser CANlib is present and enumerates channels", "[kvaser]")
{
    KvaserCanBackend backend;

    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("This build was configured without Kvaser CANlib.");
    }

    if (!backend.isAvailable()) {
        SKIP("Kvaser CANlib is linked but no channels are present - the driver is "
             "probably not installed.");
    }

    const CanDeviceInfoList devices = backend.enumerate();
    REQUIRE_FALSE(devices.empty());

    for (const CanDeviceInfo& device : devices) {
        INFO("device: " << device.name << " (" << device.handle << ")");
        CHECK(device.backend == "kvaser");
        CHECK(device.handle.starts_with("kvaser:"));
        CHECK_FALSE(device.name.empty());
        CHECK(device.capabilities.canClassic);
    }
}

TEST_CASE("A Kvaser handle that is not ours is refused", "[kvaser]")
{
    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("Built without Kvaser CANlib.");
    }

    KvaserCanBackend backend;
    CHECK(backend.open(configFor("virtual:0", 0)).code() == ErrorCode::DeviceNotFound);
    CHECK(backend.open(configFor("", 0)).code() == ErrorCode::DeviceNotFound);
}

TEST_CASE("A non-standard bitrate is refused with an explanation", "[kvaser]")
{
    // Better a clear refusal than a channel that opens and then produces error
    // frames because we guessed its segment timing.
    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("Built without Kvaser CANlib.");
    }

    const VirtualPair pair = findVirtualPair();
    if (!pair.found) {
        SKIP("No Kvaser virtual channels available.");
    }

    CanChannelConfig config = configFor(pair.first, 0);
    config.timing.bitrate = 333'333;

    KvaserCanBackend backend;
    const Result result = backend.open(config);

    CHECK(result.code() == ErrorCode::BitTimingRejected);
    CHECK(result.message().find("333333") != std::string_view::npos);
}

TEST_CASE("Kvaser Virtual 0 reaches Kvaser Virtual 1", "[kvaser][loopback]")
{
    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("Built without Kvaser CANlib.");
    }

    const VirtualPair pair = findVirtualPair();
    if (!pair.found) {
        SKIP("Fewer than two Kvaser virtual channels - install the Kvaser drivers.");
    }

    INFO("sending on " << pair.first << ", listening on " << pair.second);

    FrameCollector sender;
    FrameCollector receiver;

    KvaserCanBackend nodeA;
    KvaserCanBackend nodeB;

    nodeA.setFrameHandler(sender.handler());
    nodeB.setFrameHandler(receiver.handler());

    REQUIRE(nodeA.open(configFor(pair.first, 0)).succeeded());
    REQUIRE(nodeB.open(configFor(pair.second, 1)).succeeded());

    REQUIRE(nodeA.start().succeeded());
    REQUIRE(nodeB.start().succeeded());

    REQUIRE(nodeA.transmit(frame(0x18FF50E5)).succeeded());

    REQUIRE(receiver.waitFor(1, 3s));

    const std::vector<CanFrame> received = receiver.frames();
    REQUIRE_FALSE(received.empty());

    const CanFrame& first = received.front();
    CHECK(first.identifier == 0x18FF50E5);
    CHECK(first.isExtended());
    CHECK(first.direction == CanDirection::Rx);
    CHECK(first.channel == 1);
    CHECK(first.length == 8);
    CHECK(first.data[0] == 0xA0);

    // Hardware timestamps are relative to the start of the measurement, so the
    // only thing worth asserting is that they are moving.
    CHECK(first.timestampNs > 0);

    // The sender must see its own frame come back as Tx, whether the driver
    // echoed it or the backend synthesised the echo. That the test cannot tell
    // which happened is the point.
    REQUIRE(sender.waitFor(1, 1s));
    CHECK(sender.frames().front().direction == CanDirection::Tx);
    CHECK(sender.frames().front().channel == 0);

    nodeA.stop();
    nodeB.stop();
}

TEST_CASE("Timestamps advance monotonically across a burst", "[kvaser][timestamps]")
{
    // The driver hands back a 32-bit tick count that the backend widens to 64
    // bits. A burst is not long enough to wrap it, but it is enough to catch a
    // conversion that goes backwards or stands still.
    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("Built without Kvaser CANlib.");
    }

    const VirtualPair pair = findVirtualPair();
    if (!pair.found) {
        SKIP("No Kvaser virtual channels available.");
    }

    FrameCollector receiver;

    KvaserCanBackend sender;
    KvaserCanBackend listener;
    listener.setFrameHandler(receiver.handler());

    REQUIRE(sender.open(configFor(pair.first, 0)).succeeded());
    REQUIRE(listener.open(configFor(pair.second, 1)).succeeded());
    REQUIRE(sender.start().succeeded());
    REQUIRE(listener.start().succeeded());

    constexpr int kBurst = 50;
    for (int index = 0; index < kBurst; ++index) {
        REQUIRE(sender.transmit(frame(0x100 + static_cast<std::uint32_t>(index), 8)).succeeded());
    }

    REQUIRE(receiver.waitFor(kBurst, 5s));

    const std::vector<CanFrame> frames = receiver.frames();
    REQUIRE(frames.size() >= kBurst);

    CHECK(std::is_sorted(frames.begin(), frames.end(), [](const CanFrame& a, const CanFrame& b) {
        return a.timestampNs < b.timestampNs;
    }));

    CHECK(frames.back().timestampNs > frames.front().timestampNs);

    sender.stop();
    listener.stop();
}

TEST_CASE("Listen-only channels never transmit", "[kvaser][listenonly]")
{
    if (!KvaserCanBackend::isCompiledIn()) {
        SKIP("Built without Kvaser CANlib.");
    }

    const VirtualPair pair = findVirtualPair();
    if (!pair.found) {
        SKIP("No Kvaser virtual channels available.");
    }

    CanChannelConfig config = configFor(pair.first, 0);
    config.listenOnly = true;

    KvaserCanBackend backend;
    REQUIRE(backend.open(config).succeeded());
    REQUIRE(backend.start().succeeded());

    CHECK(backend.transmit(frame(0x100, 1)).code() == ErrorCode::InvalidState);

    backend.stop();
}
