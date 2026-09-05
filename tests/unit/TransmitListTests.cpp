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

#include <catch2/catch_test_macros.hpp>

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

/// Milliseconds, in the microseconds collectDue speaks.
[[nodiscard]] constexpr std::uint64_t ms(std::uint64_t value)
{
    return value * 1000ULL;
}

} // namespace

TEST_CASE("A manual entry never sends by itself", "[transmit]")
{
    // The safety property. A transmit list is filled in while connected to
    // something real, and a list that starts transmitting the moment a row is
    // typed puts traffic on a vehicle nobody was ready for.
    TransmitList list;
    (void)list.add(manual(0x100));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    list.collectDue(ms(1000), out);
    list.collectDue(ms(100000), out);

    CHECK(out.empty());
}

TEST_CASE("A periodic entry sends immediately and then on its period",
          "[transmit]")
{
    TransmitList list;
    (void)list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;

    // Straight away, not one period from now. Waiting a second before the first
    // frame of a 1 Hz message looks exactly like a list that is not working.
    list.collectDue(ms(0), out);
    REQUIRE(out.size() == 1);
    CHECK(out.front().identifier == 0x100);

    // Not yet.
    list.collectDue(ms(50), out);
    CHECK(out.size() == 1);

    list.collectDue(ms(100), out);
    CHECK(out.size() == 2);

    list.collectDue(ms(199), out);
    CHECK(out.size() == 2);

    list.collectDue(ms(200), out);
    CHECK(out.size() == 3);
}

TEST_CASE("A late pass sends once, not a burst to catch up", "[transmit]")
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
    list.collectDue(ms(0), out);
    REQUIRE(out.size() == 1);

    // A whole second with no dispatch at all.
    list.collectDue(ms(1000), out);
    CHECK(out.size() == 2);

    // And the period restarts from when it actually went out, not from when it
    // was owed.
    list.collectDue(ms(1050), out);
    CHECK(out.size() == 2);

    list.collectDue(ms(1100), out);
    CHECK(out.size() == 3);
}

TEST_CASE("A disabled entry stops, and re-enabling starts the period again",
          "[transmit]")
{
    // Turning one row off to see what changes is the most common thing anyone
    // does with a transmit list, which is why this is not just "delete the row".
    TransmitList list;
    const std::size_t index = list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    REQUIRE(out.size() == 1);

    list.setEnabled(index, false);
    list.collectDue(ms(100), out);
    list.collectDue(ms(200), out);
    CHECK(out.size() == 1);

    // Back on: due now, rather than counting the time it spent off as elapsed.
    list.setEnabled(index, true);
    list.collectDue(ms(250), out);
    CHECK(out.size() == 2);
}

TEST_CASE("A one-shot goes out on the next pass whatever the schedule says",
          "[transmit]")
{
    TransmitList list;
    const std::size_t index = list.add(manual(0x200));

    CHECK(list.sendOnce(index));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);

    REQUIRE(out.size() == 1);
    CHECK(out.front().identifier == 0x200);

    // Once, not once per pass.
    list.collectDue(ms(10), out);
    CHECK(out.size() == 1);
}

TEST_CASE("A one-shot on a disabled row is refused", "[transmit]")
{
    // Disabled means disabled. A send button that works on a row the user
    // switched off is a button that does something they turned off.
    TransmitList list;
    const std::size_t index = list.add(manual(0x200));
    list.setEnabled(index, false);

    CHECK_FALSE(list.sendOnce(index));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    CHECK(out.empty());
}

TEST_CASE("Editing a row keeps the counters the run has accumulated",
          "[transmit]")
{
    // The panel reads a row, the user changes a byte, the panel writes it back.
    // Without this, sentCount rewinds to whatever it was when the editor
    // opened - so a periodic row would appear to stop counting every time
    // somebody looked at it.
    TransmitList list;
    const std::size_t index = list.add(periodic(0x100, 100));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    list.collectDue(ms(100), out);

    TransmitEntry edited;
    REQUIRE(list.entryAt(index, edited));
    REQUIRE(edited.sentCount == 2);

    edited.sentCount = 0; // as a stale copy from before the sends would have it
    edited.frame.data[0] = 0x99;
    list.update(index, edited);

    TransmitEntry after;
    REQUIRE(list.entryAt(index, after));
    CHECK(after.sentCount == 2);
    CHECK(after.frame.data[0] == 0x99);
}

TEST_CASE("Restarting a measurement does not fire every row at once",
          "[transmit]")
{
    // A list that ran yesterday would otherwise consider all of its rows
    // overdue on the first pass of today's run.
    TransmitList list;
    (void)list.add(periodic(0x100, 100));
    (void)list.add(periodic(0x101, 500));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    list.collectDue(ms(2000), out);
    REQUIRE(out.size() == 4);

    list.restartSchedule();
    out.clear();

    // Due at zero on the new clock, once each - not once for every period that
    // elapsed while nothing was running.
    list.collectDue(ms(0), out);
    CHECK(out.size() == 2);

    TransmitEntry entry;
    REQUIRE(list.entryAt(0, entry));
    CHECK(entry.sentCount == 1);
}

TEST_CASE("A period below the dispatch loop's resolution is clamped",
          "[transmit]")
{
    // Zero would mean "every pass", which is not a period at all: the list
    // would be promising a rate the loop cannot hold, and the trace would show
    // it not holding it.
    TransmitList list;
    (void)list.add(periodic(0x100, 0));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    CHECK(out.size() == 1);

    // Still one full millisecond apart rather than one per call.
    list.collectDue(500, out);
    CHECK(out.size() == 1);

    list.collectDue(ms(1), out);
    CHECK(out.size() == 2);
}

TEST_CASE("Removing a row does not shift another row's schedule", "[transmit]")
{
    // The schedule is a parallel array. Erasing from one and forgetting the
    // other is the classic way to make row three start keeping row two's time.
    TransmitList list;
    (void)list.add(periodic(0x100, 100));
    (void)list.add(periodic(0x101, 1000));

    std::vector<CanFrame> out;
    list.collectDue(ms(0), out);
    REQUIRE(out.size() == 2);

    list.remove(0);
    out.clear();

    // 0x101 is on a 1000 ms period and went out at zero, so it is not due yet.
    list.collectDue(ms(500), out);
    CHECK(out.empty());

    list.collectDue(ms(1000), out);
    REQUIRE(out.size() == 1);
    CHECK(out.front().identifier == 0x101);
}
