// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/FrameQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

CanFrame frameWithId(std::uint32_t identifier)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.length = 8;
    frame.dlc = 8;
    return frame;
}

} // namespace

TEST(FrameQueueTests, CapacityIsRoundedUpToAPowerOfTwo)
{
    EXPECT_TRUE(FrameQueue{1}.capacity() == 2);
    EXPECT_TRUE(FrameQueue{2}.capacity() == 2);
    EXPECT_TRUE(FrameQueue{3}.capacity() == 4);
    EXPECT_TRUE(FrameQueue{100}.capacity() == 128);
    EXPECT_TRUE(FrameQueue{1024}.capacity() == 1024);
}

TEST(FrameQueueTests, FramesComeOutInTheOrderTheyWentIn)
{
    FrameQueue queue{16};

    for (std::uint32_t identifier = 0; identifier < 10; ++identifier) {
        ASSERT_TRUE(queue.push(frameWithId(identifier)));
    }

    EXPECT_TRUE(queue.size() == 10);

    std::vector<CanFrame> out;
    EXPECT_TRUE(queue.drainInto(out, 100) == 10);
    ASSERT_TRUE(out.size() == 10);

    for (std::uint32_t identifier = 0; identifier < 10; ++identifier) {
        EXPECT_TRUE(out[identifier].identifier == identifier);
    }

    EXPECT_TRUE(queue.empty());
}

TEST(FrameQueueTests, AFullQueueDeclinesAPushWithoutLosingAnything)
{
    // push() hands the frame back by returning false, so nothing was lost and
    // nothing is counted. The caller still holds the frame and decides whether
    // to retry or to discard it.
    FrameQueue queue{4};

    for (std::uint32_t identifier = 0; identifier < 4; ++identifier) {
        EXPECT_TRUE(queue.push(frameWithId(identifier)));
    }

    EXPECT_TRUE(queue.size() == 4);

    EXPECT_FALSE(queue.push(frameWithId(99)));
    EXPECT_FALSE(queue.push(frameWithId(100)));

    EXPECT_TRUE(queue.overflows() == 0);
    EXPECT_TRUE(queue.size() == 4);

    // The frames already queued are untouched: a full queue refuses the
    // newest, it never evicts the oldest.
    std::vector<CanFrame> out;
    ASSERT_TRUE(queue.drainInto(out, 10) == 4);
    EXPECT_TRUE(out.front().identifier == 0);
    EXPECT_TRUE(out.back().identifier == 3);
}

TEST(FrameQueueTests, AnOverfullBatchIsDiscardedByTheQueueAndCounted)
{
    // pushBatch() is the real receive path, and the backend's batch is gone
    // the moment the handler returns - so a tail that does not fit is
    // genuinely lost, and must be visible as a number.
    //
    // Silent frame loss is a defect; counted frame loss is a diagnosis.
    FrameQueue queue{4};

    std::vector<CanFrame> batch;
    for (std::uint32_t identifier = 0; identifier < 6; ++identifier) {
        batch.push_back(frameWithId(identifier));
    }

    EXPECT_TRUE(queue.pushBatch(batch) == 4);
    EXPECT_TRUE(queue.overflows() == 2);
    EXPECT_TRUE(queue.size() == 4);
}

TEST(FrameQueueTests, DrainingFreesSpaceForNewFrames)
{
    FrameQueue queue{4};

    for (std::uint32_t identifier = 0; identifier < 4; ++identifier) {
        ASSERT_TRUE(queue.push(frameWithId(identifier)));
    }

    std::vector<CanFrame> out;
    ASSERT_TRUE(queue.drainInto(out, 2) == 2);

    EXPECT_TRUE(queue.push(frameWithId(10)));
    EXPECT_TRUE(queue.push(frameWithId(11)));
    EXPECT_FALSE(queue.push(frameWithId(12)));

    ASSERT_TRUE(queue.drainInto(out, 10) == 4);
    EXPECT_TRUE(out[0].identifier == 2);
    EXPECT_TRUE(out[1].identifier == 3);
    EXPECT_TRUE(out[2].identifier == 10);
    EXPECT_TRUE(out[3].identifier == 11);
}

TEST(FrameQueueTests, IndicesWrapCorrectlyOverManyCycles)
{
    // Exercises the mask arithmetic well past the point where the write index
    // has wrapped the ring many times.
    FrameQueue queue{8};
    std::vector<CanFrame> out;

    for (std::uint32_t round = 0; round < 1000; ++round) {
        ASSERT_TRUE(queue.push(frameWithId(round)));
        ASSERT_TRUE(queue.drainInto(out, 8) == 1);
        ASSERT_TRUE(out.size() == 1);
        EXPECT_TRUE(out.front().identifier == round);
    }

    EXPECT_TRUE(queue.overflows() == 0);
}

TEST(FrameQueueTests, PushBatchAcceptsWhatFitsAndCountsTheRest)
{
    FrameQueue queue{8};

    std::vector<CanFrame> batch;
    for (std::uint32_t identifier = 0; identifier < 12; ++identifier) {
        batch.push_back(frameWithId(identifier));
    }

    EXPECT_TRUE(queue.pushBatch(batch) == 8);
    EXPECT_TRUE(queue.overflows() == 4);
    EXPECT_TRUE(queue.size() == 8);

    std::vector<CanFrame> out;
    ASSERT_TRUE(queue.drainInto(out, 100) == 8);
    EXPECT_TRUE(out.front().identifier == 0);
    EXPECT_TRUE(out.back().identifier == 7);
}

TEST(FrameQueueTests, AnEmptyBatchIsANoOp)
{
    FrameQueue queue{8};
    EXPECT_TRUE(queue.pushBatch({}) == 0);
    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.overflows() == 0);
}

TEST(FrameQueueTests, ClearDiscardsWithoutCountingAnOverflow)
{
    FrameQueue queue{8};

    for (std::uint32_t identifier = 0; identifier < 5; ++identifier) {
        ASSERT_TRUE(queue.push(frameWithId(identifier)));
    }

    queue.clear();

    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.overflows() == 0);
    EXPECT_TRUE(queue.push(frameWithId(42)));
}

TEST(FrameQueueTests, AProducerThreadAndAConsumerThreadLoseNothing)
{
    // The single-producer / single-consumer contract, exercised for real. A
    // memory-ordering mistake shows up here as a lost or duplicated frame.
    constexpr std::uint32_t kTotal = 200'000;

    FrameQueue queue{1024};

    std::atomic<bool> producerDone{false};
    std::uint64_t received = 0;
    std::uint32_t expected = 0;
    bool ordered = true;

    std::thread producer{[&queue, &producerDone] {
        for (std::uint32_t identifier = 0; identifier < kTotal;) {
            if (queue.push(frameWithId(identifier))) {
                ++identifier;
            } else {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    }};

    std::vector<CanFrame> out;
    while (!producerDone.load(std::memory_order_acquire) || !queue.empty()) {
        const std::size_t count = queue.drainInto(out, 256);

        for (std::size_t index = 0; index < count; ++index) {
            if (out[index].identifier != expected) {
                ordered = false;
            }
            ++expected;
        }

        received += count;
    }

    producer.join();

    EXPECT_TRUE(ordered);
    EXPECT_TRUE(received == kTotal);

    // The producer retried on every refusal, so nothing was ever discarded by
    // the queue - and therefore nothing was counted.
    EXPECT_TRUE(queue.overflows() == 0);
}
