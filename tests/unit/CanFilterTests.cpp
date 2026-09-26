// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFilter.h"

#include <gtest/gtest.h>

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

TEST(CanFilterTests, AnUnconfiguredFilterSetPassesEverything)
{
    // A filter nobody configured must never be the reason a bus looks dead.
    const CanFilterSet filters;

    EXPECT_TRUE(filters.empty());
    EXPECT_TRUE(filters.accepts(frame(0x000)));
    EXPECT_TRUE(filters.accepts(frame(0x7FF)));
    EXPECT_TRUE(filters.accepts(frame(0x18FF50E5, CanFrameFormat::Extended)));
}

TEST(CanFilterTests, ASingleAcceptRuleTurnsTheSetIntoAnAllowList)
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptIdentifier(0x100));

    EXPECT_TRUE(filters.accepts(frame(0x100)));
    EXPECT_FALSE(filters.accepts(frame(0x101)));
    EXPECT_FALSE(filters.accepts(frame(0x0FF)));
}

TEST(CanFilterTests, AcceptRulesAccumulate)
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptIdentifier(0x100));
    filters.add(CanFilter::acceptIdentifier(0x200));
    filters.add(CanFilter::acceptRange(0x300, 0x30F));

    EXPECT_TRUE(filters.accepts(frame(0x100)));
    EXPECT_TRUE(filters.accepts(frame(0x200)));
    EXPECT_TRUE(filters.accepts(frame(0x300)));
    EXPECT_TRUE(filters.accepts(frame(0x30F)));
    EXPECT_FALSE(filters.accepts(frame(0x310)));
    EXPECT_FALSE(filters.accepts(frame(0x150)));
}

TEST(CanFilterTests, ARejectRuleAloneBlocksOnlyWhatItNames)
{
    // "Everything except 0x7DF" must be one rule, not a hand-built allow-list
    // of two thousand identifiers.
    CanFilterSet filters;
    filters.add(CanFilter::rejectIdentifier(0x7DF));

    EXPECT_FALSE(filters.accepts(frame(0x7DF)));
    EXPECT_TRUE(filters.accepts(frame(0x7E0)));
    EXPECT_TRUE(filters.accepts(frame(0x100)));
}

TEST(CanFilterTests, ARejectRuleWinsOverAnAcceptRule)
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptRange(0x700, 0x7FF));
    filters.add(CanFilter::rejectIdentifier(0x7DF));

    EXPECT_TRUE(filters.accepts(frame(0x700)));
    EXPECT_TRUE(filters.accepts(frame(0x7E0)));
    EXPECT_FALSE(filters.accepts(frame(0x7DF)));
    EXPECT_FALSE(filters.accepts(frame(0x100))); // outside the allow-list
}

TEST(CanFilterTests, RuleOrderDoesNotChangeTheOutcome)
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
        SCOPED_TRACE(::testing::Message() << "identifier = " << identifier);
        EXPECT_TRUE(rejectLast.accepts(frame(identifier))
                    == rejectFirst.accepts(frame(identifier)));
    }
}

TEST(CanFilterTests, MaskMatchingExpressesJ1939StyleFiltering)
{
    // Accept every PGN 0xFF50 message regardless of source address: the low
    // byte of a J1939 identifier is the sender, and we do not care who sent it.
    CanFilterSet filters;
    filters.add(CanFilter::acceptMask(0x00FFFF00, 0x00FF5000));

    EXPECT_TRUE(filters.accepts(frame(0x18FF5000, CanFrameFormat::Extended)));
    EXPECT_TRUE(filters.accepts(frame(0x18FF50E5, CanFrameFormat::Extended)));
    EXPECT_TRUE(filters.accepts(frame(0x1CFF50AB, CanFrameFormat::Extended)));
    EXPECT_FALSE(filters.accepts(frame(0x18FF5100, CanFrameFormat::Extended)));
    EXPECT_FALSE(filters.accepts(frame(0x0CF00400, CanFrameFormat::Extended)));
}

TEST(CanFilterTests, RulesCanConstrainTheChannel)
{
    CanFilterSet filters;
    filters.add(CanFilter::acceptChannel(1));

    EXPECT_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 0)));
    EXPECT_TRUE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 1)));
    EXPECT_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx, 2)));
}

TEST(CanFilterTests, RulesCanConstrainFormatAndDirection)
{
    {
        CanFilter rule;
        rule.format = CanFormatMatch::ExtendedOnly;

        CanFilterSet filters;
        filters.add(rule);

        EXPECT_TRUE(filters.accepts(frame(0x100, CanFrameFormat::Extended)));
        EXPECT_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard)));
    }
    {
        CanFilter rule;
        rule.direction = CanDirectionMatch::TxOnly;

        CanFilterSet filters;
        filters.add(rule);

        EXPECT_TRUE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Tx)));
        EXPECT_FALSE(filters.accepts(frame(0x100, CanFrameFormat::Standard, CanDirection::Rx)));
    }
}

TEST(CanFilterTests, ADisabledRuleIsKeptButIgnored)
{
    // Toggling a rule off must not destroy its configuration - the user is
    // experimenting, not deleting.
    CanFilter rule = CanFilter::acceptIdentifier(0x100);
    rule.enabled = false;

    CanFilterSet filters;
    filters.add(rule);

    EXPECT_TRUE(filters.size() == 1);

    // With every rule disabled, the set constrains nothing.
    EXPECT_TRUE(filters.accepts(frame(0x100)));
    EXPECT_TRUE(filters.accepts(frame(0x999)));

    filters.filters().front().enabled = true;
    EXPECT_TRUE(filters.accepts(frame(0x100)));
    EXPECT_FALSE(filters.accepts(frame(0x999)));
}

TEST(CanFilterTests, ErrorAndRemoteFramesCanBeExcluded)
{
    CanFilter rule;
    rule.matchErrorFrames = false;

    CanFilterSet filters;
    filters.add(rule);

    CanFrame errorFrame = frame(0x100);
    errorFrame.error = true;

    EXPECT_TRUE(filters.accepts(frame(0x100)));
    EXPECT_FALSE(filters.accepts(errorFrame));
}

TEST(CanFilterTests, RetainAcceptedCompactsABatchInPlace)
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

    ASSERT_TRUE(kept == 3);
    EXPECT_TRUE(batch[0].identifier == 0x100);
    EXPECT_TRUE(batch[1].identifier == 0x150);
    EXPECT_TRUE(batch[2].identifier == 0x1FF);
}

TEST(CanFilterTests, RetainAcceptedOnAnUnfilteredSetKeepsEverything)
{
    const CanFilterSet filters;

    std::vector<CanFrame> batch{frame(0x100), frame(0x200), frame(0x300)};

    EXPECT_TRUE(filters.retainAccepted(batch) == 3);
    EXPECT_TRUE(batch[0].identifier == 0x100);
    EXPECT_TRUE(batch[2].identifier == 0x300);
}
