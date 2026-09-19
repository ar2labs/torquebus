// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFilter.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace torquebus;

namespace {

CanFrame frame(std::uint32_t identifier,
               CanFrameFormat format = CanFrameFormat::Standard,
               CanDirection direction = CanDirection::Rx,
               std::uint8_t channel = 0)
{
    CanFrame result;
    result.identifier = identifier;
    result.format = format;
    result.direction = direction;
    result.channel = channel;
    result.length = 8;
    result.dlc = 8;
    return result;
}

} // namespace

TEST_CASE("An unconfigured filter set passes everything", "[filter]")
{
    // A filter nobody configured must never be the reason a bus looks dead.
    const CanFilterSet filters;

    CHECK(filters.empty());
    CHECK(filters.accepts(frame(0x000)));
    CHECK(filters.accepts(frame(0x7FF)));
    CHECK(filters.accepts(frame(0x18FF50E5, CanFrameFormat::Extended)));
}

TEST_CASE("A single accept rule turns the set into an allow-list", "[filter]")
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptIdentifier(0x100));

    CHECK(filters.accepts(frame(0x100)));
    CHECK_FALSE(filters.accepts(frame(0x101)));
    CHECK_FALSE(filters.accepts(frame(0x0FF)));
}

TEST_CASE("Accept rules accumulate", "[filter]")
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptIdentifier(0x100));
    filters.add(CanFilter::acceptIdentifier(0x200));
    filters.add(CanFilter::acceptRange(0x300, 0x30F));

    CHECK(filters.accepts(frame(0x100)));
    CHECK(filters.accepts(frame(0x200)));
    CHECK(filters.accepts(frame(0x300)));
    CHECK(filters.accepts(frame(0x30F)));
    CHECK_FALSE(filters.accepts(frame(0x310)));
    CHECK_FALSE(filters.accepts(frame(0x150)));
}

TEST_CASE("A reject rule alone blocks only what it names", "[filter]")
{
    // "Everything except 0x7DF" must be one rule, not a hand-built allow-list
    // of two thousand identifiers.
    CanFilterSet filters;
    filters.add(CanFilter::rejectIdentifier(0x7DF));

    CHECK_FALSE(filters.accepts(frame(0x7DF)));
    CHECK(filters.accepts(frame(0x7E0)));
    CHECK(filters.accepts(frame(0x100)));
}

TEST_CASE("A reject rule wins over an accept rule", "[filter]")
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptRange(0x700, 0x7FF));
    filters.add(CanFilter::rejectIdentifier(0x7DF));

    CHECK(filters.accepts(frame(0x700)));
    CHECK(filters.accepts(frame(0x7E0)));
    CHECK_FALSE(filters.accepts(frame(0x7DF)));
    CHECK_FALSE(filters.accepts(frame(0x100))); // outside the allow-list
}

TEST_CASE("Rule order does not change the outcome", "[filter]")
{
    // Reject must win whether it was added before or after the accept rule -
    // otherwise the same project would behave differently depending on the
    // order the user happened to click.
    CanFilterSet rejectLast;
    rejectLast.add(CanFilter::acceptRange(0x700, 0x7FF));
    rejectLast.add(CanFilter::rejectIdentifier(0x7DF));

    CanFilterSet rejectFirst;
    rejectFirst.add(CanFilter::rejectIdentifier(0x7DF));
    rejectFirst.add(CanFilter::acceptRange(0x700, 0x7FF));

    for (const std::uint32_t identifier : {0x700U, 0x7DFU, 0x7E0U, 0x100U}) {
        INFO("identifier = " << identifier);
        CHECK(rejectLast.accepts(frame(identifier)) == rejectFirst.accepts(frame(identifier)));
    }
}

TEST_CASE("Mask matching expresses J1939-style filtering", "[filter][mask]")
{
    // Accept every PGN 0xFF50 message regardless of source address: the low
    // byte of a J1939 identifier is the sender, and we do not care who sent it.
    CanFilterSet filters;
    filters.add(CanFilter::acceptMask(0x00FFFF00, 0x00FF5000));

    CHECK(filters.accepts(frame(0x18FF5000, CanFrameFormat::Extended)));
    CHECK(filters.accepts(frame(0x18FF50E5, CanFrameFormat::Extended)));
    CHECK(filters.accepts(frame(0x1CFF50AB, CanFrameFormat::Extended)));
    CHECK_FALSE(filters.accepts(frame(0x18FF5100, CanFrameFormat::Extended)));
    CHECK_FALSE(filters.accepts(frame(0x0CF00400, CanFrameFormat::Extended)));
}

TEST_CASE("Rules can constrain the channel", "[filter][channel]")
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptChannel(1));

    CHECK_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 0)));
    CHECK(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 1)));
    CHECK_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 2)));
}

TEST_CASE("Rules can constrain format and direction", "[filter]")
{
    SECTION("extended only")
    {
        CanFilter rule;
        rule.format = CanFormatMatch::ExtendedOnly;

        CanFilterSet filters;
        filters.add(rule);

        CHECK(filters.accepts(frame(0x100, CanFrameFormat::Extended)));
        CHECK_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard)));
    }

    SECTION("transmitted only")
    {
        CanFilter rule;
        rule.direction = CanDirectionMatch::TxOnly;

        CanFilterSet filters;
        filters.add(rule);

        CHECK(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Tx)));
        CHECK_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx)));
    }
}

TEST_CASE("A disabled rule is kept but ignored", "[filter]")
{
    // Toggling a rule off must not destroy its configuration - the user is
    // experimenting, not deleting.
    CanFilter rule = CanFilter::acceptIdentifier(0x100);
    rule.enabled = false;

    CanFilterSet filters;
    filters.add(rule);

    CHECK(filters.size() == 1);

    // With every rule disabled, the set constrains nothing.
    CHECK(filters.accepts(frame(0x100)));
    CHECK(filters.accepts(frame(0x999)));

    filters.filters().front().enabled = true;
    CHECK(filters.accepts(frame(0x100)));
    CHECK_FALSE(filters.accepts(frame(0x999)));
}

TEST_CASE("Error and remote frames can be excluded", "[filter]")
{
    CanFilter rule;
    rule.matchErrorFrames = false;

    CanFilterSet filters;
    filters.add(rule);

    CanFrame errorFrame = frame(0x100);
    errorFrame.error = true;

    CHECK(filters.accepts(frame(0x100)));
    CHECK_FALSE(filters.accepts(errorFrame));
}

TEST_CASE("retainAccepted compacts a batch in place", "[filter][batch]")
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptRange(0x100, 0x1FF));

    std::vector<CanFrame> batch{
        frame(0x0FF),
        frame(0x100),
        frame(0x200),
        frame(0x150),
        frame(0x300),
        frame(0x1FF),
    };

    const std::size_t kept = filters.retainAccepted(batch);

    REQUIRE(kept == 3);
    CHECK(batch[0].identifier == 0x100);
    CHECK(batch[1].identifier == 0x150);
    CHECK(batch[2].identifier == 0x1FF);
}

TEST_CASE("retainAccepted on an unfiltered set keeps everything", "[filter][batch]")
{
    const CanFilterSet filters;

    std::vector<CanFrame> batch{frame(0x100), frame(0x200), frame(0x300)};

    CHECK(filters.retainAccepted(batch) == 3);
    CHECK(batch[0].identifier == 0x100);
    CHECK(batch[2].identifier == 0x300);
}
