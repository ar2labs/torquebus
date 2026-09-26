// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanStatistics.h"

#include <gtest/gtest.h>

using namespace torquebus;

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

TEST(CanStatisticsTests, AFreshAccumulatorIsAllZeroesAndOffline)
{
    const CanStatistics statistics;
    const CanStatisticsSnapshot snapshot = statistics.snapshot();

    EXPECT_TRUE(snapshot.rxFrames == 0);
    EXPECT_TRUE(snapshot.txFrames == 0);
    EXPECT_TRUE(snapshot.totalFrames() == 0);
    EXPECT_TRUE(snapshot.busLoadPercent == 0.0);
    EXPECT_TRUE(snapshot.state == CanBusState::Offline);
}

TEST(CanStatisticsTests, AChannelNobodyConfiguredOpensAt250KbitS)
{
    // Pinned rather than left to drift. The default bitrate is the one number
    // in this project that decides whether a first-time user sees traffic or an
    // empty trace, and it is spelled in three places - the timing struct, the
    // statistics accumulator's fallback, and the controller's bind. A test is
    // the only thing that keeps the three agreeing.
    EXPECT_TRUE(kDefaultBitrate == 250'000);

    const CanBitTiming timing;
    EXPECT_TRUE(timing.bitrate == kDefaultBitrate);

    // The accumulator has its own copy, used until a channel pushes one in. A
    // disagreement here would show as a bus load computed against the wrong
    // denominator - a number that looks plausible and is simply wrong, which is
    // the worst kind.
    const CanStatistics statistics;
    EXPECT_TRUE(statistics.bitrate() == kDefaultBitrate);
    EXPECT_TRUE(statistics.snapshot().bitrate == kDefaultBitrate);
}

TEST(CanStatisticsTests, RxAndTxAreCountedSeparately)
{
    CanStatistics statistics;

    for (int index = 0; index < 7; ++index) {
        statistics.recordFrame(frame(CanDirection::Rx));
    }
    for (int index = 0; index < 3; ++index) {
        statistics.recordFrame(frame(CanDirection::Tx));
    }

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    EXPECT_TRUE(snapshot.rxFrames == 7);
    EXPECT_TRUE(snapshot.txFrames == 3);
    EXPECT_TRUE(snapshot.totalFrames() == 10);
}

TEST(CanStatisticsTests, ErrorFramesAreCountedOnTopOfTheirDirection)
{
    CanStatistics statistics;

    CanFrame errorFrame = frame();
    errorFrame.error = true;

    statistics.recordFrame(errorFrame);
    statistics.recordFrame(frame());

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    EXPECT_TRUE(snapshot.errorFrames == 1);
    EXPECT_TRUE(snapshot.rxFrames == 2); // an error frame is still a received frame
}

TEST(CanStatisticsTests, FramesPerSecondReflectsTheWindowLength)
{
    {
        CanStatistics statistics;
        for (int index = 0; index < 1000; ++index) {
            statistics.recordFrame(frame());
        }
        statistics.closeWindow(kOneSecondNs);
        EXPECT_DOUBLE_EQ(statistics.snapshot().framesPerSecond, 1000.0);
    }

    {
        CanStatistics statistics;
        for (int index = 0; index < 1000; ++index) {
            statistics.recordFrame(frame());
        }
        statistics.closeWindow(kOneSecondNs / 10);
        EXPECT_DOUBLE_EQ(statistics.snapshot().framesPerSecond, 10000.0);
    }
}

TEST(CanStatisticsTests, AWindowResetsTheRateCountersButNotTheTotals)
{
    CanStatistics statistics;

    for (int index = 0; index < 100; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);

    EXPECT_DOUBLE_EQ(statistics.snapshot().framesPerSecond, 100.0);
    EXPECT_TRUE(statistics.snapshot().rxFrames == 100);

    // A second window with no traffic: the rate falls to zero, the running
    // total does not.
    statistics.closeWindow(kOneSecondNs);

    EXPECT_DOUBLE_EQ(statistics.snapshot().framesPerSecond, 0.0);
    EXPECT_TRUE(statistics.snapshot().rxFrames == 100);
}

TEST(CanStatisticsTests, BusLoadIsDerivedFromFrameBitsNotFromTheDriver)
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
    EXPECT_TRUE(snapshot.busLoadPercent > 45.0);
    EXPECT_TRUE(snapshot.busLoadPercent < 55.0);
    EXPECT_TRUE(snapshot.bitrate == 500'000);
}

TEST(CanStatisticsTests, BusLoadNeverExceeds100Percent)
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

    EXPECT_TRUE(statistics.snapshot().busLoadPercent <= 100.0);
}

TEST(CanStatisticsTests, PeakBusLoadSurvivesQuieterWindows)
{
    CanStatistics statistics;
    statistics.setBitrate(500'000);

    for (int index = 0; index < 2000; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);

    const double peak = statistics.snapshot().peakBusLoadPercent;
    EXPECT_TRUE(peak > 0.0);

    statistics.closeWindow(kOneSecondNs); // an idle window

    EXPECT_DOUBLE_EQ(statistics.snapshot().busLoadPercent, 0.0);
    EXPECT_DOUBLE_EQ(statistics.snapshot().peakBusLoadPercent, peak);
}

TEST(CanStatisticsTests, FilteredAndDroppedFramesAreTrackedApartFromDeliveredOnes)
{
    // "The filter removed it" and "we could not keep up" are different
    // diagnoses and must never be collapsed into one number.
    CanStatistics statistics;

    statistics.recordFrame(frame());
    statistics.recordFiltered(5);
    statistics.setDropped(3);

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    EXPECT_TRUE(snapshot.rxFrames == 1);
    EXPECT_TRUE(snapshot.filteredFrames == 5);
    EXPECT_TRUE(snapshot.droppedFrames == 3);
}

TEST(CanStatisticsTests, SetDroppedOverwritesRatherThanAccumulates)
{
    CanStatistics statistics;

    statistics.setDropped(10);
    statistics.setDropped(10);

    EXPECT_TRUE(statistics.snapshot().droppedFrames == 10);
}

TEST(CanStatisticsTests, ResetClearsEverything)
{
    CanStatistics statistics;
    statistics.setBitrate(1'000'000);

    for (int index = 0; index < 50; ++index) {
        statistics.recordFrame(frame());
    }
    statistics.closeWindow(kOneSecondNs);
    statistics.reset();

    const CanStatisticsSnapshot snapshot = statistics.snapshot();
    EXPECT_TRUE(snapshot.totalFrames() == 0);
    EXPECT_DOUBLE_EQ(snapshot.peakBusLoadPercent, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.framesPerSecond, 0.0);
}

TEST(CanStatisticsTests, AZeroLengthWindowIsIgnoredRatherThanDividingByZero)
{
    CanStatistics statistics;
    statistics.recordFrame(frame());

    statistics.closeWindow(0);

    EXPECT_DOUBLE_EQ(statistics.snapshot().framesPerSecond, 0.0);
    EXPECT_TRUE(statistics.snapshot().rxFrames == 1);
}
