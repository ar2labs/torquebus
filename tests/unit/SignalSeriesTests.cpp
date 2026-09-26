// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The plot's memory. Almost every case here is about a boundary - the ring
// wrapping, a window that asks for more than exists, a signal seen for the
// first time - because those are the places a plot quietly starts lying about
// what the bus did.

#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"
#include "core/plot/SignalSeries.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A message and a signal that outlive every batch built from them, which is
/// the contract DecodedSignal documents.
struct Definitions final {
    CanMessage message;
    CanSignal speed;
    CanSignal temperature;

    Definitions()
    {
        message.name = "EngineData";
        message.identifier = 0x100;

        speed.name = "EngineSpeed";
        speed.unit = "rpm";

        temperature.name = "CoolantTemp";
        temperature.unit = "degC";
    }
};

[[nodiscard]] DecodedSignal sample(const Definitions& definitions,
                                   const CanSignal& signal,
                                   std::uint64_t timestampNs,
                                   double value,
                                   bool truncated = false)
{
    DecodedSignal decoded;
    decoded.timestampNs = timestampNs;
    decoded.message = &definitions.message;
    decoded.signal = &signal;
    decoded.value = value;
    decoded.truncated = truncated;
    return decoded;
}

} // namespace

TEST(SignalSeriesTests, AFreshSeriesHoldsNothingAndClaimsNoRange)
{
    const SignalSeries series{"EngineData.EngineSpeed", "rpm", 8};

    EXPECT_TRUE(series.empty());
    EXPECT_TRUE(series.size() == 0);
    EXPECT_TRUE(series.capacity() == 8);
    EXPECT_TRUE(series.discarded() == 0);

    // Not ±infinity. An empty series that claimed a range would give an axis
    // to a plot with nothing on it.
    EXPECT_TRUE(series.minimum() == 0.0);
    EXPECT_TRUE(series.maximum() == 0.0);
}

TEST(SignalSeriesTests, SamplesComeBackOldestFirst)
{
    SignalSeries series{"s", "", 8};

    series.append(10, 1.0);
    series.append(20, 2.0);
    series.append(30, 3.0);

    ASSERT_TRUE(series.size() == 3);
    EXPECT_TRUE(series.at(0).timestampNs == 10);
    EXPECT_TRUE(series.at(2).timestampNs == 30);
    EXPECT_TRUE(series.oldest().value == 1.0);
    EXPECT_TRUE(series.newest().value == 3.0);
}

TEST(SignalSeriesTests, AFullRingDropsTheOldestAndCountsIt)
{
    SignalSeries series{"s", "", 4};

    for (std::uint64_t index = 0; index < 10; ++index) {
        series.append(index, static_cast<double>(index));
    }

    // Bounded: a measurement left running overnight must not decide how much
    // memory the application uses.
    EXPECT_TRUE(series.size() == 4);
    EXPECT_TRUE(series.discarded() == 6);

    // And it is the *oldest* that went.
    EXPECT_TRUE(series.oldest().timestampNs == 6);
    EXPECT_TRUE(series.newest().timestampNs == 9);
}

TEST(SignalSeriesTests, TheRangeDoesNotShrinkAsHistoryFallsOffTheBack)
{
    // Deliberately an over-estimate. An axis that shrank as the peak aged out
    // would make a steady signal look like it was growing - which is a lie
    // about the measurement, where a slightly wide axis is only a waste of
    // pixels.
    SignalSeries series{"s", "", 4};

    series.append(1, 100.0); // the peak, which will age out
    for (std::uint64_t index = 2; index < 12; ++index) {
        series.append(index, 5.0);
    }

    EXPECT_TRUE(series.discarded() > 0);
    EXPECT_TRUE(series.maximum() == 100.0);
    EXPECT_TRUE(series.minimum() == 5.0);
}

TEST(SignalSeriesTests, AWindowCopiesTheSamplesAtOrAfterItsStart)
{
    SignalSeries series{"s", "", 16};

    for (std::uint64_t index = 0; index < 10; ++index) {
        series.append(index * 100, static_cast<double>(index));
    }

    std::array<SignalSample, 16> buffer{};

    const std::size_t copied = series.copySince(500, buffer);

    ASSERT_TRUE(copied == 5);
    EXPECT_TRUE(buffer[0].timestampNs == 500);
    EXPECT_TRUE(buffer[4].timestampNs == 900);

    // A start before everything is the whole series; a start after everything
    // is nothing. Both are ordinary while a plot is being scrolled.
    EXPECT_TRUE(series.copySince(0, buffer) == 10);
    EXPECT_TRUE(series.copySince(10'000, buffer) == 0);
}

TEST(SignalSeriesTests, AWindowTooSmallKeepsTheNewestSamples)
{
    // A plot that fell behind should show what is happening now, not where the
    // window began. Keeping the oldest would freeze the line at the moment the
    // buffer filled.
    SignalSeries series{"s", "", 32};

    for (std::uint64_t index = 0; index < 20; ++index) {
        series.append(index, static_cast<double>(index));
    }

    std::array<SignalSample, 3> buffer{};

    ASSERT_TRUE(series.copySince(0, buffer) == 3);
    EXPECT_TRUE(buffer[0].timestampNs == 17);
    EXPECT_TRUE(buffer[2].timestampNs == 19);
}

TEST(SignalSeriesTests, AWindowOverAWrappedRingReadsInTimeOrder)
{
    // The case the bisection in copySince has to survive: the newest sample is
    // physically before the oldest one in the underlying vector.
    SignalSeries series{"s", "", 4};

    for (std::uint64_t index = 0; index < 7; ++index) {
        series.append(index * 10, static_cast<double>(index));
    }

    ASSERT_TRUE(series.size() == 4);

    std::array<SignalSample, 8> buffer{};
    const std::size_t copied = series.copySince(0, buffer);

    ASSERT_TRUE(copied == 4);
    for (std::size_t index = 1; index < copied; ++index) {
        SCOPED_TRACE(::testing::Message() << "index " << index);
        EXPECT_TRUE(buffer[index].timestampNs > buffer[index - 1].timestampNs);
    }
}

TEST(SignalSeriesTests, AStoreGivesEachSignalItsOwnHistory)
{
    const Definitions definitions;
    SignalSeriesStore store{16};

    const std::vector<DecodedSignal> batch{
        sample(definitions, definitions.speed, 100, 800.0),
        sample(definitions, definitions.temperature, 100, 82.0),
        sample(definitions, definitions.speed, 200, 900.0),
    };

    store.append(batch);

    ASSERT_TRUE(store.seriesCount() == 2);

    const SeriesId speed = store.find("EngineData.EngineSpeed");
    ASSERT_TRUE(speed != kNoSeries);

    const std::vector<SeriesInfo> infos = store.listSeries();
    ASSERT_TRUE(infos.size() == 2);
    EXPECT_TRUE(infos[speed].size == 2);
    EXPECT_TRUE(infos[speed].unit == "rpm");

    const SeriesId temperature = store.find("EngineData.CoolantTemp");
    ASSERT_TRUE(temperature != kNoSeries);
    EXPECT_TRUE(infos[temperature].size == 1);

    // Qualified by the message, because two databases can use one name for two
    // different things and a legend showing "Speed" twice would be unreadable.
    EXPECT_TRUE(store.find("EngineSpeed") == kNoSeries);
}

TEST(SignalSeriesTests, ASignalTooShortToReadIsCountedNotPlottedAsZero)
{
    // The distinction DecodedSignal::truncated exists to preserve. A frame that
    // could not hold the signal did not report zero; a line drawn through those
    // zeroes is a measurement that never happened.
    const Definitions definitions;
    SignalSeriesStore store{16};

    const std::vector<DecodedSignal> batch{
        sample(definitions, definitions.speed, 100, 800.0),
        sample(definitions, definitions.speed, 200, 0.0, /*truncated=*/true),
        sample(definitions, definitions.speed, 300, 850.0),
    };

    store.append(batch);

    const SeriesId speed = store.find("EngineData.EngineSpeed");
    ASSERT_TRUE(speed != kNoSeries);

    std::vector<SeriesWindow> windows;
    store.readWindows(std::array<SeriesId, 1>{speed}, 0, 64, windows);

    ASSERT_TRUE(windows.size() == 1);
    EXPECT_TRUE(windows[0].samples.size() == 2);
    EXPECT_TRUE(windows[0].minimum == 800.0);
    EXPECT_TRUE(store.truncated() == 1);
}

TEST(SignalSeriesTests, SeriesKeepTheirOrderOfFirstSighting)
{
    // So a legend's order is the order the bus introduced the signals, rather
    // than a hash - two runs of the same measurement should read the same way.
    const Definitions definitions;
    SignalSeriesStore store{16};

    store.append(std::vector<DecodedSignal>{
        sample(definitions, definitions.temperature, 10, 80.0),
        sample(definitions, definitions.speed, 20, 700.0),
    });

    const std::vector<SeriesInfo> infos = store.listSeries();
    ASSERT_TRUE(infos.size() == 2);
    EXPECT_TRUE(infos[0].name == "EngineData.CoolantTemp");
    EXPECT_TRUE(infos[1].name == "EngineData.EngineSpeed");
}

TEST(SignalSeriesTests, ClearingSamplesKeepsTheSignals)
{
    // Restarting a measurement must not lose the plot's selection and colours,
    // which are keyed by series. A new *project* is what reset() is for.
    const Definitions definitions;
    SignalSeriesStore store{16};

    store.append(std::vector<DecodedSignal>{sample(definitions, definitions.speed, 10, 700.0)});
    ASSERT_TRUE(store.seriesCount() == 1);

    store.clearSamples();
    EXPECT_TRUE(store.seriesCount() == 1);
    EXPECT_TRUE(store.listSeries()[0].size == 0);
    EXPECT_TRUE(store.truncated() == 0);

    store.reset();
    EXPECT_TRUE(store.seriesCount() == 0);
    EXPECT_TRUE(store.find("EngineData.EngineSpeed") == kNoSeries);
}

TEST(SignalSeriesTests, AStoreSurvivesADatabaseBeingSwappedUnderneathIt)
{
    // The reason names are copied on first sighting rather than read through
    // the definition pointer. Reloading a .dbc mid-measurement is an ordinary
    // thing to do, and a series holding a pointer into the old database would
    // dangle the moment it happened.
    SignalSeriesStore store{16};

    {
        const Definitions temporary;
        store.append(std::vector<DecodedSignal>{sample(temporary, temporary.speed, 10, 700.0)});
    }

    // The definitions are gone. The series is not.
    const SeriesId speed = store.find("EngineData.EngineSpeed");
    ASSERT_TRUE(speed != kNoSeries);

    std::vector<SeriesWindow> windows;
    store.readWindows(std::array<SeriesId, 1>{speed}, 0, 64, windows);

    ASSERT_TRUE(windows.size() == 1);
    EXPECT_TRUE(windows[0].name == "EngineData.EngineSpeed");
    EXPECT_TRUE(windows[0].unit == "rpm");
    ASSERT_TRUE(windows[0].samples.size() == 1);
    EXPECT_TRUE(windows[0].samples[0].value == 700.0);
}

TEST(SignalSeriesTests, ReadingAWindowWhileTheExecutorAppendsIsSafe)
{
    // The claim the store's mutex exists to make, and the reason it exists at
    // all where TraceStore has none: a plot is read by *walking a ring*, and a
    // bisection over indices that a concurrent wrap invalidates mid-search is a
    // torn read rather than a late one.
    //
    // Run this under -fsanitize=thread to make the check mean something. Under
    // ASan or a plain build it exercises the path and catches corruption, which
    // is worth having and is not the same thing.
    const Definitions definitions;
    SignalSeriesStore store{256};

    std::atomic<bool> stop{false};

    std::thread writer{[&] {
        std::uint64_t timestamp = 0;
        std::vector<DecodedSignal> batch;

        while (!stop.load(std::memory_order_relaxed)) {
            batch.clear();
            for (int index = 0; index < 32; ++index) {
                batch.push_back(sample(definitions,
                                       definitions.speed,
                                       ++timestamp,
                                       static_cast<double>(timestamp % 100)));
                batch.push_back(sample(definitions,
                                       definitions.temperature,
                                       timestamp,
                                       static_cast<double>(timestamp % 50)));
            }
            store.append(batch);
        }
    }};

    std::vector<SeriesWindow> windows;
    std::size_t reads = 0;

    // Long enough for the rings to wrap several times over, which is the state
    // the bisection has to survive.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};

    while (std::chrono::steady_clock::now() < deadline) {
        const std::vector<SeriesInfo> infos = store.listSeries();

        std::vector<SeriesId> ids;
        ids.reserve(infos.size());
        for (const SeriesInfo& info : infos) {
            ids.push_back(info.id);
        }

        store.readWindows(ids, store.newestTimestampNs() / 2, 128, windows);

        for (const SeriesWindow& window : windows) {
            // Whatever was read has to be internally consistent: in time order,
            // and inside the range that was read alongside it.
            for (std::size_t index = 1; index < window.samples.size(); ++index) {
                ASSERT_TRUE(window.samples[index].timestampNs
                            >= window.samples[index - 1].timestampNs);
            }

            for (const SignalSample& point : window.samples) {
                ASSERT_TRUE(point.value >= window.minimum);
                ASSERT_TRUE(point.value <= window.maximum);
            }
        }

        ++reads;
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // The test is worthless if it never actually read anything.
    EXPECT_TRUE(reads > 10);
    EXPECT_TRUE(store.discarded() > 0);
}
