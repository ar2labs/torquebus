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

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
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

TEST_CASE("A fresh series holds nothing and claims no range", "[plot]")
{
    const SignalSeries series{"EngineData.EngineSpeed", "rpm", 8};

    CHECK(series.empty());
    CHECK(series.size() == 0);
    CHECK(series.capacity() == 8);
    CHECK(series.discarded() == 0);

    // Not ±infinity. An empty series that claimed a range would give an axis
    // to a plot with nothing on it.
    CHECK(series.minimum() == 0.0);
    CHECK(series.maximum() == 0.0);
}

TEST_CASE("Samples come back oldest first", "[plot]")
{
    SignalSeries series{"s", "", 8};

    series.append(10, 1.0);
    series.append(20, 2.0);
    series.append(30, 3.0);

    REQUIRE(series.size() == 3);
    CHECK(series.at(0).timestampNs == 10);
    CHECK(series.at(2).timestampNs == 30);
    CHECK(series.oldest().value == 1.0);
    CHECK(series.newest().value == 3.0);
}

TEST_CASE("A full ring drops the oldest and counts it", "[plot]")
{
    SignalSeries series{"s", "", 4};

    for (std::uint64_t index = 0; index < 10; ++index) {
        series.append(index, static_cast<double>(index));
    }

    // Bounded: a measurement left running overnight must not decide how much
    // memory the application uses.
    CHECK(series.size() == 4);
    CHECK(series.discarded() == 6);

    // And it is the *oldest* that went.
    CHECK(series.oldest().timestampNs == 6);
    CHECK(series.newest().timestampNs == 9);
}

TEST_CASE("The range does not shrink as history falls off the back", "[plot]")
{
    // Deliberately an over-estimate. An axis that shrank as the peak aged out
    // would make a steady signal look like it was growing - which is a lie
    // about the measurement, where a slightly wide axis is only a waste of
    // pixels.
    SignalSeries series{"s", "", 4};

    series.append(1, 100.0);  // the peak, which will age out
    for (std::uint64_t index = 2; index < 12; ++index) {
        series.append(index, 5.0);
    }

    CHECK(series.discarded() > 0);
    CHECK(series.maximum() == 100.0);
    CHECK(series.minimum() == 5.0);
}

TEST_CASE("A window copies the samples at or after its start", "[plot]")
{
    SignalSeries series{"s", "", 16};

    for (std::uint64_t index = 0; index < 10; ++index) {
        series.append(index * 100, static_cast<double>(index));
    }

    std::array<SignalSample, 16> buffer{};

    const std::size_t copied = series.copySince(500, buffer);

    REQUIRE(copied == 5);
    CHECK(buffer[0].timestampNs == 500);
    CHECK(buffer[4].timestampNs == 900);

    // A start before everything is the whole series; a start after everything
    // is nothing. Both are ordinary while a plot is being scrolled.
    CHECK(series.copySince(0, buffer) == 10);
    CHECK(series.copySince(10'000, buffer) == 0);
}

TEST_CASE("A window too small keeps the newest samples", "[plot]")
{
    // A plot that fell behind should show what is happening now, not where the
    // window began. Keeping the oldest would freeze the line at the moment the
    // buffer filled.
    SignalSeries series{"s", "", 32};

    for (std::uint64_t index = 0; index < 20; ++index) {
        series.append(index, static_cast<double>(index));
    }

    std::array<SignalSample, 3> buffer{};

    REQUIRE(series.copySince(0, buffer) == 3);
    CHECK(buffer[0].timestampNs == 17);
    CHECK(buffer[2].timestampNs == 19);
}

TEST_CASE("A window over a wrapped ring reads in time order", "[plot]")
{
    // The case the bisection in copySince has to survive: the newest sample is
    // physically before the oldest one in the underlying vector.
    SignalSeries series{"s", "", 4};

    for (std::uint64_t index = 0; index < 7; ++index) {
        series.append(index * 10, static_cast<double>(index));
    }

    REQUIRE(series.size() == 4);

    std::array<SignalSample, 8> buffer{};
    const std::size_t copied = series.copySince(0, buffer);

    REQUIRE(copied == 4);
    for (std::size_t index = 1; index < copied; ++index) {
        INFO("index " << index);
        CHECK(buffer[index].timestampNs > buffer[index - 1].timestampNs);
    }
}

TEST_CASE("A store gives each signal its own history", "[plot][store]")
{
    const Definitions definitions;
    SignalSeriesStore store{16};

    const std::vector<DecodedSignal> batch{
        sample(definitions, definitions.speed, 100, 800.0),
        sample(definitions, definitions.temperature, 100, 82.0),
        sample(definitions, definitions.speed, 200, 900.0),
    };

    store.append(batch);

    REQUIRE(store.seriesCount() == 2);

    const SeriesId speed = store.find("EngineData.EngineSpeed");
    REQUIRE(speed != kNoSeries);
    CHECK(store.series(speed).size() == 2);
    CHECK(store.series(speed).unit() == "rpm");

    const SeriesId temperature = store.find("EngineData.CoolantTemp");
    REQUIRE(temperature != kNoSeries);
    CHECK(store.series(temperature).size() == 1);

    // Qualified by the message, because two databases can use one name for two
    // different things and a legend showing "Speed" twice would be unreadable.
    CHECK(store.find("EngineSpeed") == kNoSeries);
}

TEST_CASE("A signal too short to read is counted, not plotted as zero",
          "[plot][store]")
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
    REQUIRE(speed != kNoSeries);

    CHECK(store.series(speed).size() == 2);
    CHECK(store.truncated() == 1);
    CHECK(store.series(speed).minimum() == 800.0);
}

TEST_CASE("Series keep their order of first sighting", "[plot][store]")
{
    // So a legend's order is the order the bus introduced the signals, rather
    // than a hash - two runs of the same measurement should read the same way.
    const Definitions definitions;
    SignalSeriesStore store{16};

    store.append(std::vector<DecodedSignal>{
        sample(definitions, definitions.temperature, 10, 80.0),
        sample(definitions, definitions.speed, 20, 700.0),
    });

    REQUIRE(store.all().size() == 2);
    CHECK(store.all()[0].name() == "EngineData.CoolantTemp");
    CHECK(store.all()[1].name() == "EngineData.EngineSpeed");
}

TEST_CASE("Clearing samples keeps the signals", "[plot][store]")
{
    // Restarting a measurement must not lose the plot's selection and colours,
    // which are keyed by series. A new *project* is what reset() is for.
    const Definitions definitions;
    SignalSeriesStore store{16};

    store.append(std::vector<DecodedSignal>{sample(definitions, definitions.speed, 10, 700.0)});
    REQUIRE(store.seriesCount() == 1);

    store.clearSamples();
    CHECK(store.seriesCount() == 1);
    CHECK(store.series(0).empty());
    CHECK(store.truncated() == 0);

    store.reset();
    CHECK(store.seriesCount() == 0);
    CHECK(store.find("EngineData.EngineSpeed") == kNoSeries);
}

TEST_CASE("A store survives a database being swapped underneath it",
          "[plot][store]")
{
    // The reason names are copied on first sighting rather than read through
    // the definition pointer. Reloading a .dbc mid-measurement is an ordinary
    // thing to do, and a series holding a pointer into the old database would
    // dangle the moment it happened.
    SignalSeriesStore store{16};

    {
        const Definitions temporary;
        store.append(
            std::vector<DecodedSignal>{sample(temporary, temporary.speed, 10, 700.0)});
    }

    // The definitions are gone. The series is not.
    const SeriesId speed = store.find("EngineData.EngineSpeed");
    REQUIRE(speed != kNoSeries);
    CHECK(store.series(speed).name() == "EngineData.EngineSpeed");
    CHECK(store.series(speed).unit() == "rpm");
    CHECK(store.series(speed).newest().value == 700.0);
}
