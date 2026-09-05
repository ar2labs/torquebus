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
#include "drivers/peak/PeakCanBackend.h"

#include <catch2/catch_test_macros.hpp>

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
        WARN("Built without Qt SerialBus - the PEAK backend is a stub in this build.");
        return false;
    }

    const PeakCanBackend backend;
    if (!backend.isAvailable()) {
        WARN("Qt SerialBus has no 'peakcan' plugin here. Install the PEAK-System "
             "driver (PCANBasic.dll) to run these.");
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

TEST_CASE("A build without Qt SerialBus still constructs a PEAK backend", "[peak]")
{
    // The stub contract, and the reason it exists: the registry, the Hardware
    // Manager and this test are identical in both builds. Nothing here asks
    // whether the plugin is present, because a build that cannot do PEAK must
    // still get this far.
    PeakCanBackend backend;

    CHECK(backend.name() == "peak");
    CHECK_FALSE(backend.isOpen());
    CHECK(PeakCanBackend::pluginName() == "peakcan");

    // Enumerating an unavailable backend is an empty list, never an error.
    CHECK(backend.enumerate().empty());

    if (!PeakCanBackend::isCompiledIn()) {
        // And opening it says why, rather than failing silently or crashing.
        const Result result = backend.open(CanChannelConfig{});
        CHECK(result.failed());
        CHECK(result.code() == ErrorCode::BackendUnavailable);
    }
}

TEST_CASE("Enumeration produces handles that name themselves", "[peak][hardware]")
{
    if (!peakUsable()) {
        SKIP("no peakcan plugin");
    }

    PeakCanBackend backend;

    for (const CanDeviceInfo& device : backend.enumerate()) {
        INFO("device " << device.handle);

        // The handle is persisted in .tbsproj files and handed back to open()
        // on a later run, so its shape is a format, not a convenience.
        CHECK(device.handle.starts_with("peak:"));
        CHECK(device.handle.size() > 5);
        CHECK(device.backend == "peak");
        CHECK_FALSE(device.name.empty());

        // Classic CAN always; FD only when the adapter says so. An adapter
        // advertising BRS without FD would be a contradiction worth catching.
        CHECK(device.capabilities.canClassic);
        if (device.capabilities.canFdBrs) {
            CHECK(device.capabilities.canFd);
        }
    }
}

TEST_CASE("Opening a handle that names nothing fails without hanging", "[peak][hardware]")
{
    if (!peakUsable()) {
        SKIP("no peakcan plugin");
    }

    PeakCanBackend backend;

    CanChannelConfig config;
    config.deviceHandle = "peak:no_such_interface";

    // The interesting half is not that this fails - it is that it returns. The
    // failure path creates a thread, fails to build a device on it, and has to
    // unwind that thread before answering. Getting that wrong is a hang on a
    // typo'd handle, which is a thing a user does.
    const Result result = backend.open(config);

    CHECK(result.failed());
    CHECK_FALSE(backend.isOpen());
}

TEST_CASE("A channel opens, starts, stops and closes in that order",
          "[peak][hardware]")
{
    if (!peakUsable()) {
        SKIP("no peakcan plugin");
    }

    const std::string handle = firstHandle();
    if (handle.empty()) {
        SKIP("peakcan plugin present, but no PCAN adapter is connected");
    }

    PeakCanBackend backend;

    CanChannelConfig config;
    config.deviceHandle = handle;
    config.applicationChannel = 0;
    config.timing.bitrate = kDefaultBitrate;

    REQUIRE(backend.open(config).succeeded());
    CHECK(backend.isOpen());

    // Off the bus until started, which is the contract open() advertises.
    CHECK(backend.status().state == CanBusState::Offline);

    const Result started = backend.start();
    INFO("start: " << std::string{started.message()});
    REQUIRE(started.succeeded());

    // Let the plugin settle before asking the controller anything.
    std::this_thread::sleep_for(200ms);

    const CanBusStatus busStatus = backend.status();
    INFO("state: " << toString(busStatus.state));
    CHECK(busStatus.state != CanBusState::BusOff);

    backend.stop();
    CHECK(backend.status().state == CanBusState::Offline);

    // Still open after stop(): the contract says a stopped channel can be
    // started again without reapplying the configuration.
    CHECK(backend.isOpen());
    REQUIRE(backend.start().succeeded());
    backend.stop();

    backend.close();
    CHECK_FALSE(backend.isOpen());

    // Closing twice is a no-op, not a crash. The destructor closes too, so any
    // path that survives only one close would take the process down on exit.
    backend.close();
}

TEST_CASE("Frames sent on a bus with no other node come back as our own echo",
          "[peak][hardware]")
{
    if (!peakUsable()) {
        SKIP("no peakcan plugin");
    }

    const std::string handle = firstHandle();
    if (handle.empty()) {
        SKIP("peakcan plugin present, but no PCAN adapter is connected");
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

    REQUIRE(backend.open(config).succeeded());
    REQUIRE(backend.start().succeeded());

    CanFrame frame;
    frame.identifier = 0x123;
    frame.length = 3;
    frame.dlc = 3;
    frame.data[0] = 0xDE;
    frame.data[1] = 0xAD;
    frame.data[2] = 0xBE;

    const Result sent = backend.transmit(frame);
    INFO("transmit: " << std::string{sent.message()});

    // On a bus with nothing to acknowledge, the controller will not complete
    // the frame - so a failure here is the bus talking, not the backend. What
    // is asserted is that transmit() answered at all rather than blocking: it
    // hops onto the device's thread and waits, and a wrong hop deadlocks.
    std::this_thread::sleep_for(300ms);

    if (sent.succeeded()) {
        const std::lock_guard lock{mutex};

        // ReceiveOwnKey is on, so what we sent should come back to us - which
        // is how the trace shows what actually reached the bus rather than what
        // was requested. No adapter on a terminated bus means no echo, so this
        // is reported rather than required.
        if (received.empty()) {
            WARN("No echo came back. Expected on an unterminated bus with no "
                 "other node; a problem if this bus has traffic on it.");
        } else {
            CHECK(received.front().identifier == 0x123);
            CHECK(received.front().length == 3);
        }
    }

    backend.close();
}
