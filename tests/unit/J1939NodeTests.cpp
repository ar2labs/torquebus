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

#include "core/can/CanFilter.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/DbcParser.h"
#include "core/j1939/J1939Node.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
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

TEST(J1939NodeTests, SpecialValuesForNotAvailableAndErrorDecodeToNan)
{
    EXPECT_TRUE(isJ1939SpecialValue(2U, 2U));
    EXPECT_TRUE(isJ1939SpecialValue(3U, 2U));
    EXPECT_FALSE(isJ1939SpecialValue(1U, 2U));

    EXPECT_TRUE(isJ1939SpecialValue(0xFEU, 8U));
    EXPECT_TRUE(isJ1939SpecialValue(0xFFU, 8U));
    EXPECT_FALSE(isJ1939SpecialValue(0xFAU, 8U));

    EXPECT_TRUE(isJ1939SpecialValue(0xFE00U, 16U));
    EXPECT_TRUE(isJ1939SpecialValue(0xFFFFU, 16U));
    EXPECT_FALSE(isJ1939SpecialValue(0xFAFFU, 16U));

    J1939Node node{databaseWithTemperature(), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};

    // Valid reading: 100 - 40 = 60 °C
    const auto validBatch = driver.run(std::array<CanFrame, 1>{
        frameOf(j1939Identifier(kTemperature, kEngine), {100U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})});
    ASSERT_EQ(validBatch.size(), 1U);
    EXPECT_DOUBLE_EQ(validBatch[0].value, 60.0);

    // NA reading (0xFF): decoded value must be NaN
    const auto naBatch = driver.run(std::array<CanFrame, 1>{
        frameOf(j1939Identifier(kTemperature, kEngine), {0xFFU, 0U, 0U, 0U, 0U, 0U, 0U, 0U})});
    ASSERT_EQ(naBatch.size(), 1U);
    EXPECT_TRUE(std::isnan(naBatch[0].value));

    // Error reading (0xFE): decoded value must be NaN
    const auto errBatch = driver.run(std::array<CanFrame, 1>{
        frameOf(j1939Identifier(kTemperature, kEngine), {0xFEU, 0U, 0U, 0U, 0U, 0U, 0U, 0U})});
    ASSERT_EQ(errBatch.size(), 1U);
    EXPECT_TRUE(std::isnan(errBatch[0].value));
}

TEST(J1939NodeTests, CanFilterAcceptsByPgnAndOptionalSourceAddress)
{
    // CCVS1 PGN 0xFEF1 (PDU2 broadcast)
    const CanFilter filterAnySa = CanFilter::acceptPgn(0x0'FEF1U);
    EXPECT_TRUE(filterAnySa.matches(frameOf(0x18FEF100U, {0U}))); // SA 0x00, prio 6
    EXPECT_TRUE(filterAnySa.matches(frameOf(0x0CFEF10BU, {0U}))); // SA 0x0B, prio 3
    EXPECT_FALSE(filterAnySa.matches(frameOf(0x18FEEE00U, {0U}))); // Different PGN (ET1)
    EXPECT_FALSE(filterAnySa.matches(frameOf(0x101U, {0U}))); // 11-bit standard frame

    const CanFilter filterEngineSa = CanFilter::acceptPgn(0x0'FEF1U, std::uint8_t{0x00U});
    EXPECT_TRUE(filterEngineSa.matches(frameOf(0x18FEF100U, {0U})));
    EXPECT_FALSE(filterEngineSa.matches(frameOf(0x18FEF10BU, {0U}))); // Different SA

    // Request PGN 0xEA00 (PDU1 destination-specific)
    const CanFilter filterRequest = CanFilter::acceptPgn(0x0'EA00U, std::uint8_t{0x00U});
    EXPECT_TRUE(filterRequest.matches(frameOf(0x18EA0300U, {0U}))); // DA 0x03, SA 0x00
    EXPECT_FALSE(filterRequest.matches(frameOf(0x18EA0301U, {0U}))); // SA 0x01
}

// ---------------------------------------------------------------------------
// The lamps of the whole bus
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint8_t kBrakes = 0x0BU;
constexpr std::uint8_t kBody = 0x21U;

// Byte 1 of a DM1: protect 1-2, amber warning 3-4, red stop 5-6, malfunction 7-8, and 01 is on.
constexpr std::uint8_t kRedStop = 0x10U;
constexpr std::uint8_t kAmberWarning = 0x04U;
constexpr std::uint8_t kMalfunction = 0x40U;

[[nodiscard]] CanFrame
diagnosticFrom(std::uint32_t pgn, std::uint8_t source, std::uint8_t lamps, std::uint64_t atNs)
{
    CanFrame frame =
        frameOf(j1939Identifier(pgn, source), {lamps, 0xFFU, 0U, 0U, 0U, 0U, 0xFFU, 0xFFU});
    frame.timestampNs = atNs;
    return frame;
}

constexpr std::uint64_t kSecond = 1'000'000'000ULL;

/// A block with nowhere to look up a message, wired to a set of system variables.
struct LampBench final {
    LampBench()
        : node{nullptr, "J1939"}
    {
        node.setSystemVariables(&variables);
        EXPECT_TRUE(node.prepare(64U).succeeded());
    }

    [[nodiscard]] bool stop() const
    {
        return variables.value(std::string{kJ1939LampStopVariable}) > 0.5;
    }

    [[nodiscard]] bool warning() const
    {
        return variables.value(std::string{kJ1939LampWarningVariable}) > 0.5;
    }

    [[nodiscard]] bool malfunction() const
    {
        return variables.value(std::string{kJ1939LampMilVariable}) > 0.5;
    }

    [[nodiscard]] bool protect() const
    {
        return variables.value(std::string{kJ1939LampProtectVariable}) > 0.5;
    }

    SystemVariables variables;
    J1939Node node;
};

} // namespace

TEST(J1939NodeTests, TheLampsOfTheWholeBusAreTheOrOfEveryEcusLamps)
{
    // A vehicle has an engine, brakes and a body controller, each sending a DM1 with its own lamps
    // - and a database names a message once, so "DM1.RedStopLamp" is whichever of them came last. A
    // red lamp that shows for the milliseconds between the engine's message and the brakes' is not
    // a red lamp. This is what the cluster reads instead.
    LampBench bench;
    Driver driver{bench.node};

    EXPECT_FALSE(bench.stop());

    const std::array<CanFrame, 3> first{diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond),
                                        diagnosticFrom(kPgnDm1, kBody, kAmberWarning, kSecond),
                                        diagnosticFrom(kPgnDm1, kBrakes, 0x00U, kSecond)};
    driver.run(first);

    // The brakes, with every lamp dark, came last. The engine's red lamp is lit all the same.
    EXPECT_TRUE(bench.stop());
    EXPECT_TRUE(bench.warning());
    EXPECT_FALSE(bench.malfunction());
    EXPECT_FALSE(bench.protect());

    // The engine's fault clears - a DM1 with every lamp dark - and the body's amber is still there.
    const std::array<CanFrame, 1> cleared{diagnosticFrom(kPgnDm1, kEngine, 0x00U, 2U * kSecond)};
    driver.run(cleared);

    EXPECT_FALSE(bench.stop());
    EXPECT_TRUE(bench.warning());
}

TEST(J1939NodeTests, AllFourLampsReachTheirOwnVariable)
{
    LampBench bench;
    Driver driver{bench.node};

    // Protect is bits 1-2, so 0x01; the malfunction indicator 0x40.
    const std::array<CanFrame, 1> frames{
        diagnosticFrom(kPgnDm1, kEngine, kMalfunction | 0x01U, kSecond)};
    driver.run(frames);

    EXPECT_FALSE(bench.stop());
    EXPECT_FALSE(bench.warning());
    EXPECT_TRUE(bench.malfunction());
    EXPECT_TRUE(bench.protect());
}

TEST(J1939NodeTests, AnEcuThatFallsSilentStopsLightingItsLamp)
{
    // J1939-73 has a DM1 repeated every second. One that has not come for three has an ECU behind
    // it that is switched off or unplugged, and a lamp for a fault nobody can still be reporting is
    // a lamp that lies. Time is the bus's: it moves when any frame arrives.
    LampBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> lit{diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond)};
    driver.run(lit);
    EXPECT_TRUE(bench.stop());

    // Two seconds on, the body controller is still talking and the engine has not repeated itself.
    const std::array<CanFrame, 1> soon{diagnosticFrom(kPgnDm1, kBody, 0x00U, 3U * kSecond)};
    driver.run(soon);
    EXPECT_TRUE(bench.stop());

    const std::array<CanFrame, 1> late{diagnosticFrom(kPgnDm1, kBody, 0x00U, 5U * kSecond)};
    driver.run(late);
    EXPECT_FALSE(bench.stop());
}

TEST(J1939NodeTests, WhatWasWrongIsNotALamp)
{
    // A DM2 is the faults that were active, and the same bytes in the same place. A lamp lit by one
    // would be a repaired fault on the dash.
    LampBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> history{diagnosticFrom(kPgnDm2, kEngine, kRedStop, kSecond)};
    driver.run(history);

    EXPECT_FALSE(bench.stop());
}

TEST(J1939NodeTests, StoppingTheMeasurementPutsTheLampsOut)
{
    // The cluster's signals go to dashes after Stop. A variable would hold its last value for ever,
    // and a stopped measurement would show a red lamp for a fault nobody can still be reporting.
    LampBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> lit{diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond)};
    driver.run(lit);
    ASSERT_TRUE(bench.stop());

    bench.node.finish();
    EXPECT_FALSE(bench.stop());

    // And what a panel reads afterwards is still there.
    EXPECT_FALSE(bench.node.diagnostics().empty());
}

TEST(J1939NodeTests, AMeasurementStartsWithTheLampsOutWhateverTheLastOneLeft)
{
    SystemVariables variables;
    variables.set(std::string{kJ1939LampStopVariable}, 1.0);

    J1939Node node{nullptr, "J1939"};
    node.setSystemVariables(&variables);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    EXPECT_EQ(variables.value(std::string{kJ1939LampStopVariable}), 0.0);
}

TEST(J1939NodeTests, TheLampsAreWrittenWhenTheyChangeAndNotEveryPass)
{
    LampBench bench;
    Driver driver{bench.node};

    const SystemVariables::Handle stop = bench.variables.find(std::string{kJ1939LampStopVariable});
    ASSERT_TRUE(stop != SystemVariables::kUnknown);

    const std::array<CanFrame, 1> lit{diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond)};
    driver.run(lit);
    const std::uint64_t written = bench.variables.revision(stop);

    // A DM1 a second, saying the same thing: nothing changed, so nothing was written.
    for (std::uint64_t second = 2U; second < 4U; ++second) {
        const std::array<CanFrame, 1> again{
            diagnosticFrom(kPgnDm1, kEngine, kRedStop, second * kSecond)};
        driver.run(again);
    }

    EXPECT_EQ(bench.variables.revision(stop), written);
}

TEST(J1939NodeTests, ABlockWithNoVariablesStillDecodesTroubleCodes)
{
    J1939Node node{nullptr, "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> frames{diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond)};
    driver.run(frames);

    ASSERT_EQ(node.diagnostics().size(), 1U);
}

// ---------------------------------------------------------------------------
// "Error" and "not available", and the signals that are not about either
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::shared_ptr<CanDatabase> databaseWithSignal(CanSignal signal)
{
    CanMessage message;
    message.identifier = j1939Identifier(kTemperature, kEngine);
    message.format = CanFrameFormat::Extended;
    message.name = "EngineTemperature1";
    message.length = 8;
    message.signalList.push_back(std::move(signal));

    auto database = std::make_shared<CanDatabase>();
    database->addMessage(std::move(message));
    return database;
}

[[nodiscard]] double decodeFirstByte(J1939Node& node, std::uint8_t byte)
{
    Driver driver{node};
    const std::array<CanFrame, 1> frames{
        frameOf(j1939Identifier(kTemperature, kEngine), {byte, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};

    const std::span<const DecodedSignal> signals = driver.run(frames);
    return signals.empty() ? std::nan("") : signals[0].value;
}

} // namespace

TEST(J1939NodeTests, ASignedSignalIsNeverNotAvailable)
{
    // Raw 0xFF is -1 in two's complement, a perfectly good reading, and read as the unsigned number
    // it also is it is "not available". J1939's table says nothing about signed values.
    CanSignal torque;
    torque.name = "Torque";
    torque.bitLength = 8U;
    torque.isSigned = true;

    J1939Node node{databaseWithSignal(torque), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    EXPECT_DOUBLE_EQ(decodeFirstByte(node, 0xFFU), -1.0);
    EXPECT_DOUBLE_EQ(decodeFirstByte(node, 0xFEU), -2.0);
}

TEST(J1939NodeTests, AValueTheDatabaseNamesIsAStateAndNotAnError)
{
    // A turn stalk with three positions in two bits: 2 is "Right". A .dbc says so in the one place
    // it can, a value table, and a value with a name is a value with a meaning.
    CanSignal stalk;
    stalk.name = "TurnSignalSwitch";
    stalk.bitLength = 2U;
    stalk.valueNames = {{0, "None"}, {1, "Left"}, {2, "Right"}};

    J1939Node node{databaseWithSignal(stalk), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    EXPECT_DOUBLE_EQ(decodeFirstByte(node, 0x01U), 1.0);
    EXPECT_DOUBLE_EQ(decodeFirstByte(node, 0x02U), 2.0);

    // 3 has no name, and is what J1939 means by it: not available.
    EXPECT_TRUE(std::isnan(decodeFirstByte(node, 0x03U)));
}

TEST(J1939NodeTests, AnUnnamedErrorValueIsNotANumber)
{
    CanSignal level;
    level.name = "Level";
    level.bitLength = 8U;
    level.factor = 0.4;

    J1939Node node{databaseWithSignal(level), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    EXPECT_DOUBLE_EQ(decodeFirstByte(node, 250U), 100.0);
    EXPECT_TRUE(std::isnan(decodeFirstByte(node, 0xFEU))); // error
    EXPECT_TRUE(std::isnan(decodeFirstByte(node, 0xFFU))); // not available
}
