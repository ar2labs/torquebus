// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The transmit list is the first thing in this project that puts frames on a
// bus because a person asked it to, rather than because a script or a driver
// did. That makes two of its properties safety properties rather than
// conveniences:
//
//   - a list must not start transmitting on its own, and
//   - a late pass must not turn into a burst.
//
// Both have a test here, and both are the kind of thing that looks like a
// detail until it is a vehicle on a bench.
//
// `collectDue` takes the clock as an argument rather than reading one, which is
// what makes any of this testable at all: a test that had to sleep for two
// periods to check a period would be slow and would still be flaky.

#include <gtest/gtest.h>

#include "core/transmit/TransmitList.h"

#include <cstdint>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] TransmitEntry periodic(std::uint32_t identifier, std::uint32_t cycleMs)
{
    TransmitEntry entry;
    entry.name = "test";
    entry.frame.identifier = identifier;
    entry.frame.length = 1;
    entry.frame.data[0] = 0x42;
    entry.trigger = TransmitTrigger::Periodic;
    entry.cycleMs = cycleMs;
    return entry;
}

[[nodiscard]] TransmitEntry manual(std::uint32_t identifier)
{
    TransmitEntry entry = periodic(identifier, 100);
    entry.trigger = TransmitTrigger::Manual;
    return entry;
}

/// collectDue for channel 0, which is what most of these cases care about.
void collect(TransmitList& list, std::uint64_t nowUs, std::vector<CanFrame>& out)
{
    list.collectDue(nowUs, 0, out);
}

/// Milliseconds, in the microseconds collectDue speaks.
[[nodiscard]] constexpr std::uint64_t ms(std::uint64_t value)
{
    return value * 1000ULL;
}

} // namespace

TEST(TransmitListTests, AManualEntryNeverSendsByItself)
{
    // The safety property. A transmit list is filled in while connected to
    // something real, and a list that starts transmitting the moment a row is
    // typed puts traffic on a vehicle nobody was ready for.
    TransmitList list;
    (void)list.add(manual(0x100));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    collect(list, ms(1000), out);
    collect(list, ms(100000), out);

    EXPECT_TRUE(out.empty());
}

TEST(TransmitListTests, APeriodicEntrySendsImmediatelyAndThenOnItsPeriod)
{
    TransmitList list;
    (void)list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;

    // Straight away, not one period from now. Waiting a second before the first
    // frame of a 1 Hz message looks exactly like a list that is not working.
    collect(list, ms(0), out);
    ASSERT_TRUE(out.size() == 1);
    EXPECT_TRUE(out.front().identifier == 0x100);

    // Not yet.
    collect(list, ms(50), out);
    EXPECT_TRUE(out.size() == 1);

    collect(list, ms(100), out);
    EXPECT_TRUE(out.size() == 2);

    collect(list, ms(199), out);
    EXPECT_TRUE(out.size() == 2);

    collect(list, ms(200), out);
    EXPECT_TRUE(out.size() == 3);
}

TEST(TransmitListTests, ALatePassSendsOnceNotABurstToCatchUp)
{
    // The other safety property, and the one that is tempting to get wrong.
    //
    // Advancing the schedule by adding a period to the previous due time is the
    // textbook way to avoid drift. It is wrong here: a pass that arrives a
    // second late - a saturated bus, a window being dragged - would leave the
    // entry owing ten sends, and the list would fire them back to back. A burst
    // of a message meant to arrive every 100 ms is worse for whatever receives
    // it than the drift ever was.
    TransmitList list;
    (void)list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    ASSERT_TRUE(out.size() == 1);

    // A whole second with no dispatch at all.
    collect(list, ms(1000), out);
    EXPECT_TRUE(out.size() == 2);

    // And the period restarts from when it actually went out, not from when it
    // was owed.
    collect(list, ms(1050), out);
    EXPECT_TRUE(out.size() == 2);

    collect(list, ms(1100), out);
    EXPECT_TRUE(out.size() == 3);
}

TEST(TransmitListTests, ADisabledEntryStopsAndReEnablingStartsThePeriodAgain)
{
    // Turning one row off to see what changes is the most common thing anyone
    // does with a transmit list, which is why this is not just "delete the row".
    TransmitList list;
    const std::size_t index = list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    ASSERT_TRUE(out.size() == 1);

    list.setEnabled(index, false);
    collect(list, ms(100), out);
    collect(list, ms(200), out);
    EXPECT_TRUE(out.size() == 1);

    // Back on: due now, rather than counting the time it spent off as elapsed.
    list.setEnabled(index, true);
    collect(list, ms(250), out);
    EXPECT_TRUE(out.size() == 2);
}

TEST(TransmitListTests, AOneShotGoesOutOnTheNextPassWhateverTheScheduleSays)
{
    TransmitList list;
    const std::size_t index = list.add(manual(0x200));

    EXPECT_TRUE(list.sendOnce(index));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);

    ASSERT_TRUE(out.size() == 1);
    EXPECT_TRUE(out.front().identifier == 0x200);

    // Once, not once per pass.
    collect(list, ms(10), out);
    EXPECT_TRUE(out.size() == 1);
}

TEST(TransmitListTests, AOneShotOnADisabledRowIsRefused)
{
    // Disabled means disabled. A send button that works on a row the user
    // switched off is a button that does something they turned off.
    TransmitList list;
    const std::size_t index = list.add(manual(0x200));
    list.setEnabled(index, false);

    EXPECT_FALSE(list.sendOnce(index));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    EXPECT_TRUE(out.empty());
}

TEST(TransmitListTests, EditingARowKeepsTheCountersTheRunHasAccumulated)
{
    // The panel reads a row, the user changes a byte, the panel writes it back.
    // Without this, sentCount rewinds to whatever it was when the editor
    // opened - so a periodic row would appear to stop counting every time
    // somebody looked at it.
    TransmitList list;
    const std::size_t index = list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    collect(list, ms(100), out);

    TransmitEntry edited;
    ASSERT_TRUE(list.entryAt(index, edited));
    ASSERT_TRUE(edited.sentCount == 2);

    edited.sentCount = 0; // as a stale copy from before the sends would have it
    edited.frame.data[0] = 0x99;
    list.update(index, edited);

    TransmitEntry after;
    ASSERT_TRUE(list.entryAt(index, after));
    EXPECT_TRUE(after.sentCount == 2);
    EXPECT_TRUE(after.frame.data[0] == 0x99);
}

TEST(TransmitListTests, RestartingAMeasurementDoesNotFireEveryRowAtOnce)
{
    // A list that ran yesterday would otherwise consider all of its rows
    // overdue on the first pass of today's run.
    TransmitList list;
    (void)list.add(periodic(0x100, 100));
    (void)list.add(periodic(0x101, 500));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    collect(list, ms(2000), out);
    ASSERT_TRUE(out.size() == 4);

    list.restartSchedule();
    out.clear();

    // Due at zero on the new clock, once each - not once for every period that
    // elapsed while nothing was running.
    collect(list, ms(0), out);
    EXPECT_TRUE(out.size() == 2);

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_TRUE(entry.sentCount == 1);
}

TEST(TransmitListTests, APeriodBelowTheDispatchLoopSResolutionIsClamped)
{
    // Zero would mean "every pass", which is not a period at all: the list
    // would be promising a rate the loop cannot hold, and the trace would show
    // it not holding it.
    TransmitList list;
    (void)list.add(periodic(0x100, 0));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    EXPECT_TRUE(out.size() == 1);

    // Still one full millisecond apart rather than one per call.
    collect(list, 500, out);
    EXPECT_TRUE(out.size() == 1);

    collect(list, ms(1), out);
    EXPECT_TRUE(out.size() == 2);
}

TEST(TransmitListTests, RemovingARowDoesNotShiftAnotherRowSSchedule)
{
    // The schedule is a parallel array. Erasing from one and forgetting the
    // other is the classic way to make row three start keeping row two's time.
    TransmitList list;
    (void)list.add(periodic(0x100, 100));
    (void)list.add(periodic(0x101, 1000));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    ASSERT_TRUE(out.size() == 2);

    list.remove(0);
    out.clear();

    // 0x101 is on a 1000 ms period and went out at zero, so it is not due yet.
    collect(list, ms(500), out);
    EXPECT_TRUE(out.empty());

    collect(list, ms(1000), out);
    ASSERT_TRUE(out.size() == 1);
    EXPECT_TRUE(out.front().identifier == 0x101);
}

TEST(TransmitListTests, ARowGoesOutOnTheChannelItNamesAndOnNoOther)
{
    // One list serves every bus. Each channel's node collects only the rows
    // addressed to it, which is what stops a row going out twice - once per
    // channel - or on a bus the user did not choose.
    TransmitList list;

    TransmitEntry first = periodic(0x100, 100);
    first.channel = 0;
    (void)list.add(std::move(first));

    TransmitEntry second = periodic(0x200, 100);
    second.channel = 1;
    (void)list.add(std::move(second));

    std::vector<CanFrame> channelZero;
    list.collectDue(ms(0), 0, channelZero);

    ASSERT_TRUE(channelZero.size() == 1);
    EXPECT_TRUE(channelZero.front().identifier == 0x100);

    // And the frame carries the channel, so whatever transmits it and whatever
    // shows it in a trace agree about which bus it was on.
    EXPECT_TRUE(channelZero.front().channel == 0);

    std::vector<CanFrame> channelOne;
    list.collectDue(ms(0), 1, channelOne);

    ASSERT_TRUE(channelOne.size() == 1);
    EXPECT_TRUE(channelOne.front().identifier == 0x200);
    EXPECT_TRUE(channelOne.front().channel == 1);
}

TEST(TransmitListTests, AOneShotWaitsForItsOwnChannelSPass)
{
    // The case that broke the original design. One-shots were a
    // single-producer single-consumer queue, and one node per channel means
    // several consumers - so whichever ran first would drain a frame meant for
    // another bus and send it on its own.
    TransmitList list;

    TransmitEntry entry = manual(0x300);
    entry.channel = 1;
    const std::size_t index = list.add(std::move(entry));

    EXPECT_TRUE(list.sendOnce(index));

    // Channel 0 runs first and must leave it alone.
    std::vector<CanFrame> channelZero;
    list.collectDue(ms(0), 0, channelZero);
    EXPECT_TRUE(channelZero.empty());

    std::vector<CanFrame> channelOne;
    list.collectDue(ms(0), 1, channelOne);
    ASSERT_TRUE(channelOne.size() == 1);
    EXPECT_TRUE(channelOne.front().identifier == 0x300);
}

TEST(TransmitListTests, APressMadeWhileStoppedDoesNotFireWhenTheRunStarts)
{
    // Send is pressed, nothing is running, the frame waits. Ten minutes later
    // somebody presses Start - and a press from ten minutes ago is not a
    // request to transmit now.
    TransmitList list;
    const std::size_t index = list.add(manual(0x400));

    EXPECT_TRUE(list.sendOnce(index));

    list.restartSchedule();

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    EXPECT_TRUE(out.empty());
}

TEST(TransmitListTests, SwitchingARowOnByEditingItStartsItNowNotAtItsOldDueTime)
{
    // The panel edits a row through update(), not setEnabled(). A row that
    // ran, was switched off and was switched on again from its checkbox must
    // go out at once, like one switched on through setEnabled().
    TransmitList list;
    const std::size_t index = list.add(periodic(0x500, 1000));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    ASSERT_EQ(out.size(), 1U);

    TransmitEntry edited;
    ASSERT_TRUE(list.entryAt(index, edited));
    edited.enabled = false;
    list.update(index, edited);

    edited.enabled = true;
    list.update(index, edited);

    out.clear();
    collect(list, ms(10), out);
    EXPECT_EQ(out.size(), 1U);
}

TEST(TransmitListTests, ShorteningACycleDoesNotWaitOutTheOldOne)
{
    TransmitList list;
    const std::size_t index = list.add(periodic(0x501, 10000));

    std::vector<CanFrame> out;
    collect(list, ms(0), out);
    ASSERT_EQ(out.size(), 1U);

    TransmitEntry edited;
    ASSERT_TRUE(list.entryAt(index, edited));
    edited.cycleMs = 100;
    list.update(index, edited);

    out.clear();
    collect(list, ms(150), out);
    EXPECT_EQ(out.size(), 1U);
}

TEST(TransmitListTests, ARowTurnedPeriodicStartsAtOnce)
{
    TransmitList list;
    const std::size_t index = list.add(manual(0x502));

    std::vector<CanFrame> out;
    collect(list, ms(5000), out);
    EXPECT_TRUE(out.empty());

    TransmitEntry edited;
    ASSERT_TRUE(list.entryAt(index, edited));
    edited.trigger = TransmitTrigger::Periodic;
    list.update(index, edited);

    collect(list, ms(5001), out);
    EXPECT_EQ(out.size(), 1U);
}
