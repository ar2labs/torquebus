// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Several wires into one: what comes out, in what order, and what happens when more arrives than a
// pass can carry.
//
// The node is the policy the graph refuses to pick by itself, so what is tested is the policy -
// timestamps decide, a tie goes to the lower input, the order of a wire is never changed, a frame
// is not altered, and a pass carries a batch at most - and that nothing is lost without being
// counted. Two cases run a few thousand passes of random traffic, because the bookkeeping that
// decides which frames wait and which are dropped is the part a hand-picked example does not
// exercise.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameMergeNode.h"
#include "core/pipeline/nodes/FrameNodes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

using Wire = std::vector<CanFrame>;
using Ids = std::vector<std::uint32_t>;

/// A frame at `timestamp` on `channel`; its identifier says which one it is, and is what a test
/// compares.
[[nodiscard]] CanFrame
frameAt(std::uint64_t timestamp, std::uint32_t identifier, std::uint8_t channel = 0U)
{
    CanFrame frame;
    frame.timestampNs = timestamp;
    frame.identifier = identifier;
    frame.channel = channel;
    frame.length = 1U;
    frame.dlc = 1U;
    frame.data[0] = static_cast<std::uint8_t>(identifier & 0xFFU);
    return frame;
}

[[nodiscard]] Ids idsOf(std::span<const CanFrame> frames)
{
    Ids ids;
    ids.reserve(frames.size());

    for (const CanFrame& frame : frames) {
        ids.push_back(frame.identifier);
    }

    return ids;
}

/// One merge node and the context that drives it: a pass is the four wires' frames, and what comes
/// back is what the node published - valid until the next pass, as it would be to a real consumer.
class Bench final {
public:
    explicit Bench(std::size_t batch = 16U) { EXPECT_TRUE(node.prepare(batch).succeeded()); }

    FrameMergeNode node;

    std::span<const CanFrame>
    pass(const Wire& a = {}, const Wire& b = {}, const Wire& c = {}, const Wire& d = {})
    {
        const std::array<PortBatch, FrameMergeNode::kInputCount> inputs{
            PortBatch{std::span<const CanFrame>{a}},
            PortBatch{std::span<const CanFrame>{b}},
            PortBatch{std::span<const CanFrame>{c}},
            PortBatch{std::span<const CanFrame>{d}},
        };

        std::array<PortBatch, 1> outputs{};
        NodeContext context{inputs, outputs};
        node.process(context);

        return outputs[0].as<CanFrame>();
    }
};

[[nodiscard]] bool sameFrame(const CanFrame& left, const CanFrame& right)
{
    return left.timestampNs == right.timestampNs && left.identifier == right.identifier
           && left.channel == right.channel && left.dlc == right.dlc && left.length == right.length
           && left.direction == right.direction && left.format == right.format
           && left.fd == right.fd && left.brs == right.brs && left.esi == right.esi
           && left.rtr == right.rtr && left.error == right.error && left.data == right.data;
}

/// A source that is not a channel: it publishes the frames it was given, every pass.
class Emitter final : public IPipelineNode {
public:
    explicit Emitter(Wire frames)
        : m_frames{std::move(frames)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.emitter"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    void process(NodeContext& context) override
    {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_frames});
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames}};

    Wire m_frames;
};

} // namespace

TEST(FrameMergeNodeTests, TwoWiresComeOutInTheOrderOfTheirTimestamps)
{
    Bench bench;

    const Wire a{frameAt(10U, 0xA1U, 0U), frameAt(30U, 0xA2U, 0U), frameAt(50U, 0xA3U, 0U)};
    const Wire b{frameAt(20U, 0xB1U, 1U), frameAt(40U, 0xB2U, 1U), frameAt(60U, 0xB3U, 1U)};

    const std::span<const CanFrame> out = bench.pass(a, b);

    EXPECT_EQ(idsOf(out), (Ids{0xA1U, 0xB1U, 0xA2U, 0xB2U, 0xA3U, 0xB3U}));

    // And each is on the bus it came from: that is what the block downstream reads.
    ASSERT_EQ(out.size(), 6U);
    EXPECT_EQ(out[0].channel, 0U);
    EXPECT_EQ(out[1].channel, 1U);
    EXPECT_EQ(out[4].channel, 0U);
    EXPECT_EQ(out[5].channel, 1U);

    EXPECT_EQ(bench.node.passedFrames(), 6U);
    EXPECT_EQ(bench.node.waitingFrames(), 0U);
    EXPECT_EQ(bench.node.heldOverFrames(), 0U);
    EXPECT_EQ(bench.node.droppedFrames(), 0U);
}

TEST(FrameMergeNodeTests, AnyOfTheFourInputsTakesPartAndNoneIsFavoured)
{
    Bench bench;

    // Only the last two are wired.
    const Wire c{frameAt(1U, 0xC1U), frameAt(4U, 0xC2U)};
    const Wire d{frameAt(2U, 0xD1U), frameAt(3U, 0xD2U)};
    EXPECT_EQ(idsOf(bench.pass({}, {}, c, d)), (Ids{0xC1U, 0xD1U, 0xD2U, 0xC2U}));

    // The first and the last.
    const Wire a{frameAt(5U, 0xA1U)};
    const Wire d2{frameAt(3U, 0xD3U), frameAt(9U, 0xD4U)};
    EXPECT_EQ(idsOf(bench.pass(a, {}, {}, d2)), (Ids{0xD3U, 0xA1U, 0xD4U}));

    // All four at once, written in the order that is not the order of the ports.
    const Wire a4{frameAt(40U, 0xA4U)};
    const Wire b4{frameAt(20U, 0xB4U)};
    const Wire c4{frameAt(30U, 0xC4U)};
    const Wire d4{frameAt(10U, 0xD5U)};
    EXPECT_EQ(idsOf(bench.pass(a4, b4, c4, d4)), (Ids{0xD5U, 0xB4U, 0xC4U, 0xA4U}));
}

TEST(FrameMergeNodeTests, ATieGoesToTheLowerInput)
{
    Bench bench;

    const Wire a{frameAt(5U, 0x01U)};
    const Wire b{frameAt(5U, 0x02U)};
    const Wire c{frameAt(5U, 0x03U)};
    const Wire d{frameAt(5U, 0x04U)};

    EXPECT_EQ(idsOf(bench.pass(a, b, c, d)), (Ids{0x01U, 0x02U, 0x03U, 0x04U}));

    // Not an accident of which wire has more frames, or which has the first: the lower input is
    // first whichever those are.
    const Wire a2{frameAt(5U, 0x11U), frameAt(5U, 0x12U)};
    const Wire d2{frameAt(5U, 0x14U)};
    EXPECT_EQ(idsOf(bench.pass(a2, {}, {}, d2)), (Ids{0x11U, 0x12U, 0x14U}));

    EXPECT_EQ(idsOf(bench.pass({}, b, c, {})), (Ids{0x02U, 0x03U}));
    EXPECT_EQ(idsOf(bench.pass({}, c, b, {})), (Ids{0x03U, 0x02U}));
}

TEST(FrameMergeNodeTests, AWireIsNeverReorderedEvenWhenItsTimestampsAre)
{
    Bench bench;

    // A recording written by something that did not keep its timestamps in order. The merge takes
    // the next frame of a wire and never looks past it, so what a wire said first it still says
    // first.
    const Wire a{frameAt(30U, 0xA1U), frameAt(10U, 0xA2U), frameAt(20U, 0xA3U)};
    const Wire b{frameAt(15U, 0xB1U)};

    EXPECT_EQ(idsOf(bench.pass(a, b)), (Ids{0xB1U, 0xA1U, 0xA2U, 0xA3U}));

    const Wire c{frameAt(10U, 0xC1U), frameAt(5U, 0xC2U)};
    const Wire d{frameAt(7U, 0xD1U)};

    EXPECT_EQ(idsOf(bench.pass({}, {}, c, d)), (Ids{0xD1U, 0xC1U, 0xC2U}));
}

TEST(FrameMergeNodeTests, AFrameComesOutAsItWentInWhateverItSays)
{
    Bench bench;

    CanFrame fd = frameAt(100U, 0x18FECA00U, 3U);
    fd.format = CanFrameFormat::Extended;
    fd.direction = CanDirection::Tx;
    fd.fd = true;
    fd.brs = true;
    fd.esi = true;
    fd.length = 64U;
    fd.dlc = 15U;

    for (std::size_t index = 0U; index < fd.data.size(); ++index) {
        fd.data[index] = static_cast<std::uint8_t>(index * 3U + 1U);
    }

    CanFrame remote = frameAt(90U, 0x123U, 1U);
    remote.rtr = true;

    CanFrame error = frameAt(95U, 0U, 2U);
    error.error = true;

    const Wire a{fd};
    const Wire b{remote, error};

    const std::span<const CanFrame> out = bench.pass(a, b);

    ASSERT_EQ(out.size(), 3U);
    EXPECT_TRUE(sameFrame(out[0], remote));
    EXPECT_TRUE(sameFrame(out[1], error));
    EXPECT_TRUE(sameFrame(out[2], fd));
}

TEST(FrameMergeNodeTests, OneWireGoesOnAsItIsAndIsNotCopied)
{
    Bench bench;

    const Wire c{frameAt(5U, 0xC1U), frameAt(9U, 0xC2U)};

    const std::span<const CanFrame> out = bench.pass({}, {}, c);

    // The same view, which is what costs nothing - for a wire that is not the first, as well.
    EXPECT_EQ(out.data(), c.data());
    EXPECT_EQ(out.size(), c.size());
    EXPECT_EQ(bench.node.passedFrames(), 2U);

    const Wire d{frameAt(1U, 0xD1U)};
    EXPECT_EQ(bench.pass({}, {}, {}, d).data(), d.data());

    // Two wires are not a view of either: they are put in order.
    const std::span<const CanFrame> both = bench.pass(c, d);
    EXPECT_NE(both.data(), c.data());
    EXPECT_NE(both.data(), d.data());
}

TEST(FrameMergeNodeTests, WhatItPublishesIsWhereItWasWhicheverPassItIs)
{
    // process() must not allocate: the buffer a pass publishes into is made once, in prepare(), and
    // is the same one every pass - however many frames, and however many of them waited.
    Bench bench{4U};

    const Wire a{frameAt(1U, 0xA1U), frameAt(3U, 0xA2U), frameAt(5U, 0xA3U)};
    const Wire b{frameAt(2U, 0xB1U), frameAt(4U, 0xB2U), frameAt(6U, 0xB3U)};

    const CanFrame* const first = bench.pass(a, b).data();
    EXPECT_EQ(bench.pass().data(), first); // the two that waited
    EXPECT_EQ(bench.pass(a, b).data(), first);
    EXPECT_EQ(bench.pass(a, b, a, b).data(), first); // more than it can carry
    EXPECT_EQ(bench.pass().data(), first);
}

TEST(FrameMergeNodeTests, NothingInIsNothingOut)
{
    Bench bench;

    EXPECT_TRUE(bench.pass().empty());
    EXPECT_EQ(bench.node.passedFrames(), 0U);

    const Wire a{frameAt(1U, 0xA1U), frameAt(2U, 0xA2U)};
    const Wire b{frameAt(3U, 0xB1U)};
    EXPECT_EQ(bench.pass(a, b).size(), 3U);

    // The pass after it is empty, and does not hand the last one's frames over again.
    EXPECT_TRUE(bench.pass().empty());
    EXPECT_EQ(bench.node.passedFrames(), 3U);
}

TEST(FrameMergeNodeTests, APassCarriesABatchAndWhatDoesNotFitWaitsForTheNext)
{
    Bench bench{4U};

    const Wire a{frameAt(1U, 0xA1U), frameAt(3U, 0xA2U), frameAt(5U, 0xA3U)};
    const Wire b{frameAt(2U, 0xB1U), frameAt(4U, 0xB2U), frameAt(6U, 0xB3U)};

    // Six frames and room for four: the first four by time go on, and the last two wait.
    EXPECT_EQ(idsOf(bench.pass(a, b)), (Ids{0xA1U, 0xB1U, 0xA2U, 0xB2U}));
    EXPECT_EQ(bench.node.waitingFrames(), 2U);
    EXPECT_EQ(bench.node.heldOverFrames(), 2U);
    EXPECT_EQ(bench.node.passedFrames(), 4U);

    // A pass with nothing new still goes on with what waited.
    EXPECT_EQ(idsOf(bench.pass()), (Ids{0xA3U, 0xB3U}));
    EXPECT_EQ(bench.node.waitingFrames(), 0U);
    EXPECT_EQ(bench.node.passedFrames(), 6U);

    // Counted when they first had to wait, and not again for each pass they waited.
    EXPECT_EQ(bench.node.heldOverFrames(), 2U);
    EXPECT_EQ(bench.node.droppedFrames(), 0U);

    EXPECT_TRUE(bench.pass().empty());
}

TEST(FrameMergeNodeTests, WhatWaitedGoesBeforeWhatArrivedLaterAndAfterNothingItWasBefore)
{
    Bench bench{2U};

    const Wire a{frameAt(1U, 0xA1U), frameAt(5U, 0xA2U)};
    const Wire b{frameAt(3U, 0xB1U), frameAt(5U, 0xB2U)};

    // A2 and B2 tie at 5, A2 first, and both wait.
    EXPECT_EQ(idsOf(bench.pass(a, b)), (Ids{0xA1U, 0xB1U}));
    EXPECT_EQ(bench.node.waitingFrames(), 2U);

    // The next pass brings two more at 5, one from the first wire and one from the third: the two
    // that waited are first, in the order they waited in, and the new ones after them, by input.
    const Wire a2{frameAt(5U, 0xA3U)};
    const Wire c2{frameAt(5U, 0xC1U)};

    EXPECT_EQ(idsOf(bench.pass(a2, {}, c2)), (Ids{0xA2U, 0xB2U}));
    EXPECT_EQ(bench.node.waitingFrames(), 2U);

    EXPECT_EQ(idsOf(bench.pass()), (Ids{0xA3U, 0xC1U}));
    EXPECT_EQ(bench.node.droppedFrames(), 0U);
    EXPECT_EQ(bench.node.passedFrames(), 6U);
}

TEST(FrameMergeNodeTests, WhatFitsNeitherThePassNorTheRoomToWaitIsDroppedAndCounted)
{
    // A batch of two: room to wait for three batches' worth less one, which is six.
    Bench bench{2U};

    const auto times = [](std::uint64_t first, std::uint32_t tag) {
        return Wire{frameAt(first, tag + 1U), frameAt(first + 4U, tag + 2U)};
    };

    // Eight frames in the first pass, at 1 .. 8: two go on and six wait.
    EXPECT_EQ(idsOf(bench.pass(
                  times(1U, 0x100U), times(2U, 0x200U), times(3U, 0x300U), times(4U, 0x400U))),
              (Ids{0x101U, 0x201U}));
    EXPECT_EQ(bench.node.waitingFrames(), 6U);
    EXPECT_EQ(bench.node.heldOverFrames(), 6U);
    EXPECT_EQ(bench.node.droppedFrames(), 0U);

    // Eight more at 11 .. 18: with six waiting that is fourteen for two and six. The six that are
    // lost are the latest.
    EXPECT_EQ(idsOf(bench.pass(
                  times(11U, 0x500U), times(12U, 0x600U), times(13U, 0x700U), times(14U, 0x800U))),
              (Ids{0x301U, 0x401U}));
    EXPECT_EQ(bench.node.waitingFrames(), 6U);
    EXPECT_EQ(bench.node.droppedFrames(), 6U);
    EXPECT_EQ(bench.node.heldOverFrames(), 8U);

    // Everything that was in is somewhere: passed, waiting or dropped.
    EXPECT_EQ(bench.node.passedFrames() + bench.node.waitingFrames() + bench.node.droppedFrames(),
              16U);

    // And what waited goes on in order, the ones from the second pass that fitted last of all.
    EXPECT_EQ(idsOf(bench.pass()), (Ids{0x102U, 0x202U}));
    EXPECT_EQ(idsOf(bench.pass()), (Ids{0x302U, 0x402U}));
    EXPECT_EQ(idsOf(bench.pass()), (Ids{0x501U, 0x601U}));
    EXPECT_TRUE(bench.pass().empty());
    EXPECT_EQ(bench.node.waitingFrames(), 0U);
    EXPECT_EQ(bench.node.passedFrames(), 10U);
    EXPECT_EQ(bench.node.droppedFrames(), 6U);
}

TEST(FrameMergeNodeTests, ABatchOfOneStillMergesAndCarries)
{
    // The smallest batch there is - and a request for none is a batch of one, not a node that
    // swallows everything.
    for (const std::size_t asked : {std::size_t{1U}, std::size_t{0U}}) {
        Bench bench{asked};

        const Wire a{frameAt(1U, 0xA1U), frameAt(4U, 0xA2U)};
        const Wire b{frameAt(2U, 0xB1U), frameAt(3U, 0xB2U)};

        EXPECT_EQ(idsOf(bench.pass(a, b)), (Ids{0xA1U})) << asked;
        EXPECT_EQ(idsOf(bench.pass()), (Ids{0xB1U})) << asked;
        EXPECT_EQ(idsOf(bench.pass()), (Ids{0xB2U})) << asked;
        EXPECT_EQ(idsOf(bench.pass()), (Ids{0xA2U})) << asked;
        EXPECT_EQ(bench.node.droppedFrames(), 0U) << asked;
    }
}

TEST(FrameMergeNodeTests, AMeasurementThatStartsAgainStartsFromNothing)
{
    Bench bench{2U};

    const Wire a{frameAt(1U, 0xA1U), frameAt(2U, 0xA2U), frameAt(3U, 0xA3U)};
    const Wire b{frameAt(4U, 0xB1U), frameAt(5U, 0xB2U), frameAt(6U, 0xB3U)};
    const Wire c{frameAt(7U, 0xC1U), frameAt(8U, 0xC2U), frameAt(9U, 0xC3U)};
    const Wire d{frameAt(10U, 0xD1U), frameAt(11U, 0xD2U), frameAt(12U, 0xD3U)};

    (void)bench.pass(a, b, c, d);
    (void)bench.pass(a, b, c, d);
    ASSERT_GT(bench.node.waitingFrames(), 0U);
    ASSERT_GT(bench.node.droppedFrames(), 0U);

    // The second prepare is a new measurement: nothing of the first is left to come out of it, or
    // to be counted in it.
    ASSERT_TRUE(bench.node.prepare(2U).succeeded());

    EXPECT_EQ(bench.node.waitingFrames(), 0U);
    EXPECT_EQ(bench.node.passedFrames(), 0U);
    EXPECT_EQ(bench.node.heldOverFrames(), 0U);
    EXPECT_EQ(bench.node.droppedFrames(), 0U);
    EXPECT_TRUE(bench.pass().empty());

    EXPECT_EQ(idsOf(bench.pass(a)), (Ids{0xA1U, 0xA2U, 0xA3U}));
}

TEST(FrameMergeNodeTests, TheNodeCountsWhatItPassesHoldsOverAndDrops)
{
    Bench bench{2U};

    const Wire a{frameAt(1U, 0xA1U), frameAt(5U, 0xA2U)};
    const Wire b{frameAt(2U, 0xB1U), frameAt(6U, 0xB2U)};
    const Wire c{frameAt(3U, 0xC1U), frameAt(7U, 0xC2U)};
    const Wire d{frameAt(4U, 0xD1U), frameAt(8U, 0xD2U)};

    (void)bench.pass(a, b, c, d);

    const auto value = [&bench](const std::string& label) {
        for (const NodeStatistic& statistic : bench.node.statistics()) {
            if (statistic.label == label) {
                return static_cast<std::int64_t>(statistic.value);
            }
        }

        return std::int64_t{-1};
    };

    EXPECT_EQ(value("Frames passed"), 2);
    EXPECT_EQ(value("Frames held over"), 6);
    EXPECT_EQ(value("Frames dropped"), 0);
    EXPECT_EQ(bench.node.statistics().size(), 3U);
}

TEST(FrameMergeNodeTests, LightTrafficIsPutInOrderAcrossEveryPassAndNothingIsLost)
{
    // A few thousand passes of random traffic on four wires with a clock they share: each pass is a
    // window of time, and each wire says a few things in it. With a batch that is more than what
    // arrives, nothing waits for long and nothing is lost - and what comes out, over all the
    // passes, is in the order of its timestamps, which is the property the node is for.
    constexpr std::size_t kBatch = 16U;
    constexpr std::uint64_t kWindow = 1000U;

    Bench bench{kBatch};

    std::uint32_t state = 20260721U;
    const auto random = [&state](std::uint32_t bound) {
        state = state * 1664525U + 1013904223U;
        return (state >> 8U) % bound;
    };

    std::array<std::uint32_t, 4> sequence{};
    std::size_t sent = 0U;
    std::vector<CanFrame> heard;

    const auto hear = [&heard](std::span<const CanFrame> out) {
        EXPECT_LE(out.size(), kBatch);
        heard.insert(heard.end(), out.begin(), out.end());
    };

    for (std::uint64_t windowStart = 0U; windowStart < 3000U * kWindow; windowStart += kWindow) {
        std::array<Wire, 4> wires;

        for (std::uint32_t wire = 0U; wire < 4U; ++wire) {
            const std::uint32_t count = random(4U); // up to three a wire: twelve at the most
            std::vector<std::uint64_t> times;

            for (std::uint32_t index = 0U; index < count; ++index) {
                times.push_back(windowStart + random(static_cast<std::uint32_t>(kWindow)));
            }

            std::ranges::sort(times);

            for (const std::uint64_t time : times) {
                wires[wire].push_back(frameAt(
                    time, (wire << 16U) | sequence[wire]++, static_cast<std::uint8_t>(wire)));
                ++sent;
            }
        }

        hear(bench.pass(wires[0], wires[1], wires[2], wires[3]));
    }

    // Bounded: a node that never lets go of what waits fails this test, and does not hold it.
    for (int guard = 0; bench.node.waitingFrames() > 0U && guard < 100000; ++guard) {
        hear(bench.pass());
    }

    EXPECT_EQ(bench.node.waitingFrames(), 0U);

    EXPECT_EQ(bench.node.droppedFrames(), 0U);
    EXPECT_EQ(heard.size(), sent);
    EXPECT_EQ(bench.node.passedFrames(), sent);

    // In the order of time, over every pass.
    EXPECT_TRUE(std::ranges::is_sorted(heard, {}, &CanFrame::timestampNs));

    // And each wire's own frames are the ones it sent, in the order it sent them.
    for (std::uint32_t wire = 0U; wire < 4U; ++wire) {
        std::uint32_t expected = 0U;

        for (const CanFrame& frame : heard) {
            if ((frame.identifier >> 16U) == wire) {
                EXPECT_EQ(frame.identifier & 0xFFFFU, expected) << "wire " << wire;
                EXPECT_EQ(frame.channel, wire);
                ++expected;
            }
        }

        EXPECT_EQ(expected, sequence[wire]) << "wire " << wire;
    }
}

TEST(FrameMergeNodeTests, HeavyTrafficLosesTheLatestFramesAndCountsEveryOne)
{
    // More arrives than a batch can carry, pass after pass, so the room to wait fills and frames
    // are dropped. Nothing here may be lost without a count, a wire's frames still come out in
    // order, and what comes out is still in the order of time: dropping takes the latest, and does
    // not shuffle what is left.
    constexpr std::size_t kBatch = 8U;
    constexpr std::uint64_t kWindow = 1000U;

    Bench bench{kBatch};

    std::uint32_t state = 77U;
    const auto random = [&state](std::uint32_t bound) {
        state = state * 1664525U + 1013904223U;
        return (state >> 8U) % bound;
    };

    std::array<std::uint32_t, 4> sequence{};
    std::size_t sent = 0U;
    std::vector<CanFrame> heard;
    std::size_t mostWaiting = 0U;

    const auto hear = [&](std::span<const CanFrame> out) {
        EXPECT_LE(out.size(), kBatch);
        heard.insert(heard.end(), out.begin(), out.end());
        mostWaiting = std::max(mostWaiting, bench.node.waitingFrames());
    };

    for (std::uint64_t windowStart = 0U; windowStart < 2000U * kWindow; windowStart += kWindow) {
        std::array<Wire, 4> wires;

        for (std::uint32_t wire = 0U; wire < 4U; ++wire) {
            const std::uint32_t count =
                random(static_cast<std::uint32_t>(kBatch) + 1U); // up to a batch a wire
            std::vector<std::uint64_t> times;

            for (std::uint32_t index = 0U; index < count; ++index) {
                times.push_back(windowStart + random(static_cast<std::uint32_t>(kWindow)));
            }

            std::ranges::sort(times);

            for (const std::uint64_t time : times) {
                wires[wire].push_back(frameAt(
                    time, (wire << 16U) | sequence[wire]++, static_cast<std::uint8_t>(wire)));
                ++sent;
            }
        }

        hear(bench.pass(wires[0], wires[1], wires[2], wires[3]));
    }

    // Bounded: a node that never lets go of what waits fails this test, and does not hold it.
    for (int guard = 0; bench.node.waitingFrames() > 0U && guard < 100000; ++guard) {
        hear(bench.pass());
    }

    EXPECT_EQ(bench.node.waitingFrames(), 0U);

    // The room to wait is three batches, and is never more.
    EXPECT_LE(mostWaiting, 3U * kBatch);

    // Every frame that went in either came out or was counted as dropped.
    EXPECT_GT(bench.node.droppedFrames(), 0U);
    EXPECT_EQ(heard.size() + bench.node.droppedFrames(), sent);
    EXPECT_EQ(bench.node.passedFrames(), heard.size());
    EXPECT_GT(bench.node.heldOverFrames(), 0U);

    // In the order of time, and each wire in its own order - with gaps where a frame was dropped,
    // and never reordered.
    EXPECT_TRUE(std::ranges::is_sorted(heard, {}, &CanFrame::timestampNs));

    for (std::uint32_t wire = 0U; wire < 4U; ++wire) {
        std::int64_t previous = -1;

        for (const CanFrame& frame : heard) {
            if ((frame.identifier >> 16U) == wire) {
                EXPECT_GT(static_cast<std::int64_t>(frame.identifier & 0xFFFFU), previous)
                    << "wire " << wire;
                previous = static_cast<std::int64_t>(frame.identifier & 0xFFFFU);
            }
        }
    }
}

TEST(FrameMergeNodeTests, TheTypeIsInTheCatalogWithFourFramesInputsAndOneOutput)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* info = catalog.find("can.merge");
    ASSERT_NE(info, nullptr);

    EXPECT_EQ(info->category, "Transforms");
    EXPECT_EQ(info->displayName, "Frame Merge");
    EXPECT_FALSE(info->description.empty());
    EXPECT_TRUE(info->parameters.empty());
    ASSERT_EQ(info->inputs.size(), 4U);
    ASSERT_EQ(info->outputs.size(), 1U);

    for (const PortDescriptor& input : info->inputs) {
        EXPECT_EQ(input.type, PortType::Frames);
        EXPECT_FALSE(input.fanIn) << "an input takes one wire; the merge is how there are several";
    }

    EXPECT_EQ(info->outputs[0].type, PortType::Frames);

    // The node that is built has the ports the canvas draws.
    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> node;
    ASSERT_TRUE(catalog.create("can.merge", NodeParameters{}, context, "merge", node).succeeded());
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->typeName(), "can.merge");

    ASSERT_EQ(node->inputs().size(), info->inputs.size());

    for (std::size_t port = 0U; port < info->inputs.size(); ++port) {
        EXPECT_EQ(node->inputs()[port].name, info->inputs[port].name);
        EXPECT_EQ(node->inputs()[port].type, info->inputs[port].type);
    }

    ASSERT_EQ(node->outputs().size(), 1U);
    EXPECT_EQ(node->outputs()[0].name, info->outputs[0].name);
}

TEST(FrameMergeNodeTests, SeveralWiresEndInTheMergeAndTwoInOneInputStillDoNot)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const auto node = [](std::string id, std::string type, std::int64_t channel = -1) {
        NodeDescription description{.id = std::move(id), .typeName = std::move(type)};

        if (channel >= 0) {
            description.parameters = {{"channel", ParameterValue::fromInteger(channel)}};
        }

        return description;
    };

    GraphDescription description;
    description.addNode(node("can_1", "can.source", 0));
    description.addNode(node("can_2", "can.source", 1));
    description.addNode(node("merge", "can.merge"));
    description.addNode(node("j1939", "j1939.decoder"));
    description.addEdge(EdgeDescription{"can_1", 0, "merge", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "merge", 1});
    description.addEdge(EdgeDescription{"merge", 0, "j1939", 0});

    EXPECT_TRUE(description.validate(catalog).succeeded());

    // Two wires into the one input of the block behind it are still refused - and the message says
    // where the other is for frames.
    GraphDescription wrong;
    wrong.addNode(node("can_1", "can.source", 0));
    wrong.addNode(node("can_2", "can.source", 1));
    wrong.addNode(node("j1939", "j1939.decoder"));
    wrong.addEdge(EdgeDescription{"can_1", 0, "j1939", 0});
    wrong.addEdge(EdgeDescription{"can_2", 0, "j1939", 0});

    const Result refused = wrong.validate(catalog);
    ASSERT_TRUE(refused.failed());
    EXPECT_NE(std::string{refused.message()}.find("Frame Merge"), std::string::npos)
        << std::string{refused.message()};

    // Nor can two wires share an input of the merge itself.
    GraphDescription twice;
    twice.addNode(node("can_1", "can.source", 0));
    twice.addNode(node("can_2", "can.source", 1));
    twice.addNode(node("merge", "can.merge"));
    twice.addEdge(EdgeDescription{"can_1", 0, "merge", 2});
    twice.addEdge(EdgeDescription{"can_2", 0, "merge", 2});

    EXPECT_TRUE(twice.validate(catalog).failed());

    // And there are four inputs and not five.
    GraphDescription beyond;
    beyond.addNode(node("can_1", "can.source", 0));
    beyond.addNode(node("merge", "can.merge"));
    beyond.addEdge(EdgeDescription{"can_1", 0, "merge", 4});

    EXPECT_TRUE(beyond.validate(catalog).failed());
}

TEST(FrameMergeNodeTests, TheMergeIsAFrameStreamTheGraphAcceptsAnywhereAFrameStreamGoes)
{
    // Wired the way a project wires it, on a graph that is compiled and run by hand: two sources
    // that are not channels - a pair of nodes that publish a batch of their own - into the merge
    // and on to a sink that keeps what it is given.
    PipelineGraph graph;
    const NodeId first = graph.addNode(
        std::make_unique<Emitter>(Wire{frameAt(1U, 0xA1U, 0U), frameAt(4U, 0xA2U, 0U)}));
    const NodeId second = graph.addNode(
        std::make_unique<Emitter>(Wire{frameAt(2U, 0xB1U, 1U), frameAt(3U, 0xB2U, 1U)}));
    const NodeId merge = graph.addNode(std::make_unique<FrameMergeNode>());

    Ids heard;
    const NodeId sink =
        graph.addNode(std::make_unique<FrameSinkNode>([&heard](std::span<const CanFrame> frames) {
            for (const CanFrame& frame : frames) {
                heard.push_back(frame.identifier);
            }
        }));

    ASSERT_TRUE(graph.connect(PortRef{first, 0}, PortRef{merge, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{second, 0}, PortRef{merge, 3}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{merge, 0}, PortRef{sink, 0}).succeeded());

    // A third wire into one that is taken is what the graph refuses.
    EXPECT_TRUE(graph.connect(PortRef{second, 0}, PortRef{merge, 0}).failed());

    ASSERT_TRUE(graph.compile(8U).succeeded());
    graph.execute();

    EXPECT_EQ(heard, (Ids{0xA1U, 0xB1U, 0xB2U, 0xA2U}));

    // Again: the same four, as a second pass of the same bus - the merge does not remember.
    heard.clear();
    graph.execute();
    EXPECT_EQ(heard, (Ids{0xA1U, 0xB1U, 0xB2U, 0xA2U}));
}
