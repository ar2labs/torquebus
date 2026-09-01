// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/trace/TraceStore.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace torquebus;

namespace {

CanFrame frame(std::uint32_t identifier,
               std::uint64_t timestampNs,
               std::uint8_t channel = 0,
               std::uint8_t length = 8)
{
    CanFrame result;
    result.identifier = identifier;
    result.timestampNs = timestampNs;
    result.channel = channel;
    result.length = length;
    result.dlc = length;
    result.format = identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended
                                                        : CanFrameFormat::Standard;
    return result;
}

} // namespace

TEST_CASE("A row stays small enough to keep a million of them", "[trace]")
{
    // Every 8 bytes here is 8 MB of resident memory at full capacity.
    STATIC_REQUIRE(sizeof(TraceRow) <= 104);
}

TEST_CASE("A new store is empty", "[trace]")
{
    const TraceStore store{100};

    CHECK(store.empty());
    CHECK(store.size() == 0);
    CHECK(store.capacity() == 100);
    CHECK(store.totalAppended() == 0);
    CHECK(store.discarded() == 0);
    CHECK(store.identifiers().empty());
}

TEST_CASE("Rows come back in arrival order", "[trace]")
{
    TraceStore store{100};

    std::vector<CanFrame> batch;
    for (std::uint32_t index = 0; index < 5; ++index) {
        batch.push_back(frame(0x100 + index, (index + 1) * 1000));
    }
    store.append(batch);

    REQUIRE(store.size() == 5);
    for (std::uint32_t index = 0; index < 5; ++index) {
        CHECK(store.row(index).frame.identifier == 0x100 + index);
    }
}

TEST_CASE("The oldest rows fall off a full ring, and are counted", "[trace][ring]")
{
    // A trace that grows until the machine swaps is worse than one that says
    // "showing the last N". The count is what makes the loss honest.
    TraceStore store{4};

    std::vector<CanFrame> batch;
    for (std::uint32_t index = 0; index < 10; ++index) {
        batch.push_back(frame(0x100 + index, (index + 1) * 1000));
    }
    store.append(batch);

    CHECK(store.size() == 4);
    CHECK(store.capacity() == 4);
    CHECK(store.totalAppended() == 10);
    CHECK(store.discarded() == 6);

    // Row 0 is the oldest *retained*, not the oldest ever seen.
    CHECK(store.row(0).frame.identifier == 0x106);
    CHECK(store.row(3).frame.identifier == 0x109);
}

TEST_CASE("The ring survives many wraps", "[trace][ring]")
{
    TraceStore store{8};

    for (std::uint32_t index = 0; index < 1000; ++index) {
        const CanFrame single = frame(0x100 + index, (index + 1) * 1000);
        store.append(std::span<const CanFrame>{&single, 1});
    }

    CHECK(store.size() == 8);
    CHECK(store.totalAppended() == 1000);
    CHECK(store.discarded() == 992);
    CHECK(store.row(7).frame.identifier == 0x100 + 999);
    CHECK(store.row(0).frame.identifier == 0x100 + 992);
}

TEST_CASE("Delta is the gap since the previous frame on any channel",
          "[trace][timing]")
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1'000'000),  // 1 ms
        frame(0x200, 1'500'000),  // +500 us
        frame(0x100, 3'000'000),  // +1500 us
    };
    store.append(batch);

    CHECK(store.row(0).deltaUs == 0); // nothing to measure against
    CHECK(store.row(1).deltaUs == 500);
    CHECK(store.row(2).deltaUs == 1500);
}

TEST_CASE("Cycle is the gap since the same identifier on the same channel",
          "[trace][timing]")
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1'000'000),
        frame(0x200, 1'500'000),
        frame(0x100, 3'000'000), // 2000 us after the previous 0x100
        frame(0x200, 3'500'000), // 2000 us after the previous 0x200
    };
    store.append(batch);

    // Zero on a first sighting, not "0 ms" - which would read as "arriving
    // constantly" rather than "first time seen".
    CHECK(store.row(0).cycleUs == 0);
    CHECK(store.row(1).cycleUs == 0);
    CHECK(store.row(2).cycleUs == 2000);
    CHECK(store.row(3).cycleUs == 2000);
}

TEST_CASE("The same identifier on two channels is two identifiers",
          "[trace][timing]")
{
    // CAN 1 and CAN 2 are different buses. 0x100 on one has nothing to do with
    // 0x100 on the other, and sharing a cycle time between them would be a
    // fabricated number.
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1'000'000, 0),
        frame(0x100, 1'200'000, 1),
        frame(0x100, 3'000'000, 0),
    };
    store.append(batch);

    CHECK(store.row(1).cycleUs == 0);    // first sighting on channel 1
    CHECK(store.row(2).cycleUs == 2000); // measured against channel 0 only
    CHECK(store.identifiers().size() == 2);
}

TEST_CASE("Occurrence counts sightings of one identifier", "[trace]")
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1000),
        frame(0x200, 2000),
        frame(0x100, 3000),
        frame(0x100, 4000),
    };
    store.append(batch);

    CHECK(store.row(0).occurrence == 1);
    CHECK(store.row(1).occurrence == 1);
    CHECK(store.row(2).occurrence == 2);
    CHECK(store.row(3).occurrence == 3);
}

TEST_CASE("The identifier index keeps first-seen order", "[trace][fixed]")
{
    // The fixed view must not reshuffle itself while someone is reading it.
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x300, 1000),
        frame(0x100, 2000),
        frame(0x200, 3000),
        frame(0x100, 4000),
    };
    store.append(batch);

    REQUIRE(store.identifiers().size() == 3);
    CHECK(store.identifiers()[0].identifier == 0x300);
    CHECK(store.identifiers()[1].identifier == 0x100);
    CHECK(store.identifiers()[2].identifier == 0x200);
    CHECK(store.identifiers()[1].count == 2);
}

TEST_CASE("Min and max cycle track the spread", "[trace][fixed]")
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 0),
        frame(0x100, 1'000'000), // 1000 us
        frame(0x100, 6'000'000), // 5000 us
        frame(0x100, 8'000'000), // 2000 us
    };
    store.append(batch);

    REQUIRE(store.identifiers().size() == 1);
    const TraceIdentifierStats& stats = store.identifiers().front();

    CHECK(stats.count == 4);
    CHECK(stats.lastCycleUs == 2000);
    CHECK(stats.minCycleUs == 1000);
    CHECK(stats.maxCycleUs == 5000);
}

TEST_CASE("Changed bytes are flagged between consecutive frames",
          "[trace][fixed]")
{
    // The single most useful thing a fixed-ID view does: show which byte moved.
    TraceStore store{100};

    CanFrame first = frame(0x100, 1000);
    first.data[0] = 0x11;
    first.data[1] = 0x22;
    first.data[2] = 0x33;

    CanFrame second = frame(0x100, 2000);
    second.data[0] = 0x11; // unchanged
    second.data[1] = 0x99; // changed
    second.data[2] = 0x33; // unchanged

    store.append(std::span<const CanFrame>{&first, 1});
    store.append(std::span<const CanFrame>{&second, 1});

    REQUIRE(store.identifiers().size() == 1);
    CHECK(store.identifiers().front().changedBytes == (std::uint64_t{1} << 1U));
}

TEST_CASE("A timestamp that goes backwards does not produce a huge delta",
          "[trace][timing]")
{
    // Two adapters with unsynchronised clocks, or a replayed log spliced out of
    // order. A wrapped subtraction here would show a delta of several thousand
    // years, which is the kind of number that makes a user distrust the whole
    // panel.
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 5'000'000),
        frame(0x200, 1'000'000), // earlier than the one before it
    };
    store.append(batch);

    CHECK(store.row(1).deltaUs == 0);
}

TEST_CASE("clear() empties everything, including the identifier index",
          "[trace]")
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{frame(0x100, 1000), frame(0x200, 2000)};
    store.append(batch);
    REQUIRE(store.size() == 2);

    store.clear();

    CHECK(store.empty());
    CHECK(store.totalAppended() == 0);
    CHECK(store.discarded() == 0);
    CHECK(store.identifiers().empty());
    CHECK(store.capacity() == 100); // the allocation is kept

    // And a fresh append starts cleanly rather than continuing the old counts.
    store.append(batch);
    CHECK(store.row(0).occurrence == 1);
    CHECK(store.row(0).deltaUs == 0);
}

TEST_CASE("Extended and standard identifiers with the same value are distinct",
          "[trace]")
{
    TraceStore store{100};

    CanFrame standard = frame(0x100, 1000);
    standard.format = CanFrameFormat::Standard;

    CanFrame extended = frame(0x100, 2000);
    extended.format = CanFrameFormat::Extended;

    store.append(std::span<const CanFrame>{&standard, 1});
    store.append(std::span<const CanFrame>{&extended, 1});

    CHECK(store.identifiers().size() == 2);
    CHECK(store.row(1).cycleUs == 0); // not measured against the standard one
}
