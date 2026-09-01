// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanStatistics.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace torquebus;
using Catch::Approx;

namespace {

constexpr std::uint64_t kOneSecondNs = 1'000'000'000ULL;

CanFrame frame(CanDirection direction = CanDirection::Rx, std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = 0x100;
    result.direction = direction;
    result.length = length;
    result.dlc = length;
    return result;
}

} // namespace

TEST_CASE("A fresh accumulator is all zeroes and offline", "[statistics]")
{
    const CanStatistics statistics;
    const CanStatisticsSnapshot snapshot = statistics.snapshot();

    CHECK(snapshot.rxFrames == 0);
    CHECK(snapshot.txFrames == 0);
    CHECK(snapshot.totalFrames() == 0);
    CHECK(snapshot.busLoadPercent == 0.0);
    CHECK(snapshot.state == CanBusState::Offline);
}

TEST_CASE("Rx and Tx are counted separately", "[statistics]")
{
    CanStatistics statistics;

    for (int index = 0; index < 7; ++index) {
        statistics.recordFrame(frame(CanDirection::Rx));
    }
    for (int index = 0; index < 3; ++index) {
        statistics.recordFrame(frame(CanDirection::Tx));
    }

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    CHECK(snapshot.rxFrames == 7);
    CHECK(snapshot.txFrames == 3);
    CHECK(snapshot.totalFrames() == 10);
}

TEST_CASE("Error frames are counted on top of their direction", "[statistics]")
{
    CanStatistics statistics;

    CanFrame errorFrame = frame();
    errorFrame.error = true;

    statistics.recordFrame(errorFrame);
    statistics.recordFrame(frame());

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    CHECK(snapshot.errorFrames == 1);
    CHECK(snapshot.rxFrames == 2); // an error frame is still a received frame
}

TEST_CASE("Frames per second reflects the window length", "[statistics]")
{
    CanStatistics statistics;

    for (int index = 0; index < 1000; ++index) {
        statistics.recordFrame(frame());
    }

    SECTION("a one second window")
    {
        statistics.closeWindow(kOneSecondNs);
        CHECK(statistics.snapshot().framesPerSecond == Approx(1000.0));
    }

    SECTION("a 100 ms window extrapolates")
    {
        statistics.closeWindow(kOneSecondNs / 10);
        CHECK(statistics.snapshot().framesPerSecond == Approx(10000.0));
    }
}

TEST_CASE("A window resets the rate counters but not the totals", "[statistics]")
{
    CanStatistics statistics;

    for (int index = 0; index < 100; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);

    CHECK(statistics.snapshot().framesPerSecond == Approx(100.0));
    CHECK(statistics.snapshot().rxFrames == 100);

    // A second window with no traffic: the rate falls to zero, the running
    // total does not.
    statistics.closeWindow(kOneSecondNs);

    CHECK(statistics.snapshot().framesPerSecond == Approx(0.0));
    CHECK(statistics.snapshot().rxFrames == 100);
}

TEST_CASE("Bus load is derived from frame bits, not from the driver",
          "[statistics][busload]")
{
    // An adapter-reported percentage means different things on different
    // adapters. Computing it ourselves is the only way the number stays
    // comparable when the user swaps hardware.
    CanStatistics statistics;
    statistics.setBitrate(500'000);

    const CanFrame sample = frame(CanDirection::Rx, 8);
    const std::uint32_t bitsPerFrame = approximateFrameBitCount(sample);

    // Enough frames to fill roughly half the bus for one second.
    const auto frameCount = static_cast<int>((500'000 / 2) / bitsPerFrame);

    for (int index = 0; index < frameCount; ++index) {
        statistics.recordFrame(sample);
    }

    statistics.closeWindow(kOneSecondNs);

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    CHECK(snapshot.busLoadPercent > 45.0);
    CHECK(snapshot.busLoadPercent < 55.0);
    CHECK(snapshot.bitrate == 500'000);
}

TEST_CASE("Bus load never exceeds 100 percent", "[statistics][busload]")
{
    // Worst-case bit stuffing plus a short window can otherwise produce a
    // nonsense number like 118 %, which erodes trust in every other figure
    // on the panel.
    CanStatistics statistics;
    statistics.setBitrate(125'000);

    for (int index = 0; index < 10'000; ++index) {
        statistics.recordFrame(frame(CanDirection::Rx, 8));
    }

    statistics.closeWindow(kOneSecondNs / 10);

    CHECK(statistics.snapshot().busLoadPercent <= 100.0);
}

TEST_CASE("Peak bus load survives quieter windows", "[statistics][busload]")
{
    CanStatistics statistics;
    statistics.setBitrate(500'000);

    for (int index = 0; index < 2000; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);

    const double peak = statistics.snapshot().peakBusLoadPercent;
    CHECK(peak > 0.0);

    statistics.closeWindow(kOneSecondNs); // an idle window

    CHECK(statistics.snapshot().busLoadPercent == Approx(0.0));
    CHECK(statistics.snapshot().peakBusLoadPercent == Approx(peak));
}

TEST_CASE("Filtered and dropped frames are tracked apart from delivered ones",
          "[statistics]")
{
    // "The filter removed it" and "we could not keep up" are different
    // diagnoses and must never be collapsed into one number.
    CanStatistics statistics;

    statistics.recordFrame(frame());
    statistics.recordFiltered(5);
    statistics.setDropped(3);

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    CHECK(snapshot.rxFrames == 1);
    CHECK(snapshot.filteredFrames == 5);
    CHECK(snapshot.droppedFrames == 3);
}

TEST_CASE("setDropped overwrites rather than accumulates", "[statistics]")
{
    CanStatistics statistics;

    statistics.setDropped(10);
    statistics.setDropped(10);

    CHECK(statistics.snapshot().droppedFrames == 10);
}

TEST_CASE("reset() clears everything", "[statistics]")
{
    CanStatistics statistics;
    statistics.setBitrate(1'000'000);

    for (int index = 0; index < 50; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);
    statistics.reset();

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    CHECK(snapshot.totalFrames() == 0);
    CHECK(snapshot.peakBusLoadPercent == Approx(0.0));
    CHECK(snapshot.framesPerSecond == Approx(0.0));
}

TEST_CASE("A zero-length window is ignored rather than dividing by zero",
          "[statistics]")
{
    CanStatistics statistics;
    statistics.recordFrame(frame());

    statistics.closeWindow(0);

    CHECK(statistics.snapshot().framesPerSecond == Approx(0.0));
    CHECK(statistics.snapshot().rxFrames == 1);
}
