// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The PEAK backend against whatever PCAN adapter is on the machine.
//
// Unlike Kvaser, PEAK ships no virtual channels, so there is nothing to test
// against on a bare machine and nothing this file can assert about frames
// crossing a bus. What it can do - and what it exists for - is exercise the
// half that has no hardware in it: enumeration, the plugin lookup, and the
// open/start/stop/close ordering including the thread that carries the device.
//
// Those are exactly where this backend differs from Kvaser, and exactly what
// cannot be reasoned about from a header. A QCanBusDevice built on the wrong
// thread works perfectly until the first frame arrives on the other one; a
// close() that tears down in the wrong order hangs on exit and nowhere else.
// Both are caught here, on any machine with the PEAK driver, adapter or not.
//
// Every case skips rather than fails when the plugin is absent, so a checkout
// on a machine with no PEAK software still goes green.

#include "core/can/CanFrame.h"
#include "plugins/driver-peak/PeakCanBackend.h"

#include <gtest/gtest.h>

#include <iostream>

#include <chrono>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

/// True when this build and this machine can do PEAK at all.
[[nodiscard]] bool peakUsable()
{
    if (!PeakCanBackend::isCompiledIn()) {
        std::cout << (::testing::Message()
                      << "Built without Qt SerialBus - the PEAK backend is a stub in this build.")
                         .GetString()
                  << '\n';
        return false;
    }

    const PeakCanBackend backend;
    if (!backend.isAvailable()) {
        std::cout << (::testing::Message()
                      << "Qt SerialBus has no 'peakcan' plugin here. Install the PEAK-System "
                         "driver (PCANBasic.dll) to run these.")
                         .GetString()
                  << '\n';
        return false;
    }

    return true;
}

/// The first PEAK channel, or an empty handle when nothing is plugged in.
[[nodiscard]] std::string firstHandle()
{
    PeakCanBackend backend;
    const CanDeviceInfoList devices = backend.enumerate();
    return devices.empty() ? std::string{} : devices.front().handle;
}

} // namespace

TEST(PeakLoopbackTests, ABuildWithoutQtSerialBusStillConstructsAPEAKBackend)
{
    // The stub contract, and the reason it exists: the registry, the Hardware
    // Manager and this test are identical in both builds. Nothing here asks
    // whether the plugin is present, because a build that cannot do PEAK must
    // still get this far.
    PeakCanBackend backend;

    EXPECT_TRUE(backend.name() == "peak");
    EXPECT_FALSE(backend.isOpen());
    EXPECT_TRUE(PeakCanBackend::pluginName() == "peakcan");

    // Enumerating an unavailable backend is an empty list, never an error.
    EXPECT_TRUE(backend.enumerate().empty());

    if (!PeakCanBackend::isCompiledIn()) {
        // And opening it says why, rather than failing silently or crashing.
        const Result result = backend.open(CanChannelConfig{});
        EXPECT_TRUE(result.failed());
        EXPECT_TRUE(result.code() == ErrorCode::BackendUnavailable);
    }
}

TEST(PeakLoopbackTests, EnumerationProducesHandlesThatNameThemselves)
{
    if (!peakUsable()) {
        GTEST_SKIP() << "no peakcan plugin";
    }

    PeakCanBackend backend;

    for (const CanDeviceInfo& device : backend.enumerate()) {
        SCOPED_TRACE(::testing::Message() << "device " << device.handle);

        // The handle is persisted in .tbsproj files and handed back to open()
        // on a later run, so its shape is a format, not a convenience.
        EXPECT_TRUE(device.handle.starts_with("peak:"));
        EXPECT_TRUE(device.handle.size() > 5);
        EXPECT_TRUE(device.backend == "peak");
        EXPECT_FALSE(device.name.empty());

        // Classic CAN always; FD only when the adapter says so. An adapter
        // advertising BRS without FD would be a contradiction worth catching.
        EXPECT_TRUE(device.capabilities.canClassic);
        if (device.capabilities.canFdBrs) {
            EXPECT_TRUE(device.capabilities.canFd);
        }
    }
}

TEST(PeakLoopbackTests, OpeningAHandleThatNamesNothingFailsWithoutHanging)
{
    if (!peakUsable()) {
        GTEST_SKIP() << "no peakcan plugin";
    }

    PeakCanBackend backend;

    CanChannelConfig config;
    config.deviceHandle = "peak:no_such_interface";

    // The interesting half is not that this fails - it is that it returns. The
    // failure path creates a thread, fails to build a device on it, and has to
    // unwind that thread before answering. Getting that wrong is a hang on a
    // typo'd handle, which is a thing a user does.
    const Result result = backend.open(config);

    EXPECT_TRUE(result.failed());
    EXPECT_FALSE(backend.isOpen());
}

TEST(PeakLoopbackTests, AChannelOpensStartsStopsAndClosesInThatOrder)
{
    if (!peakUsable()) {
        GTEST_SKIP() << "no peakcan plugin";
    }

    const std::string handle = firstHandle();
    if (handle.empty()) {
        GTEST_SKIP() << "peakcan plugin present, but no PCAN adapter is connected";
    }

    PeakCanBackend backend;

    CanChannelConfig config;
    config.deviceHandle = handle;
    config.applicationChannel = 0;
    config.timing.bitrate = kDefaultBitrate;

    ASSERT_TRUE(backend.open(config).succeeded());
    EXPECT_TRUE(backend.isOpen());

    // Off the bus until started, which is the contract open() advertises.
    EXPECT_TRUE(backend.status().state == CanBusState::Offline);

    const Result started = backend.start();
    SCOPED_TRACE(::testing::Message() << "start: " << std::string{started.message()});
    ASSERT_TRUE(started.succeeded());

    // Let the plugin settle before asking the controller anything.
    std::this_thread::sleep_for(200ms);

    const CanBusStatus busStatus = backend.status();
    SCOPED_TRACE(::testing::Message() << "state: " << toString(busStatus.state));
    EXPECT_TRUE(busStatus.state != CanBusState::BusOff);

    backend.stop();
    EXPECT_TRUE(backend.status().state == CanBusState::Offline);

    // Still open after stop(): the contract says a stopped channel can be
    // started again without reapplying the configuration.
    EXPECT_TRUE(backend.isOpen());
    ASSERT_TRUE(backend.start().succeeded());
    backend.stop();

    backend.close();
    EXPECT_FALSE(backend.isOpen());

    // Closing twice is a no-op, not a crash. The destructor closes too, so any
    // path that survives only one close would take the process down on exit.
    backend.close();
}

TEST(PeakLoopbackTests, FramesSentOnABusWithNoOtherNodeComeBackAsOurOwnEcho)
{
    if (!peakUsable()) {
        GTEST_SKIP() << "no peakcan plugin";
    }

    const std::string handle = firstHandle();
    if (handle.empty()) {
        GTEST_SKIP() << "peakcan plugin present, but no PCAN adapter is connected";
    }

    std::mutex mutex;
    std::vector<CanFrame> received;

    PeakCanBackend backend;
    backend.setFrameHandler([&](std::span<const CanFrame> batch) {
        const std::lock_guard lock{mutex};
        received.insert(received.end(), batch.begin(), batch.end());
    });

    CanChannelConfig config;
    config.deviceHandle = handle;
    config.timing.bitrate = kDefaultBitrate;

    ASSERT_TRUE(backend.open(config).succeeded());
    ASSERT_TRUE(backend.start().succeeded());

    CanFrame frame;
    frame.identifier = 0x123;
    frame.length = 3;
    frame.dlc = 3;
    frame.data[0] = 0xDE;
    frame.data[1] = 0xAD;
    frame.data[2] = 0xBE;

    const Result sent = backend.transmit(frame);
    SCOPED_TRACE(::testing::Message() << "transmit: " << std::string{sent.message()});

    // On a bus with nothing to acknowledge, the controller will not complete
    // the frame - so a failure here is the bus talking, not the backend. What
    // is asserted is that transmit() answered at all rather than blocking: it
    // hops onto the device's thread and waits, and a wrong hop deadlocks.
    std::this_thread::sleep_for(300ms);

    if (sent.succeeded()) {
        const std::lock_guard lock{mutex};

        // Required, not hoped for. The echo is synthesised by transmit()
        // rather than read back from the driver - a QCanBusFrame carries no
        // direction, so a driver echo would arrive indistinguishable from
        // received traffic and the trace would call our own frame an Rx.
        //
        // Which means it does not depend on the bus at all: if transmit()
        // reported success, the echo is there.
        ASSERT_FALSE(received.empty());

        const CanFrame& echo = received.front();
        EXPECT_TRUE(echo.identifier == 0x123);
        EXPECT_TRUE(echo.length == 3);
        EXPECT_TRUE(echo.data[0] == 0xDE);

        // The whole point of synthesising it.
        EXPECT_FALSE(echo.isRx());
    }

    backend.close();
}
