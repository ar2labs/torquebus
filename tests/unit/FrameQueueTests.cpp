// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/FrameQueue.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Capacity is rounded up to a power of two", "[queue]")
{
    CHECK(FrameQueue{1}.capacity() == 2);
    CHECK(FrameQueue{2}.capacity() == 2);
    CHECK(FrameQueue{3}.capacity() == 4);
    CHECK(FrameQueue{100}.capacity() == 128);
    CHECK(FrameQueue{1024}.capacity() == 1024);
}

TEST_CASE("Frames come out in the order they went in", "[queue]")
{
    FrameQueue queue{16};

    for (std::uint32_t identifier = 0; identifier < 10; ++identifier) {
        REQUIRE(queue.push(frameWithId(identifier)));
    }

    CHECK(queue.size() == 10);

    std::vector<CanFrame> out;
    CHECK(queue.drainInto(out, 100) == 10);
    REQUIRE(out.size() == 10);

    for (std::uint32_t identifier = 0; identifier < 10; ++identifier) {
        CHECK(out[identifier].identifier == identifier);
    }

    CHECK(queue.empty());
}

TEST_CASE("A full queue declines a push without losing anything",
          "[queue][overflow]")
{
    // push() hands the frame back by returning false, so nothing was lost and
    // nothing is counted. The caller still holds the frame and decides whether
    // to retry or to discard it.
    FrameQueue queue{4};

    for (std::uint32_t identifier = 0; identifier < 4; ++identifier) {
        CHECK(queue.push(frameWithId(identifier)));
    }

    CHECK(queue.size() == 4);

    CHECK_FALSE(queue.push(frameWithId(99)));
    CHECK_FALSE(queue.push(frameWithId(100)));

    CHECK(queue.overflows() == 0);
    CHECK(queue.size() == 4);

    // The frames already queued are untouched: a full queue refuses the
    // newest, it never evicts the oldest.
    std::vector<CanFrame> out;
    REQUIRE(queue.drainInto(out, 10) == 4);
    CHECK(out.front().identifier == 0);
    CHECK(out.back().identifier == 3);
}

TEST_CASE("An overfull batch is discarded by the queue and counted",
          "[queue][overflow]")
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

    CHECK(queue.pushBatch(batch) == 4);
    CHECK(queue.overflows() == 2);
    CHECK(queue.size() == 4);
}

TEST_CASE("Draining frees space for new frames", "[queue]")
{
    FrameQueue queue{4};

    for (std::uint32_t identifier = 0; identifier < 4; ++identifier) {
        REQUIRE(queue.push(frameWithId(identifier)));
    }

    std::vector<CanFrame> out;
    REQUIRE(queue.drainInto(out, 2) == 2);

    CHECK(queue.push(frameWithId(10)));
    CHECK(queue.push(frameWithId(11)));
    CHECK_FALSE(queue.push(frameWithId(12)));

    REQUIRE(queue.drainInto(out, 10) == 4);
    CHECK(out[0].identifier == 2);
    CHECK(out[1].identifier == 3);
    CHECK(out[2].identifier == 10);
    CHECK(out[3].identifier == 11);
}

TEST_CASE("Indices wrap correctly over many cycles", "[queue]")
{
    // Exercises the mask arithmetic well past the point where the write index
    // has wrapped the ring many times.
    FrameQueue queue{8};
    std::vector<CanFrame> out;

    for (std::uint32_t round = 0; round < 1000; ++round) {
        REQUIRE(queue.push(frameWithId(round)));
        REQUIRE(queue.drainInto(out, 8) == 1);
        REQUIRE(out.size() == 1);
        CHECK(out.front().identifier == round);
    }

    CHECK(queue.overflows() == 0);
}

TEST_CASE("pushBatch accepts what fits and counts the rest", "[queue][batch]")
{
    FrameQueue queue{8};

    std::vector<CanFrame> batch;
    for (std::uint32_t identifier = 0; identifier < 12; ++identifier) {
        batch.push_back(frameWithId(identifier));
    }

    CHECK(queue.pushBatch(batch) == 8);
    CHECK(queue.overflows() == 4);
    CHECK(queue.size() == 8);

    std::vector<CanFrame> out;
    REQUIRE(queue.drainInto(out, 100) == 8);
    CHECK(out.front().identifier == 0);
    CHECK(out.back().identifier == 7);
}

TEST_CASE("An empty batch is a no-op", "[queue][batch]")
{
    FrameQueue queue{8};
    CHECK(queue.pushBatch({}) == 0);
    CHECK(queue.empty());
    CHECK(queue.overflows() == 0);
}

TEST_CASE("clear() discards without counting an overflow", "[queue]")
{
    FrameQueue queue{8};

    for (std::uint32_t identifier = 0; identifier < 5; ++identifier) {
        REQUIRE(queue.push(frameWithId(identifier)));
    }

    queue.clear();

    CHECK(queue.empty());
    CHECK(queue.overflows() == 0);
    CHECK(queue.push(frameWithId(42)));
}

TEST_CASE("A producer thread and a consumer thread lose nothing", "[queue][threading]")
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

    CHECK(ordered);
    CHECK(received == kTotal);

    // The producer retried on every refusal, so nothing was ever discarded by
    // the queue - and therefore nothing was counted.
    CHECK(queue.overflows() == 0);
}
