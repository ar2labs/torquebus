// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Asking a bus who is on it.
//
// A claim is made once, at power-up, so a measurement started on a machine that was already running
// has none to read; the request is the remedy. What matters is where it goes (every channel, from
// the null address, once, and only when pressed), what happens when it cannot go (a channel that is
// not running says so and the others are still asked), and that an ECU on the bus answers it
// through the real path - the engine, the channel, the bus block and the node - and not only in a
// unit test of the node.

#include "core/can/CanEngine.h"
#include "core/j1939/J1939ClaimRequest.h"
#include "core/j1939/J1939Id.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A virtual channel, bound as application channel `index`.
[[nodiscard]] Result addVirtualChannel(CanEngine& engine, std::uint8_t index)
{
    CanChannelConfig config;
    config.deviceHandle = "virtual:" + std::to_string(index);
    config.applicationChannel = index;
    config.timing.bitrate = 250'000;

    return engine.addChannel(std::make_unique<VirtualCanBackend>(), config);
}

/// Polls instead of sleeping a fixed time: the engine runs on a thread of its own, and a test that
/// waits for what it came to see is as slow as the machine and no slower.
[[nodiscard]] bool waitUntil(const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};

    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    return done();
}

/// Everything the channels of an engine delivered, kept for the test thread to read.
class Heard final {
public:
    void add(std::span<const CanFrame> frames)
    {
        const std::lock_guard<std::mutex> lock{m_mutex};
        m_frames.insert(m_frames.end(), frames.begin(), frames.end());
    }

    /// The frames so far that are `pgn` from `source`, or from anyone when `source` is nothing.
    [[nodiscard]] std::vector<CanFrame> of(std::uint32_t pgn,
                                           std::optional<std::uint8_t> source = {}) const
    {
        const std::lock_guard<std::mutex> lock{m_mutex};
        std::vector<CanFrame> matching;

        for (const CanFrame& frame : m_frames) {
            const std::optional<J1939Id> id = j1939Decompose(frame);

            if (id.has_value() && id->pgn() == pgn
                && (!source.has_value() || id->sourceAddress == *source)) {
                matching.push_back(frame);
            }
        }

        return matching;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<CanFrame> m_frames;
};

} // namespace

TEST(J1939ClaimRequestTests, AnEngineWithNoChannelsHasNobodyToAsk)
{
    CanEngine engine;

    const J1939ClaimRequestOutcome outcome = j1939RequestAddressClaims(engine);

    EXPECT_TRUE(outcome.sent.empty());
    EXPECT_TRUE(outcome.refused.empty());
}

TEST(J1939ClaimRequestTests, AMeasurementThatIsNotRunningRefusesOnEveryChannelAndSaysWhich)
{
    CanEngine engine;
    ASSERT_TRUE(addVirtualChannel(engine, 0U).succeeded());
    ASSERT_TRUE(addVirtualChannel(engine, 1U).succeeded());

    const J1939ClaimRequestOutcome outcome = j1939RequestAddressClaims(engine);

    EXPECT_TRUE(outcome.sent.empty());
    ASSERT_EQ(outcome.refused.size(), 2U);

    // Counted from one, as the channels are named everywhere a person reads them.
    EXPECT_EQ(outcome.refused[0].rfind("CAN 1: ", 0U), 0U) << outcome.refused[0];
    EXPECT_EQ(outcome.refused[1].rfind("CAN 2: ", 0U), 0U) << outcome.refused[1];
}

TEST(J1939ClaimRequestTests, ARunningMeasurementIsAskedOnEveryChannelFromTheNullAddress)
{
    CanEngine engine;
    ASSERT_TRUE(addVirtualChannel(engine, 0U).succeeded());
    ASSERT_TRUE(addVirtualChannel(engine, 1U).succeeded());

    Heard heard;
    engine.addFrameSink([&heard](std::span<const CanFrame> frames) { heard.add(frames); });

    const Result started = engine.start();
    ASSERT_TRUE(started.succeeded()) << std::string{started.message()};

    const J1939ClaimRequestOutcome outcome = j1939RequestAddressClaims(engine);

    EXPECT_EQ(outcome.sent, (std::vector<std::uint8_t>{0U, 1U}));
    EXPECT_TRUE(outcome.refused.empty());

    // One request on each channel, and the same request: priority 6, to everyone, from 254, for
    // PGN 60928 least significant byte first.
    ASSERT_TRUE(waitUntil([&heard] { return heard.of(kPgnRequest).size() >= 2U; }));
    engine.stop();

    const std::vector<CanFrame> requests = heard.of(kPgnRequest);
    ASSERT_EQ(requests.size(), 2U);

    for (const CanFrame& request : requests) {
        EXPECT_EQ(request.identifier, 0x18EAFFFEU);
        EXPECT_EQ(j1939RequestedPgn(request), kPgnAddressClaimed);
    }

    EXPECT_NE(requests[0].channel, requests[1].channel);
}

TEST(J1939ClaimRequestTests, AScriptedEcuOnTheBusClaimsAgainThroughTheEngine)
{
    // The ECU announces itself at power-up and says nothing more about it. Asked, it announces
    // itself again, with the same claim - through a CAN Bus block, a channel and the engine, as in
    // a project.
    CanEngine engine;
    ASSERT_TRUE(addVirtualChannel(engine, 0U).succeeded());

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "can_1",
        .typeName = "can.bus",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "ecu",
        .typeName = "lua.ecu",
        .parameters = {{"channel", ParameterValue::fromInteger(0)},
                       {"script",
                        ParameterValue::fromText(
                            "function on_enable() j1939_claim(0x21, 26, 0, nil, 1, false) end")}},
    });
    description.addEdge(EdgeDescription{"ecu", 0, "can_1", 0});
    description.addEdge(EdgeDescription{"can_1", 0, "ecu", 0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    ASSERT_TRUE(description.validate(catalog).succeeded());
    engine.setGraphDescription(description, catalog);

    Heard heard;
    engine.addFrameSink([&heard](std::span<const CanFrame> frames) { heard.add(frames); });

    const Result started = engine.start();
    ASSERT_TRUE(started.succeeded()) << std::string{started.message()};

    const auto claims = [&heard] { return heard.of(kPgnAddressClaimed, std::uint8_t{0x21U}); };

    // The claim at power-up first, so that what follows the request is known to be the answer to
    // it.
    ASSERT_TRUE(waitUntil([&claims] { return claims().size() == 1U; }));

    const J1939ClaimRequestOutcome outcome = j1939RequestAddressClaims(engine);
    EXPECT_EQ(outcome.sent, (std::vector<std::uint8_t>{0U}));

    EXPECT_TRUE(waitUntil([&claims] { return claims().size() == 2U; }));
    engine.stop();

    const std::vector<CanFrame> both = claims();
    ASSERT_EQ(both.size(), 2U);
    EXPECT_EQ(both[1].identifier, both[0].identifier);
    EXPECT_TRUE(std::equal(both[0].data.begin(), both[0].data.begin() + 8, both[1].data.begin()));
}
