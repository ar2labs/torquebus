// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/trace/TraceStore.h"

#include <gtest/gtest.h>

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
    result.format =
        identifier > kMaxStandardIdentifier ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    return result;
}

} // namespace

TEST(TraceStoreTests, ARowStaysSmallEnoughToKeepAMillionOfThem)
{
    // Every 8 bytes here is 8 MB of resident memory at full capacity.
    static_assert(sizeof(TraceRow) <= 104);
}

TEST(TraceStoreTests, ANewStoreIsEmpty)
{
    const TraceStore store{100};

    EXPECT_TRUE(store.empty());
    EXPECT_TRUE(store.size() == 0);
    EXPECT_TRUE(store.capacity() == 100);
    EXPECT_TRUE(store.totalAppended() == 0);
    EXPECT_TRUE(store.discarded() == 0);
    EXPECT_TRUE(store.identifiers().empty());
}

TEST(TraceStoreTests, RowsComeBackInArrivalOrder)
{
    TraceStore store{100};

    std::vector<CanFrame> batch;
    for (std::uint32_t index = 0; index < 5; ++index) {
        batch.push_back(frame(0x100 + index, (index + 1) * 1000));
    }
    store.append(batch);

    ASSERT_TRUE(store.size() == 5);
    for (std::uint32_t index = 0; index < 5; ++index) {
        EXPECT_TRUE(store.row(index).frame.identifier == 0x100 + index);
    }
}

TEST(TraceStoreTests, TheOldestRowsFallOffAFullRingAndAreCounted)
{
    // A trace that grows until the machine swaps is worse than one that says
    // "showing the last N". The count is what makes the loss honest.
    TraceStore store{4};

    std::vector<CanFrame> batch;
    for (std::uint32_t index = 0; index < 10; ++index) {
        batch.push_back(frame(0x100 + index, (index + 1) * 1000));
    }
    store.append(batch);

    EXPECT_TRUE(store.size() == 4);
    EXPECT_TRUE(store.capacity() == 4);
    EXPECT_TRUE(store.totalAppended() == 10);
    EXPECT_TRUE(store.discarded() == 6);

    // Row 0 is the oldest *retained*, not the oldest ever seen.
    EXPECT_TRUE(store.row(0).frame.identifier == 0x106);
    EXPECT_TRUE(store.row(3).frame.identifier == 0x109);
}

TEST(TraceStoreTests, TheRingSurvivesManyWraps)
{
    TraceStore store{8};

    for (std::uint32_t index = 0; index < 1000; ++index) {
        const CanFrame single = frame(0x100 + index, (index + 1) * 1000);
        store.append(std::span<const CanFrame>{&single, 1});
    }

    EXPECT_TRUE(store.size() == 8);
    EXPECT_TRUE(store.totalAppended() == 1000);
    EXPECT_TRUE(store.discarded() == 992);
    EXPECT_TRUE(store.row(7).frame.identifier == 0x100 + 999);
    EXPECT_TRUE(store.row(0).frame.identifier == 0x100 + 992);
}

TEST(TraceStoreTests, DeltaIsTheGapSinceThePreviousFrameOnAnyChannel)
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1'000'000), // 1 ms
        frame(0x200, 1'500'000), // +500 us
        frame(0x100, 3'000'000), // +1500 us
    };
    store.append(batch);

    EXPECT_TRUE(store.row(0).deltaUs == 0); // nothing to measure against
    EXPECT_TRUE(store.row(1).deltaUs == 500);
    EXPECT_TRUE(store.row(2).deltaUs == 1500);
}

TEST(TraceStoreTests, CycleIsTheGapSinceTheSameIdentifierOnTheSameChannel)
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
    EXPECT_TRUE(store.row(0).cycleUs == 0);
    EXPECT_TRUE(store.row(1).cycleUs == 0);
    EXPECT_TRUE(store.row(2).cycleUs == 2000);
    EXPECT_TRUE(store.row(3).cycleUs == 2000);
}

TEST(TraceStoreTests, TheSameIdentifierOnTwoChannelsIsTwoIdentifiers)
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

    EXPECT_TRUE(store.row(1).cycleUs == 0); // first sighting on channel 1
    EXPECT_TRUE(store.row(2).cycleUs == 2000); // measured against channel 0 only
    EXPECT_TRUE(store.identifiers().size() == 2);
}

TEST(TraceStoreTests, OccurrenceCountsSightingsOfOneIdentifier)
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 1000),
        frame(0x200, 2000),
        frame(0x100, 3000),
        frame(0x100, 4000),
    };
    store.append(batch);

    EXPECT_TRUE(store.row(0).occurrence == 1);
    EXPECT_TRUE(store.row(1).occurrence == 1);
    EXPECT_TRUE(store.row(2).occurrence == 2);
    EXPECT_TRUE(store.row(3).occurrence == 3);
}

TEST(TraceStoreTests, TheIdentifierIndexKeepsFirstSeenOrder)
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

    ASSERT_TRUE(store.identifiers().size() == 3);
    EXPECT_TRUE(store.identifiers()[0].identifier == 0x300);
    EXPECT_TRUE(store.identifiers()[1].identifier == 0x100);
    EXPECT_TRUE(store.identifiers()[2].identifier == 0x200);
    EXPECT_TRUE(store.identifiers()[1].count == 2);
}

TEST(TraceStoreTests, MinAndMaxCycleTrackTheSpread)
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{
        frame(0x100, 0),
        frame(0x100, 1'000'000), // 1000 us
        frame(0x100, 6'000'000), // 5000 us
        frame(0x100, 8'000'000), // 2000 us
    };
    store.append(batch);

    ASSERT_TRUE(store.identifiers().size() == 1);
    const TraceIdentifierStats& stats = store.identifiers().front();

    EXPECT_TRUE(stats.count == 4);
    EXPECT_TRUE(stats.lastCycleUs == 2000);
    EXPECT_TRUE(stats.minCycleUs == 1000);
    EXPECT_TRUE(stats.maxCycleUs == 5000);
}

TEST(TraceStoreTests, ChangedBytesAreFlaggedBetweenConsecutiveFrames)
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

    ASSERT_TRUE(store.identifiers().size() == 1);
    EXPECT_TRUE(store.identifiers().front().changedBytes == (std::uint64_t{1} << 1U));
}

TEST(TraceStoreTests, ATimestampThatGoesBackwardsDoesNotProduceAHugeDelta)
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

    EXPECT_TRUE(store.row(1).deltaUs == 0);
}

TEST(TraceStoreTests, ClearEmptiesEverythingIncludingTheIdentifierIndex)
{
    TraceStore store{100};

    const std::vector<CanFrame> batch{frame(0x100, 1000), frame(0x200, 2000)};
    store.append(batch);
    ASSERT_TRUE(store.size() == 2);

    store.clear();

    EXPECT_TRUE(store.empty());
    EXPECT_TRUE(store.totalAppended() == 0);
    EXPECT_TRUE(store.discarded() == 0);
    EXPECT_TRUE(store.identifiers().empty());
    EXPECT_TRUE(store.capacity() == 100); // the allocation is kept

    // And a fresh append starts cleanly rather than continuing the old counts.
    store.append(batch);
    EXPECT_TRUE(store.row(0).occurrence == 1);
    EXPECT_TRUE(store.row(0).deltaUs == 0);
}

TEST(TraceStoreTests, ExtendedAndStandardIdentifiersWithTheSameValueAreDistinct)
{
    TraceStore store{100};

    CanFrame standard = frame(0x100, 1000);
    standard.format = CanFrameFormat::Standard;

    CanFrame extended = frame(0x100, 2000);
    extended.format = CanFrameFormat::Extended;

    store.append(std::span<const CanFrame>{&standard, 1});
    store.append(std::span<const CanFrame>{&extended, 1});

    EXPECT_TRUE(store.identifiers().size() == 2);
    EXPECT_TRUE(store.row(1).cycleUs == 0); // not measured against the standard one
}
