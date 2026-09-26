// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The J1939 block, rather than the protocol underneath it.
//
// Two things here are the reason the block exists at all, and both are cases
// where the ordinary DBC decoder gives a confidently wrong answer: a message
// arriving from an address the database did not anticipate, and a transport
// packet decoded as though it were a message.

#include "core/database/DbcParser.h"
#include "core/j1939/J1939Node.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint8_t kEngine = 0x00U;
constexpr std::uint8_t kOtherEngine = 0x03U;

/// Engine Temperature, a real broadcast PGN.
constexpr std::uint32_t kTemperature = 0x0'FEEEU;

/// A database with one message, written for an engine at address 0.
[[nodiscard]] std::shared_ptr<CanDatabase> databaseWithTemperature(std::uint8_t length = 8U,
                                                                   std::uint16_t startBit = 0U)
{
    CanSignal coolant;
    coolant.name = "EngineCoolantTemperature";
    coolant.startBit = startBit;
    coolant.bitLength = 8U;
    coolant.byteOrder = ByteOrder::Intel;
    coolant.factor = 1.0;
    coolant.offset = -40.0;

    CanMessage message;
    message.identifier = j1939Identifier(kTemperature, kEngine);
    message.format = CanFrameFormat::Extended;
    message.name = "EngineTemperature1";
    message.length = length;
    message.signalList.push_back(coolant);

    auto database = std::make_shared<CanDatabase>();
    database->addMessage(std::move(message));

    return database;
}

[[nodiscard]] CanFrame frameOf(std::uint32_t identifier, std::vector<std::uint8_t> payload)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.format = CanFrameFormat::Extended;
    frame.length = static_cast<std::uint8_t>(payload.size());
    frame.dlc = frame.length;

    for (std::size_t index = 0U; index < payload.size(); ++index) {
        frame.data[index] = payload[index];
    }

    return frame;
}

[[nodiscard]] CanFrame
bamFrame(std::uint32_t pgn, std::uint16_t size, std::uint8_t packets, std::uint8_t source)
{
    return frameOf(j1939Identifier(kPgnTransportConnection, source, kJ1939GlobalAddress, 7U),
                   {32U,
                    static_cast<std::uint8_t>(size & 0xFFU),
                    static_cast<std::uint8_t>((size >> 8U) & 0xFFU),
                    packets,
                    0xFFU,
                    static_cast<std::uint8_t>(pgn & 0xFFU),
                    static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU),
                    static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU)});
}

[[nodiscard]] CanFrame
dataFrame(std::uint8_t sequence, std::vector<std::uint8_t> seven, std::uint8_t source)
{
    seven.resize(J1939Transport::kBytesPerPacket, 0xFFU);
    seven.insert(seven.begin(), sequence);

    return frameOf(j1939Identifier(kPgnTransportData, source, kJ1939GlobalAddress, 7U),
                   std::move(seven));
}

/// Runs one batch through a node and hands back what it published.
class Driver final {
public:
    explicit Driver(J1939Node& node)
        : m_node{node}
    { }

    /// Not [[nodiscard]]: several cases here drive the block for its side
    /// effects - the address table, the fault lists - and never look at the
    /// signals.
    std::span<const DecodedSignal> run(std::span<const CanFrame> frames)
    {
        m_inputs[0] = PortBatch{frames};
        m_outputs[0] = PortBatch{};

        NodeContext context{m_inputs, m_outputs};
        m_node.process(context);

        return m_outputs[0].as<DecodedSignal>();
    }

private:
    J1939Node& m_node;
    std::array<PortBatch, 1> m_inputs{};
    std::array<PortBatch, 1> m_outputs{};
};

} // namespace

TEST(J1939NodeTests, AMessageDecodesWhateverAddressItCameFrom)
{
    // The reason the block exists. The database names this group for the engine
    // at address 0; on this bench the engine answers from 3, so every frame
    // arrives under an identifier the database has never seen. Matching by
    // identifier finds nothing and the bus looks silent.
    J1939Node node{databaseWithTemperature(), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());
    EXPECT_TRUE(node.knownPgns() == 1U);

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kTemperature, kOtherEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    const std::span<const DecodedSignal> signals = driver.run(frames);

    ASSERT_TRUE(signals.size() == 1U);
    EXPECT_TRUE(signals[0].signal->name == "EngineCoolantTemperature");
    EXPECT_TRUE(signals[0].value == 20.0);

    // And the identifier travels on as it arrived, so the source address is
    // still there for anything downstream that wants to know which ECU spoke.
    EXPECT_TRUE(j1939Decompose(signals[0].identifier).sourceAddress == kOtherEngine);
}

TEST(J1939NodeTests, ATransportPacketIsNeverDecodedAsAMessage)
{
    // A TP.DT is seven bytes of payload under a sequence number. Handed to a
    // signal decoder it produces a full set of plausible numbers, every one of
    // them wrong - so the reassembler is asked first and a frame it claims goes
    // no further.
    J1939Node node{databaseWithTemperature(12U, 64U), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};

    // The announcement and the first packet on their own: nothing is complete,
    // so nothing may be emitted.
    const std::array<CanFrame, 2> opening{bamFrame(kTemperature, 12U, 2U, kEngine),
                                          dataFrame(1U, {1U, 2U, 3U, 4U, 5U, 6U, 7U}, kEngine)};

    EXPECT_TRUE(driver.run(opening).empty());

    // The last packet completes it, and the signal is read out of byte 8 of the
    // reassembled message - a byte beyond anything a single frame can carry.
    // Packet one filled bytes 0..6, so byte 8 is the second byte of this one.
    const std::array<CanFrame, 1> closing{dataFrame(2U, {9U, 100U, 10U, 11U, 12U}, kEngine)};
    const std::span<const DecodedSignal> signals = driver.run(closing);

    ASSERT_TRUE(signals.size() == 1U);
    EXPECT_TRUE(signals[0].value == 60.0);
}

TEST(J1939NodeTests, ATransferThatLosesAPacketEmitsNothingAtAll)
{
    J1939Node node{databaseWithTemperature(12U, 64U), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 3> frames{bamFrame(kTemperature, 21U, 3U, kEngine),
                                         dataFrame(1U, {1U, 2U, 3U, 4U, 5U, 6U, 7U}, kEngine),
                                         dataFrame(3U, {1U, 2U, 3U, 4U, 5U, 6U, 7U}, kEngine)};

    EXPECT_TRUE(driver.run(frames).empty());
}

TEST(J1939NodeTests, TheBlockWatchesTheBusWithNoDatabaseAtAll)
{
    // Reassembly, the address table and trouble codes do not need one. A block
    // dropped on the canvas should already be telling somebody who is out
    // there.
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());
    EXPECT_TRUE(node.knownPgns() == 0U);

    Driver driver{node};
    const std::array<CanFrame, 2> frames{
        frameOf(j1939Identifier(kTemperature, kEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}),
        frameOf(j1939Identifier(kTemperature, kOtherEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    EXPECT_TRUE(driver.run(frames).empty());

    ASSERT_TRUE(node.addresses().nodes().size() == 2U);
    EXPECT_TRUE(node.addresses().nodes()[0].address == kEngine);
    EXPECT_TRUE(node.addresses().nodes()[1].address == kOtherEngine);
}

TEST(J1939NodeTests, ATroubleCodeInOneFrameReachesTheBlock)
{
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU})};

    driver.run(frames);

    ASSERT_TRUE(node.diagnostics().size() == 1U);

    const J1939Diagnostic& message = node.diagnostics()[0];
    EXPECT_TRUE(message.active);
    EXPECT_TRUE(message.sourceAddress == kEngine);
    EXPECT_TRUE(message.lamps.malfunction == J1939LampState::On);
    ASSERT_TRUE(message.faults.size() == 1U);
    EXPECT_TRUE(message.faults[0].spn == 100U);
    EXPECT_TRUE(message.faults[0].fmi == 1U);
}

TEST(J1939NodeTests, AHealthyECUIsRecordedAsHavingNothingWrong)
{
    // Not as absent. An ECU that says it is fine is different from one that has
    // never spoken, and the difference is the whole value of asking.
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x00U, 0x00U, 0U, 0U, 0U, 0U, 0xFFU, 0xFFU})};

    driver.run(frames);

    ASSERT_TRUE(node.diagnostics().size() == 1U);
    EXPECT_TRUE(node.diagnostics()[0].faults.empty());
}

TEST(J1939NodeTests, SeveralFaultsArriveOverTransportAndAreFiledTogether)
{
    // More than one active fault does not fit in eight bytes, which is why a
    // real DM1 is a transport message and why this path has to work.
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};

    // Lamps, then three codes: 2 + 12 = 14 bytes, two packets.
    const std::array<CanFrame, 3> frames{
        bamFrame(kPgnDm1, 14U, 2U, kEngine),
        dataFrame(1U, {0x40U, 0x00U, 100U, 0U, 1U, 2U, 110U}, kEngine),
        dataFrame(2U, {0U, 3U, 1U, 190U, 0U, 4U, 7U}, kEngine)};

    driver.run(frames);

    ASSERT_TRUE(node.diagnostics().size() == 1U);

    const J1939Diagnostic& message = node.diagnostics()[0];
    ASSERT_TRUE(message.faults.size() == 3U);
    EXPECT_TRUE(message.faults[0].spn == 100U);
    EXPECT_TRUE(message.faults[1].spn == 110U);
    EXPECT_TRUE(message.faults[2].spn == 190U);
}

TEST(J1939NodeTests, ANewerFaultListReplacesTheOlderOneFromTheSameECU)
{
    // A fault list is a statement about now. Keeping the previous one would
    // show a repaired fault beside the current answer as though both were true.
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};

    const std::array<CanFrame, 1> faulted{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU})};
    driver.run(faulted);

    const std::array<CanFrame, 1> healthy{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x00U, 0x00U, 0U, 0U, 0U, 0U, 0xFFU, 0xFFU})};
    driver.run(healthy);

    ASSERT_TRUE(node.diagnostics().size() == 1U);
    EXPECT_TRUE(node.diagnostics()[0].faults.empty());
}

TEST(J1939NodeTests, WhatIsWrongNowAndWhatUsedToBeAreKeptApart)
{
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 2> frames{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU}),
        frameOf(j1939Identifier(kPgnDm2, kEngine), {0x00U, 0x00U, 110U, 0U, 3U, 2U, 0xFFU, 0xFFU})};

    driver.run(frames);

    ASSERT_TRUE(node.diagnostics().size() == 2U);

    // DM1 first for the same ECU: what is wrong now is read before what used
    // to be.
    EXPECT_TRUE(node.diagnostics()[0].active);
    EXPECT_TRUE(node.diagnostics()[0].faults[0].spn == 100U);
    EXPECT_FALSE(node.diagnostics()[1].active);
    EXPECT_TRUE(node.diagnostics()[1].faults[0].spn == 110U);
}

TEST(J1939NodeTests, TurningTheSPNAssemblyOffLeavesTheBytesAndTheFault)
{
    J1939Node node{nullptr, "J1939"};
    node.setSpnReading(J1939SpnReading::RawOnly);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU})};

    driver.run(frames);

    ASSERT_TRUE(node.diagnostics().size() == 1U);
    ASSERT_TRUE(node.diagnostics()[0].faults.size() == 1U);
    EXPECT_FALSE(node.diagnostics()[0].faults[0].spnAssembled);
    EXPECT_TRUE(node.diagnostics()[0].faults[0].raw[0] == 100U);
}

TEST(J1939NodeTests, TheBlockCountsWhatAPersonWouldWantToCompare)
{
    // "PGNs not in the database" next to "messages decoded" is the pair that
    // names a bus with the wrong database loaded - which otherwise looks quiet
    // rather than wrong.
    J1939Node node{databaseWithTemperature(), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 2> frames{
        frameOf(j1939Identifier(kTemperature, kEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}),
        frameOf(j1939Identifier(0x0'FEE5U, kEngine), {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    driver.run(frames);

    const std::vector<NodeStatistic> statistics = node.statistics();

    const auto value = [&statistics](std::string_view label) -> std::uint64_t {
        for (const NodeStatistic& statistic : statistics) {
            if (statistic.label == label) {
                return statistic.value;
            }
        }

        return ~std::uint64_t{0};
    };

    EXPECT_TRUE(value("Frames") == 2U);
    EXPECT_TRUE(value("Messages decoded") == 1U);
    EXPECT_TRUE(value("PGNs not in the database") == 1U);
    EXPECT_TRUE(value("Signals emitted") == 1U);
    EXPECT_TRUE(value("ECUs seen") == 1U);
}

TEST(J1939NodeTests, TheBlockIsInTheCatalogAndBuildsWithoutADatabase)
{
    // A freshly dropped block has no path yet, and a graph that will not
    // compile until every block is configured cannot be built up in any order
    // but one.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* info = catalog.find("j1939.decoder");
    ASSERT_TRUE(info != nullptr);
    EXPECT_TRUE(info->category == "Transforms");
    EXPECT_TRUE(info->inputs.size() == 1U);
    EXPECT_TRUE(info->outputs.size() == 1U);

    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> node;

    const Result result = catalog.create("j1939.decoder", NodeParameters{}, context, "j", node);

    ASSERT_TRUE(result.succeeded());
    ASSERT_TRUE(node != nullptr);
    EXPECT_TRUE(node->typeName() == "j1939.decoder");
}

TEST(J1939NodeTests, NothingPublishedIsNotTheSameAsNobodyOnTheBus)
{
    // A measurement that has not started and a bus with no traffic look alike
    // in an empty table. The revision tells them apart, and a panel has to say
    // which one it is showing.
    J1939Network network;

    EXPECT_TRUE(network.revision() == 0U);
    EXPECT_TRUE(network.snapshot().nodes.empty());
    EXPECT_TRUE(network.snapshot().revision == 0U);
}

TEST(J1939NodeTests, TheBlockHandsItsViewOverWhenTheBusChanges)
{
    J1939Network network;

    J1939Node node{nullptr, "J1939"};
    node.setNetwork(&network);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> first{
        frameOf(j1939Identifier(kTemperature, kEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    driver.run(first);

    const std::uint64_t afterFirst = network.revision();
    EXPECT_TRUE(afterFirst > 0U);

    const J1939NetworkSnapshot snapshot = network.snapshot();
    ASSERT_TRUE(snapshot.nodes.size() == 1U);
    EXPECT_TRUE(snapshot.nodes[0].address == kEngine);
    EXPECT_TRUE(snapshot.revision == afterFirst);

    // A second ECU is a change, so it is handed over.
    const std::array<CanFrame, 1> second{
        frameOf(j1939Identifier(kTemperature, kOtherEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};
    driver.run(second);

    EXPECT_TRUE(network.revision() > afterFirst);
    EXPECT_TRUE(network.snapshot().nodes.size() == 2U);
}

TEST(J1939NodeTests, ASteadyBusIsNotCopiedOverAndOver)
{
    // The same ECUs sending the same messages for an hour change nothing after
    // the first second. Copying the table every pass anyway would be an
    // allocation per batch bought for no reason at all.
    J1939Network network;

    J1939Node node{nullptr, "J1939"};
    node.setNetwork(&network);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kTemperature, kEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    driver.run(frames);
    const std::uint64_t settled = network.revision();

    for (int pass = 0; pass < 10; ++pass) {
        driver.run(frames);
    }

    EXPECT_TRUE(network.revision() == settled);
}

TEST(J1939NodeTests, AFaultReachesThePanelSideOfTheHandOver)
{
    J1939Network network;

    J1939Node node{nullptr, "J1939"};
    node.setNetwork(&network);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU})};

    driver.run(frames);

    const J1939NetworkSnapshot snapshot = network.snapshot();
    ASSERT_TRUE(snapshot.diagnostics.size() == 1U);
    ASSERT_TRUE(snapshot.diagnostics[0].faults.size() == 1U);
    EXPECT_TRUE(snapshot.diagnostics[0].faults[0].spn == 100U);
}

TEST(J1939NodeTests, ClearingForANewMeasurementIsAChangeAPanelNotices)
{
    // Not a reset to zero: a panel watching for movement would miss a clear
    // that put the counter back where it already was, and go on showing the
    // previous run's bus.
    J1939Network network;
    network.publish({J1939NetworkNode{}}, {}, {});

    const std::uint64_t before = network.revision();
    network.clear();

    EXPECT_TRUE(network.revision() > before);
    EXPECT_TRUE(network.snapshot().nodes.empty());
}
