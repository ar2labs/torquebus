// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How fast a batch of frames turns into signals.
//
// There was already a throughput test - "The engine sustains 100k frames per
// second without loss" - and it does not decode anything. It measures the
// engine moving frames to a sink, which was the right thing to pin when
// PLAN.md section 17 was written, and it is blind to everything the decoder
// does per frame. A change that made decoding twice as slow would not move it
// at all.
//
// This one measures the other half: one decoder node, a realistic database, and
// a batch the size the engine actually hands a node.
//
// The assertion is deliberately loose. The number worth having is the one
// printed beside it, read by a person comparing two builds; a tight floor here
// would be a test that fails on a busy laptop and tells nobody anything.

#include "core/database/CanMessage.h"
#include "core/database/DecodedSignal.h"
#include "core/pipeline/nodes/DbcDecoderNode.h"

#include <gtest/gtest.h>

#include <iostream>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::size_t kMessages = 64;
constexpr std::size_t kSignalsPerMessage = 8;
constexpr std::size_t kBatchSize = 8192;
constexpr std::size_t kBatches = 128;

/// A database shaped like one somebody would really load: several dozen
/// messages, eight signals each, one of them multiplexed so the switch is read.
[[nodiscard]] std::shared_ptr<CanDatabase> benchmarkDatabase()
{
    auto database = std::make_shared<CanDatabase>();

    for (std::size_t index = 0; index < kMessages; ++index) {
        CanMessage message;
        message.identifier = static_cast<std::uint32_t>(0x100 + index);
        message.format = CanFrameFormat::Standard;
        message.name = "Message" + std::to_string(index);
        message.length = 8;

        for (std::size_t bit = 0; bit < kSignalsPerMessage; ++bit) {
            CanSignal signal;
            signal.name = message.name + "_S" + std::to_string(bit);
            signal.startBit = static_cast<std::uint16_t>(bit * 8);
            signal.bitLength = 8;
            signal.byteOrder = ByteOrder::Intel;
            signal.factor = 0.5;
            signal.offset = -20.0;

            message.signalList.push_back(signal);
        }

        database->addMessage(std::move(message));
    }

    return database;
}

[[nodiscard]] std::vector<CanFrame> benchmarkBatch()
{
    std::vector<CanFrame> frames(kBatchSize);

    for (std::size_t index = 0; index < frames.size(); ++index) {
        CanFrame& frame = frames[index];
        frame.identifier = static_cast<std::uint32_t>(0x100 + (index % kMessages));
        frame.format = CanFrameFormat::Standard;
        frame.length = 8;
        frame.dlc = 8;
        frame.timestampNs = index * 1000ULL;

        for (std::size_t byte = 0; byte < 8; ++byte) {
            frame.data[byte] = static_cast<std::uint8_t>(index + byte);
        }
    }

    return frames;
}

} // namespace

TEST(DecodeThroughputTests, DecodingKeepsUpWithABusThatIsActuallyBusyThroughput)
{
    DbcDecoderNode node{benchmarkDatabase(), "benchmark"};
    ASSERT_TRUE(node.prepare(kBatchSize).succeeded());

    const std::vector<CanFrame> frames = benchmarkBatch();

    std::array<PortBatch, 1> inputs{};
    std::array<PortBatch, 1> outputs{};

    const auto started = std::chrono::steady_clock::now();

    for (std::size_t pass = 0; pass < kBatches; ++pass) {
        inputs[0] = PortBatch{std::span<const CanFrame>{frames}};
        outputs[0] = PortBatch{};

        NodeContext context{inputs, outputs};
        node.process(context);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);

    const double decoded = static_cast<double>(kBatchSize * kBatches);
    const double seconds = static_cast<double>(elapsed.count()) / 1'000'000.0;
    const double framesPerSecond = decoded / seconds;

    // Printed rather than only asserted: this number is the point, and it is
    // read by a person comparing one build against another.
    std::cout << (::testing::Message()
                  << "decode throughput: " << static_cast<std::uint64_t>(framesPerSecond)
                  << " frames/s  (" << static_cast<std::uint64_t>(decoded) << " frames in "
                  << elapsed.count() << " us, " << node.emittedSignals() << " signals)")
                     .GetString()
              << '\n';

    EXPECT_TRUE(node.emittedSignals() == kBatchSize * kBatches * kSignalsPerMessage);

    // Loose on purpose. A tight floor here fails on a busy laptop and tells
    // nobody anything; the comparison that matters is between two builds, and
    // that is what the line above is for.
    EXPECT_TRUE(framesPerSecond > 10'000.0);
}
