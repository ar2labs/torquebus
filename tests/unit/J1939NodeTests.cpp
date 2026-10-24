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

#include "core/can/CanEngine.h"
#include "core/can/CanFilter.h"
#include "core/dashboard/DashboardFaults.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/DbcParser.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Node.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include "UniqueTempPath.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

/// The one bus a single J1939 block publishes, or an empty one before it has published anything.
/// Most of what is asked here is about one block, and so about one bus.
[[nodiscard]] const J1939BusView& busOf(const J1939NetworkSnapshot& snapshot)
{
    static const J1939BusView kNothingPublished{};

    EXPECT_LE(snapshot.buses.size(), 1U) << "one J1939 block publishes one bus";
    return snapshot.buses.empty() ? kNothingPublished : snapshot.buses.front();
}

/// The same for a snapshot nobody keeps, as a copy: a reference would outlive it.
[[nodiscard]] J1939BusView busOf(J1939NetworkSnapshot&& snapshot)
{
    EXPECT_LE(snapshot.buses.size(), 1U) << "one J1939 block publishes one bus";
    return snapshot.buses.empty() ? J1939BusView{} : std::move(snapshot.buses.front());
}

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
    EXPECT_TRUE(busOf(network.snapshot()).nodes.empty());
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
    ASSERT_TRUE(busOf(snapshot).nodes.size() == 1U);
    EXPECT_TRUE(busOf(snapshot).nodes[0].address == kEngine);
    EXPECT_TRUE(snapshot.revision == afterFirst);

    // A second ECU is a change, so it is handed over.
    const std::array<CanFrame, 1> second{
        frameOf(j1939Identifier(kTemperature, kOtherEngine), {60U, 0U, 0U, 0U, 0U, 0U, 0U, 0U})};
    driver.run(second);

    EXPECT_TRUE(network.revision() > afterFirst);
    EXPECT_TRUE(busOf(network.snapshot()).nodes.size() == 2U);
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
    ASSERT_TRUE(busOf(snapshot).diagnostics.size() == 1U);
    ASSERT_TRUE(busOf(snapshot).diagnostics[0].faults.size() == 1U);
    EXPECT_TRUE(busOf(snapshot).diagnostics[0].faults[0].spn == 100U);
}

TEST(J1939NodeTests, ClearingForANewMeasurementIsAChangeAPanelNotices)
{
    // Not a reset to zero: a panel watching for movement would miss a clear
    // that put the counter back where it already was, and go on showing the
    // previous run's bus.
    J1939Network network;
    network.publish({J1939NetworkNode{}}, {}, {}, {J1939ActiveFault{}}, {kEngine});
    ASSERT_EQ(busOf(network.snapshot()).reporting.size(), 1U);

    const std::uint64_t before = network.revision();
    network.clear();

    EXPECT_TRUE(network.revision() > before);
    EXPECT_TRUE(busOf(network.snapshot()).nodes.empty());

    // What the last run's bus was saying is not what this one's is: neither the codes, nor the ECUs
    // that were repeating a DM1.
    EXPECT_TRUE(busOf(network.snapshot()).activeFaults.empty());
    EXPECT_TRUE(busOf(network.snapshot()).reporting.empty());
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
// The trouble codes of the whole bus
// ---------------------------------------------------------------------------
//
// The lamps above say that something is wrong; this is the list of what. The block works it out on
// every pass against the clock of the bus, because a reader of the published diagnostics cannot:
// those are published when their content changes, so the timestamp in one is when it *changed*, and
// says nothing about whether its ECU is still repeating it.

namespace {

/// A DM1 from `source` carrying one code: its SPN, its FMI and how many times it has occurred.
[[nodiscard]] CanFrame faultFrom(std::uint8_t source,
                                 std::uint8_t lamps,
                                 std::uint32_t spn,
                                 std::uint8_t fmi,
                                 std::uint8_t occurrences,
                                 std::uint64_t atNs)
{
    CanFrame frame =
        frameOf(j1939Identifier(kPgnDm1, source),
                {lamps,
                 0xFFU,
                 static_cast<std::uint8_t>(spn & 0xFFU),
                 static_cast<std::uint8_t>((spn >> 8U) & 0xFFU),
                 static_cast<std::uint8_t>((((spn >> 16U) & 0x07U) << 5U) | (fmi & 0x1FU)),
                 occurrences,
                 0xFFU,
                 0xFFU});
    frame.timestampNs = atNs;
    return frame;
}

/// A block wired to a network, as the engine wires it.
struct NetworkBench final {
    NetworkBench()
        : node{nullptr, "J1939"}
    {
        node.setNetwork(&network);
        EXPECT_TRUE(node.prepare(64U).succeeded());
    }

    J1939Network network;
    J1939Node node;
};

} // namespace

TEST(J1939NodeTests, TheCodesTheBusIsReportingAreHandedOverWithTheLampsTheyCameUnder)
{
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 2> frames{faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond),
                                         faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(frames);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(busOf(snapshot).activeFaults.size(), 2U);

    // ECU by ECU in address order, however they arrived.
    EXPECT_EQ(busOf(snapshot).activeFaults[0].sourceAddress, kEngine);
    EXPECT_EQ(busOf(snapshot).activeFaults[0].code.spn, 100U);
    EXPECT_EQ(busOf(snapshot).activeFaults[0].code.fmi, 1U);
    EXPECT_EQ(busOf(snapshot).activeFaults[0].code.occurrenceCount, 3U);
    EXPECT_TRUE(busOf(snapshot).activeFaults[0].lamps.redStop == J1939LampState::On);

    EXPECT_EQ(busOf(snapshot).activeFaults[1].sourceAddress, kBody);
    EXPECT_EQ(busOf(snapshot).activeFaults[1].code.spn, 4096U);
    EXPECT_TRUE(busOf(snapshot).activeFaults[1].lamps.amberWarning == J1939LampState::On);
    EXPECT_TRUE(busOf(snapshot).activeFaults[1].lamps.redStop != J1939LampState::On);

    // Handed over with the ECUs they name, from the same pass.
    EXPECT_EQ(busOf(snapshot).nodes.size(), 2U);
}

TEST(J1939NodeTests, AnEcuThatReportsNothingHasNothingOnTheList)
{
    // The placeholder a healthy ECU sends is already filtered out of its DM1; what is left of an
    // ECU with nothing wrong is an empty list, not a code numbered zero.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> frames{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)};
    driver.run(frames);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    EXPECT_TRUE(snapshot.published);
    EXPECT_EQ(busOf(snapshot).diagnostics.size(), 1U);
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());
}

TEST(J1939NodeTests, WhatWasWrongIsNotOnTheListOfWhatIsWrong)
{
    NetworkBench bench;
    Driver driver{bench.node};

    CanFrame history = faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond);
    history.identifier = j1939Identifier(kPgnDm2, kEngine);

    const std::array<CanFrame, 1> frames{history};
    driver.run(frames);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    EXPECT_EQ(busOf(snapshot).diagnostics.size(), 1U);
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());
}

TEST(J1939NodeTests, AnEcuThatFallsSilentTakesItsCodesOffTheListAndTheLampsGoOutWithThem)
{
    // Lamps and codes are the same statement about the bus, and are aged by the same rule against
    // the same clock: a list that kept a code after its lamp had gone out would be a dashboard that
    // disagrees with its own cluster.
    SystemVariables variables;
    J1939Network network;

    J1939Node node{nullptr, "J1939"};
    node.setNetwork(&network);
    node.setSystemVariables(&variables);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};

    const std::array<CanFrame, 1> lit{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(lit);
    ASSERT_EQ(busOf(network.snapshot()).activeFaults.size(), 1U);
    ASSERT_GT(variables.value(std::string{kJ1939LampStopVariable}), 0.5);

    // Two seconds on, the body controller is still talking and the engine has not repeated itself:
    // still current.
    const std::array<CanFrame, 1> soon{diagnosticFrom(kPgnDm1, kBody, 0x00U, 3U * kSecond)};
    driver.run(soon);
    EXPECT_EQ(busOf(network.snapshot()).activeFaults.size(), 1U);

    const std::uint64_t before = network.revision();

    const std::array<CanFrame, 1> late{diagnosticFrom(kPgnDm1, kBody, 0x00U, 5U * kSecond)};
    driver.run(late);

    // Gone from the list, and the change was published: a panel is told, not left to find out.
    const J1939NetworkSnapshot snapshot = network.snapshot();
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());
    EXPECT_GT(network.revision(), before);
    EXPECT_LT(variables.value(std::string{kJ1939LampStopVariable}), 0.5);

    // What the engine last said is still what a panel that reads the ECUs finds.
    ASSERT_EQ(busOf(snapshot).diagnostics.size(), 2U);
    EXPECT_EQ(busOf(snapshot).diagnostics[0].faults.size(), 1U);
}

TEST(J1939NodeTests, AnEcuThatComesBackPutsItsCodeBackOnTheList)
{
    // The same DM1 as before, byte for byte: nothing in the message is a change, and the list is.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> first{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(first);

    const std::array<CanFrame, 1> elsewhere{diagnosticFrom(kPgnDm1, kBody, 0x00U, 6U * kSecond)};
    driver.run(elsewhere);
    ASSERT_TRUE(busOf(bench.network.snapshot()).activeFaults.empty());

    const std::array<CanFrame, 1> back{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, 7U * kSecond)};
    driver.run(back);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(busOf(snapshot).activeFaults.size(), 1U);
    EXPECT_EQ(busOf(snapshot).activeFaults[0].code.spn, 100U);
}

TEST(J1939NodeTests, AFaultRepeatedEverySecondIsHandedOverOnceAndNotEveryPass)
{
    // The cost the design is built around: the same ECUs saying the same things for an hour change
    // nothing after the first second, and the list is worked out every pass without being
    // published.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> first{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(first);
    const std::uint64_t settled = bench.network.revision();

    for (std::uint64_t second = 2U; second < 8U; ++second) {
        const std::array<CanFrame, 1> again{
            faultFrom(kEngine, kRedStop, 100U, 1U, 3U, second * kSecond)};
        driver.run(again);
    }

    EXPECT_EQ(bench.network.revision(), settled);
    EXPECT_EQ(busOf(bench.network.snapshot()).activeFaults.size(), 1U);

    // A different code from the same ECU is a change: its new message replaces the old.
    const std::array<CanFrame, 1> another{faultFrom(kEngine, kRedStop, 110U, 0U, 1U, 8U * kSecond)};
    driver.run(another);

    EXPECT_GT(bench.network.revision(), settled);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(busOf(snapshot).activeFaults.size(), 1U);
    EXPECT_EQ(busOf(snapshot).activeFaults[0].code.spn, 110U);
}

TEST(J1939NodeTests, StoppingTheMeasurementEmptiesTheListAndKeepsWhatTheEcusSaid)
{
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> lit{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(lit);
    ASSERT_EQ(busOf(bench.network.snapshot()).activeFaults.size(), 1U);

    const std::uint64_t running = bench.network.revision();
    bench.node.finish();

    // A stopped measurement reports no fault, the same as its lamps - and a panel is told.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());
    EXPECT_GT(bench.network.revision(), running);

    // What the J1939 panel reads afterwards is still there, with the ECUs.
    EXPECT_TRUE(snapshot.published);
    ASSERT_EQ(busOf(snapshot).diagnostics.size(), 1U);
    EXPECT_EQ(busOf(snapshot).diagnostics[0].faults.size(), 1U);
    EXPECT_EQ(busOf(snapshot).nodes.size(), 1U);

    // Stopping a block that had nothing to report publishes nothing.
    NetworkBench quiet;
    quiet.node.finish();
    EXPECT_EQ(quiet.network.revision(), 0U);
}

TEST(J1939NodeTests, AMeasurementStartsWithAnEmptyList)
{
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> lit{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(lit);
    ASSERT_FALSE(busOf(bench.network.snapshot()).activeFaults.empty());

    // What the engine does at Start, and the block does at prepare: nothing from the last run.
    bench.network.clear();
    EXPECT_TRUE(busOf(bench.network.snapshot()).activeFaults.empty());

    ASSERT_TRUE(bench.node.prepare(64U).succeeded());
    const std::array<CanFrame, 1> healthy{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)};
    driver.run(healthy);

    EXPECT_TRUE(busOf(bench.network.snapshot()).activeFaults.empty());
}

TEST(J1939NodeTests, TheEcusRepeatingADm1AreHandedOverAndTheHealthyOnesAreAmongThem)
{
    // A healthy ECU has no code, so the list of codes cannot name it - and that list is as empty
    // for an ECU that has stopped talking. Without this a panel could not tell one that says it is
    // fine from one that has said nothing for a while.
    NetworkBench bench;
    Driver driver{bench.node};

    // The engine has a code, the body controller is healthy, and the brakes have only ever answered
    // a request: a DM2 is history, it is not repeated, and is not a reason to be reporting.
    const std::array<CanFrame, 3> frames{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond),
                                         diagnosticFrom(kPgnDm1, kBody, 0x00U, kSecond),
                                         diagnosticFrom(kPgnDm2, kBrakes, kRedStop, kSecond)};
    driver.run(frames);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();

    ASSERT_EQ(busOf(snapshot).reporting.size(), 2U);
    EXPECT_EQ(busOf(snapshot).reporting[0], kEngine);
    EXPECT_EQ(busOf(snapshot).reporting[1], kBody);

    EXPECT_EQ(busOf(snapshot).activeFaults.size(), 1U);
    EXPECT_EQ(busOf(snapshot).diagnostics.size(), 3U);
}

TEST(J1939NodeTests, AHealthyEcuThatGoesQuietLeavesTheReportingAndAPanelIsToldSo)
{
    // Nothing leaves the list of codes - there was no code on it - and that is the change a panel
    // that shows what each ECU last said has to hear of: the "none" of that ECU is the last thing
    // it said and not something it is saying.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 2> first{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond),
                                        diagnosticFrom(kPgnDm1, kBody, 0x00U, kSecond)};
    driver.run(first);
    ASSERT_EQ(busOf(bench.network.snapshot()).reporting.size(), 2U);

    // Two seconds on, only the body controller has repeated itself: the engine has missed one.
    const std::array<CanFrame, 1> soon{diagnosticFrom(kPgnDm1, kBody, 0x00U, 3U * kSecond)};
    driver.run(soon);
    EXPECT_EQ(busOf(bench.network.snapshot()).reporting.size(), 2U);

    const std::uint64_t before = bench.network.revision();

    const std::array<CanFrame, 1> late{diagnosticFrom(kPgnDm1, kBody, 0x00U, 5U * kSecond)};
    driver.run(late);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(busOf(snapshot).reporting.size(), 1U);
    EXPECT_EQ(busOf(snapshot).reporting[0], kBody);
    EXPECT_GT(bench.network.revision(), before);

    // What the engine last said is still there, to be shown as that.
    EXPECT_EQ(busOf(snapshot).diagnostics.size(), 2U);
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());

    // And when it speaks again it is reporting again, with the same bytes as before.
    const std::array<CanFrame, 1> back{diagnosticFrom(kPgnDm1, kEngine, 0x00U, 6U * kSecond)};
    driver.run(back);

    EXPECT_EQ(busOf(bench.network.snapshot()).reporting.size(), 2U);
}

TEST(J1939NodeTests, EcusRepeatingTheirDm1AreHandedOverOnceAndNotEveryPass)
{
    // The same cost the list of codes is built around: the reporting is worked out on every pass
    // and published when it differs, and six healthy ECUs saying the same thing for an hour are one
    // publication.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 2> first{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond),
                                        diagnosticFrom(kPgnDm1, kBody, 0x00U, kSecond)};
    driver.run(first);
    const std::uint64_t settled = bench.network.revision();

    for (std::uint64_t second = 2U; second < 12U; ++second) {
        const std::array<CanFrame, 2> again{
            diagnosticFrom(kPgnDm1, kEngine, 0x00U, second * kSecond),
            diagnosticFrom(kPgnDm1, kBody, 0x00U, second * kSecond)};
        driver.run(again);
    }

    EXPECT_EQ(bench.network.revision(), settled);
    EXPECT_EQ(busOf(bench.network.snapshot()).reporting.size(), 2U);
}

TEST(J1939NodeTests, StoppingTheMeasurementTakesTheReportingOffEvenWhenNothingWasWrong)
{
    // A stopped measurement repeats no DM1. The list of codes was empty already, so what has to be
    // published is the other list - and a block that published at Stop only when it had codes would
    // leave a healthy ECU "reporting" for ever.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> healthy{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)};
    driver.run(healthy);
    ASSERT_EQ(busOf(bench.network.snapshot()).reporting.size(), 1U);

    const std::uint64_t running = bench.network.revision();
    bench.node.finish();

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    EXPECT_TRUE(busOf(snapshot).reporting.empty());
    EXPECT_GT(bench.network.revision(), running);

    // What the ECU last said is still what the J1939 panel reads afterwards.
    EXPECT_TRUE(snapshot.published);
    EXPECT_EQ(busOf(snapshot).diagnostics.size(), 1U);
}

TEST(J1939NodeTests, AnEcuThatClaimedItsAddressIsNamedOnTheFaultListByTheShippedFunctionTable)
{
    // The whole path a name takes: an Address Claimed on the wire, the address table, the
    // hand-over, and the table of functions that ships with the program.
    J1939NameTables tables;
    ASSERT_TRUE(
        tables.loadFile(std::filesystem::path{TORQUEBUS_DATA_DIR} / "j1939-names-functions.csv")
            .succeeded());

    J1939Name name;
    name.industryGroup = 1U;
    name.function = 0U;
    name.manufacturerCode = 33U;
    name.identityNumber = 1U;

    std::vector<std::uint8_t> bytes;
    for (std::uint8_t index = 0U; index < kJ1939NameBytes; ++index) {
        bytes.push_back(static_cast<std::uint8_t>((name.value() >> (index * 8U)) & 0xFFU));
    }

    CanFrame claim = frameOf(j1939Identifier(kPgnAddressClaimed, kEngine), bytes);
    claim.timestampNs = kSecond;

    NetworkBench bench;
    Driver driver{bench.node};

    // The engine claimed its address and then reported a fault; the body controller reported one
    // and was never heard claiming anything.
    const std::array<CanFrame, 3> frames{claim,
                                         faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond),
                                         faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond)};
    driver.run(frames);

    const std::vector<DashboardFault> faults = dashboardFaultsOf(bench.network.snapshot(), &tables);
    ASSERT_EQ(faults.size(), 2U);

    EXPECT_EQ(faults[0].sourceAddress, kEngine);
    ASSERT_TRUE(faults[0].ecu.has_value());
    EXPECT_EQ(*faults[0].ecu, "Engine");

    EXPECT_EQ(faults[1].sourceAddress, kBody);
    EXPECT_FALSE(faults[1].ecu.has_value());
}

TEST(J1939NodeTests, AReaderOfTheHandOverNeverSeesHalfOfAPublication)
{
    // The panel reads on the GUI thread what the executor thread publishes. The codes, and the ECUs
    // that are repeating a DM1, are published with the ECUs they name in one step, under one lock;
    // a reader that could see the codes of one publication and the ECUs of another would list a
    // fault for an address nobody was seen at.
    J1939Network network;
    std::atomic<bool> finished{false};

    std::thread writer{[&network, &finished] {
        for (std::uint32_t round = 1U; round <= 20'000U; ++round) {
            const std::size_t count = round % 6U;

            std::vector<J1939NetworkNode> nodes(count);
            std::vector<J1939ActiveFault> faults(count);
            std::vector<std::uint8_t> reporting(count);

            network.publish(std::move(nodes), {}, {}, std::move(faults), std::move(reporting));
        }

        finished.store(true, std::memory_order_release);
    }};

    std::size_t torn = 0U;
    std::size_t reads = 0U;

    while (!finished.load(std::memory_order_acquire)) {
        const J1939NetworkSnapshot snapshot = network.snapshot();
        ++reads;

        if (busOf(snapshot).nodes.size() != busOf(snapshot).activeFaults.size()
            || busOf(snapshot).nodes.size() != busOf(snapshot).reporting.size()) {
            ++torn;
        }
    }

    writer.join();

    EXPECT_GT(reads, 0U);
    EXPECT_EQ(torn, 0U);
}

TEST(J1939NodeTests, ASnapshotCarriesTheRevisionItsContentsAreFrom)
{
    // A panel remembers the revision of the copy it took and asks again when revision() says
    // another. A copy that reported a revision newer than its contents would be taken for the newer
    // publication, and the panel would not ask again - showing one publication behind until
    // something else moved. The window for that is a few nanoseconds wide, so this is a net more
    // than a proof; what makes it hold is that the revision and the contents are read under the one
    // lock a publication is made under.
    J1939Network network;
    std::atomic<bool> finished{false};

    std::thread writer{[&network, &finished] {
        for (std::uint32_t round = 1U; round <= 50'000U; ++round) {
            // The round the contents are from. From a network nobody has published to, the
            // revision that publication gets is that same number.
            J1939NetworkNode node;
            node.framesSeen = round;

            network.publish({node}, {}, {});
        }

        finished.store(true, std::memory_order_release);
    }};

    std::size_t wrong = 0U;
    std::size_t reads = 0U;

    while (!finished.load(std::memory_order_acquire)) {
        const J1939NetworkSnapshot snapshot = network.snapshot();
        ++reads;

        if (!busOf(snapshot).nodes.empty()
            && snapshot.revision != busOf(snapshot).nodes.front().framesSeen) {
            ++wrong;
        }
    }

    writer.join();

    EXPECT_GT(reads, 0U);
    EXPECT_EQ(wrong, 0U);

    // And what is left at the end is the last publication, and says so.
    const J1939NetworkSnapshot last = network.snapshot();
    ASSERT_EQ(busOf(last).nodes.size(), 1U);
    EXPECT_EQ(last.revision, 50'000U);
    EXPECT_EQ(busOf(last).nodes.front().framesSeen, 50'000U);
}

// ---------------------------------------------------------------------------
// A bus that falls silent altogether
// ---------------------------------------------------------------------------
//
// An ECU that stops repeating its DM1 is aged against the newest frame on the bus. That cannot see
// a bus on which nothing arrives at all - every ECU at once, an adapter unplugged - because the
// clock of the bus is its frames and it stopped with them. What the machine's clock is asked is how
// long that has gone on, and never what time it is.

namespace {

/// A block with the machine's clock in the test's hand, wired to a network and to variables.
struct SilenceBench final {
    SilenceBench()
        : node{nullptr, "J1939"}
        , driver{node}
    {
        node.setNetwork(&network);
        node.setSystemVariables(&variables);
        node.setHostClock([this] {
            ++asked;
            return hostNs;
        });

        EXPECT_TRUE(node.prepare(64U).succeeded());
    }

    /// A pass in which nothing arrived.
    void idle() { driver.run(std::span<const CanFrame>{}); }

    /// The engine reports one fault under the red stop lamp, at bus time `seconds`.
    void engineFaultAt(std::uint64_t seconds)
    {
        const std::array<CanFrame, 1> frame{
            faultFrom(kEngine, kRedStop, 100U, 1U, 3U, seconds * kSecond)};
        driver.run(frame);
    }

    [[nodiscard]] bool stopLamp() const
    {
        return variables.value(std::string{kJ1939LampStopVariable}) > 0.5;
    }

    [[nodiscard]] std::size_t listed() const
    {
        return busOf(network.snapshot()).activeFaults.size();
    }

    /// How many ECUs are repeating a DM1, healthy ones included.
    [[nodiscard]] std::size_t reporting() const
    {
        return busOf(network.snapshot()).reporting.size();
    }

    J1939Network network;
    SystemVariables variables;
    J1939Node node;
    Driver driver;

    /// The machine's time, from an arbitrary start, and how often it was asked.
    std::uint64_t hostNs{500U * kSecond};
    int asked{0};
};

} // namespace

TEST(J1939NodeTests, ABusThatFallsSilentAllAtOnceLetsGoOfItsCodesAndItsLampsAfterThreeSeconds)
{
    SilenceBench bench;

    bench.engineFaultAt(1U);
    ASSERT_EQ(bench.listed(), 1U);
    ASSERT_TRUE(bench.stopLamp());

    // The first pass without a frame starts the count: nothing has been quiet for any time yet.
    bench.idle();
    EXPECT_EQ(bench.listed(), 1U);

    // Just short of three seconds of silence: still what the engine last said, and still current.
    bench.hostNs += 2'900'000'000ULL;
    bench.idle();

    EXPECT_EQ(bench.listed(), 1U);
    EXPECT_TRUE(bench.stopLamp());

    const std::uint64_t before = bench.network.revision();

    // Past it: the engine has not repeated its DM1 for longer than the rule allows, and no other
    // ECU is talking either - which is what used to keep the lamp lit until Stop.
    bench.hostNs += 200'000'000ULL;
    bench.idle();

    EXPECT_EQ(bench.listed(), 0U);
    EXPECT_FALSE(bench.stopLamp());

    // A panel is told, and what the engine last said is still what a panel that reads it finds.
    EXPECT_GT(bench.network.revision(), before);
    EXPECT_EQ(busOf(bench.network.snapshot()).diagnostics.size(), 1U);
}

TEST(J1939NodeTests,
     SilenceThatEndsBeforeTheRuleDoesIsNotAnEmptyBusAndTheCountStartsOverWithEveryFrame)
{
    SilenceBench bench;

    bench.engineFaultAt(1U);
    bench.idle();

    bench.hostNs += 2'000'000'000ULL;
    bench.idle();
    ASSERT_EQ(bench.listed(), 1U);

    // The engine repeats itself, and the count of how long the bus has been quiet starts over with
    // it. Four seconds after the first silence began is not four seconds of silence.
    bench.engineFaultAt(3U);
    bench.idle();

    bench.hostNs += 2'000'000'000ULL;
    bench.idle();

    EXPECT_EQ(bench.listed(), 1U);
    EXPECT_TRUE(bench.stopLamp());
}

TEST(J1939NodeTests, AnEcuThatSpeaksAgainAfterTheBusWentSilentIsListedAgain)
{
    SilenceBench bench;

    bench.engineFaultAt(1U);
    bench.idle();

    bench.hostNs += 5'000'000'000ULL;
    bench.idle();
    ASSERT_EQ(bench.listed(), 0U);
    ASSERT_FALSE(bench.stopLamp());

    // The bus's own clock kept running while nobody spoke, and the engine's DM1 - the same bytes as
    // before - comes back on it.
    bench.engineFaultAt(6U);

    EXPECT_EQ(bench.listed(), 1U);
    EXPECT_TRUE(bench.stopLamp());
}

TEST(J1939NodeTests, ABusThatIsSparseButNotSilentLosesNothingBetweenItsMessages)
{
    // A DM1 a second, and the passes between them idle: the ordinary look of a quiet bus. What is
    // asked of the machine's clock between them is a second at most, and a second is not three.
    SilenceBench bench;

    for (std::uint64_t second = 1U; second <= 12U; ++second) {
        bench.engineFaultAt(second);

        bench.idle();
        bench.hostNs += 500'000'000ULL;
        bench.idle();
        bench.hostNs += 500'000'000ULL;
        bench.idle();

        ASSERT_EQ(bench.listed(), 1U) << "second " << second;
        ASSERT_TRUE(bench.stopLamp()) << "second " << second;
    }
}

TEST(J1939NodeTests, TheActiveFaultsTheBlockCountsAreTheOnesTheBusIsReportingNow)
{
    // The Statistics panel shows this number next to the frame counts. An ECU that has gone quiet,
    // or a measurement that has stopped, has stopped reporting its faults, and a counter that went
    // on saying "1" would say that the machine has a fault that nothing is reporting.
    SilenceBench bench;

    const auto counted = [&bench] {
        for (const NodeStatistic& statistic : bench.node.statistics()) {
            if (statistic.label == "Active faults") {
                return statistic.value;
            }
        }

        return std::uint64_t{9999U};
    };

    EXPECT_EQ(counted(), 0U);

    bench.engineFaultAt(1U);
    EXPECT_EQ(counted(), 1U);

    // The bus falls silent: nothing is being reported.
    bench.idle();
    bench.hostNs += 4'000'000'000ULL;
    bench.idle();
    EXPECT_EQ(counted(), 0U);

    // And again, to the end of the measurement.
    bench.engineFaultAt(10U);
    EXPECT_EQ(counted(), 1U);

    bench.node.finish();
    EXPECT_EQ(counted(), 0U);
}

TEST(J1939NodeTests, TheMachinesClockIsAskedOnlyOnAPassWithNoFrameAndSomethingToAge)
{
    SilenceBench bench;

    // Nothing heard, so nothing to go out of date: the clock is not asked.
    bench.idle();
    bench.idle();
    EXPECT_EQ(bench.asked, 0);

    // A pass with frames in it never asks - the contract of a pass is that it does not wait for
    // anything it does not need.
    bench.engineFaultAt(1U);
    EXPECT_EQ(bench.asked, 0);

    bench.idle();
    EXPECT_EQ(bench.asked, 1);

    bench.engineFaultAt(2U);
    EXPECT_EQ(bench.asked, 1);
}

TEST(J1939NodeTests, ABusThatCarriesOnlyErrorFramesIsASilentOne)
{
    // A shorted or bus-off network delivers a batch of error frames every pass. None of them says
    // anything a DM1 could be repeated by, and a lamp that stayed lit through it would be lit for a
    // bus that carries no data at all.
    SilenceBench bench;

    bench.engineFaultAt(1U);
    ASSERT_EQ(bench.listed(), 1U);

    CanFrame error;
    error.error = true;
    error.timestampNs = 2U * kSecond;
    const std::array<CanFrame, 1> storm{error};

    bench.driver.run(storm);
    bench.hostNs += 4'000'000'000ULL;
    bench.driver.run(storm);

    EXPECT_EQ(bench.listed(), 0U);
    EXPECT_FALSE(bench.stopLamp());
}

TEST(J1939NodeTests, TheMachinesClockIsNotAskedOfABusWithNoDm1RepeatingOnIt)
{
    // Nothing is repeating a DM1: no code, no lamp, no ECU. A DM2 is an answer to a request and is
    // never repeated, so one on the bus is nothing that can go out of date either - and ageing only
    // ever takes away, so there is no reason to read a clock.
    SilenceBench bench;

    const std::array<CanFrame, 1> history{diagnosticFrom(kPgnDm2, kEngine, kRedStop, kSecond)};
    bench.driver.run(history);

    for (int pass = 0; pass < 5; ++pass) {
        bench.idle();
    }

    EXPECT_EQ(bench.asked, 0);
}

TEST(J1939NodeTests, ABusOfHealthyEcusThatFallsSilentLetsGoOfItsReportingAfterThreeSeconds)
{
    // The case the list of codes and the lamps cannot show: nothing is listed and nothing is lit,
    // and an ECU that said it was fine goes on being one that is saying so until somebody notices
    // that nothing has said it for a while. The same rule, on the same clock.
    SilenceBench bench;

    const std::array<CanFrame, 1> healthy{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)};
    bench.driver.run(healthy);
    ASSERT_EQ(bench.reporting(), 1U);

    // The first pass without a frame starts the count; just short of three seconds is not past it.
    bench.idle();
    bench.hostNs += 2'900'000'000ULL;
    bench.idle();
    EXPECT_EQ(bench.reporting(), 1U);

    const std::uint64_t before = bench.network.revision();

    bench.hostNs += 200'000'000ULL;
    bench.idle();

    EXPECT_EQ(bench.reporting(), 0U);

    // A panel is told, and what the ECU last said is still what it finds.
    EXPECT_GT(bench.network.revision(), before);
    EXPECT_EQ(busOf(bench.network.snapshot()).diagnostics.size(), 1U);

    // And with nothing left to age, the clock is not read for it any more.
    const int asked = bench.asked;
    bench.idle();
    bench.idle();

    EXPECT_EQ(bench.asked, asked);
}

TEST(J1939NodeTests, AMeasurementStartsWithNobodyReportingWhateverTheLastOneLeft)
{
    SilenceBench bench;

    const std::array<CanFrame, 1> healthy{diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)};
    bench.driver.run(healthy);
    ASSERT_EQ(bench.reporting(), 1U);

    // What the engine does at Start: the network is cleared and the block prepared again. An ECU
    // the last measurement heard is not one this one has, so there is nothing to age and the idle
    // passes that open it do not read the clock.
    bench.network.clear();
    ASSERT_TRUE(bench.node.prepare(64U).succeeded());

    bench.idle();
    bench.idle();

    EXPECT_EQ(bench.asked, 0);
    EXPECT_EQ(bench.reporting(), 0U);
}

TEST(J1939NodeTests, WithNoClockOfItsOwnTheBlockUsesTheMachinesSteadyClock)
{
    // The default, which is what the application runs with: a fault is listed, the bus goes quiet
    // for a moment, and a moment is not three seconds.
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> frame{faultFrom(kEngine, kRedStop, 100U, 1U, 3U, kSecond)};
    driver.run(frame);

    for (int pass = 0; pass < 50; ++pass) {
        driver.run(std::span<const CanFrame>{});
    }

    EXPECT_EQ(busOf(bench.network.snapshot()).activeFaults.size(), 1U);
}

// ---------------------------------------------------------------------------
// More than one bus
// ---------------------------------------------------------------------------
//
// A J1939 block on each of two buses - a truck's powertrain and its body - and one hand-over
// between them and the panels. What a block publishes is its own bus, under the channel its frames
// arrive on, and it leaves the other bus alone; the lamps are both buses' at once, because a
// cluster has one stop lamp. It used to be one view for the whole graph, and whichever block had
// changed last was the bus the panels showed.

namespace {

[[nodiscard]] CanFrame onChannel(CanFrame frame, std::uint8_t channel)
{
    frame.channel = channel;
    return frame;
}

/// Two J1939 blocks, one on CAN 1 and one on CAN 2, wired to one network and one set of variables
/// as the engine wires every J1939 block of a graph - and with the machine's clock in the test's
/// hand.
struct TwoBusBench final {
    TwoBusBench()
        : powertrain{nullptr, "J1939 powertrain"}
        , body{nullptr, "J1939 body"}
        , drivePowertrain{powertrain}
        , driveBody{body}
    {
        for (J1939Node* node : {&powertrain, &body}) {
            node->setNetwork(&network);
            node->setSystemVariables(&variables);
            node->setHostClock([this] { return hostNs; });
            EXPECT_TRUE(node->prepare(64U).succeeded());
        }
    }

    /// One frame on CAN 1, to the block that reads it.
    void onPowertrain(const CanFrame& frame)
    {
        const std::array<CanFrame, 1> frames{onChannel(frame, 0U)};
        drivePowertrain.run(frames);
    }

    /// One frame on CAN 2, to the block that reads it.
    void onBody(const CanFrame& frame)
    {
        const std::array<CanFrame, 1> frames{onChannel(frame, 1U)};
        driveBody.run(frames);
    }

    [[nodiscard]] bool lit(std::string_view lamp) const
    {
        return variables.value(std::string{lamp}) > 0.5;
    }

    J1939Network network;
    SystemVariables variables;
    std::uint64_t hostNs{900U * kSecond};

    J1939Node powertrain;
    J1939Node body;
    Driver drivePowertrain;
    Driver driveBody;
};

} // namespace

TEST(J1939NodeTests, TwoBlocksOnTwoBusesAreTwoBusesInTheHandOver)
{
    TwoBusBench bench;

    bench.onPowertrain(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond));
    bench.onBody(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond));

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);

    // In channel order, each with what its own bus said and nothing of the other's.
    EXPECT_EQ(snapshot.buses[0].channel, 0U);
    ASSERT_EQ(snapshot.buses[0].nodes.size(), 1U);
    EXPECT_EQ(snapshot.buses[0].nodes[0].address, kEngine);
    ASSERT_EQ(snapshot.buses[0].activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.buses[0].activeFaults[0].code.spn, 100U);
    EXPECT_EQ(snapshot.buses[0].reporting, std::vector<std::uint8_t>{kEngine});

    EXPECT_EQ(snapshot.buses[1].channel, 1U);
    ASSERT_EQ(snapshot.buses[1].nodes.size(), 1U);
    EXPECT_EQ(snapshot.buses[1].nodes[0].address, kBody);
    ASSERT_EQ(snapshot.buses[1].activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.buses[1].activeFaults[0].code.spn, 4096U);

    // And found by its channel.
    ASSERT_NE(snapshot.bus(1U), nullptr);
    EXPECT_EQ(snapshot.bus(1U)->nodes.front().address, kBody);
    EXPECT_EQ(snapshot.bus(2U), nullptr);
}

TEST(J1939NodeTests, WhatOneBusPublishesLeavesTheOtherBusAsItWas)
{
    // One publication used to replace the other: the panels showed the bus that had changed last,
    // and the fault list lost the codes of one bus whenever the other one moved.
    TwoBusBench bench;

    bench.onPowertrain(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond));
    bench.onBody(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond));

    // The engine reports another code, and the powertrain publishes again.
    const std::uint64_t before = bench.network.revision();
    bench.onPowertrain(faultFrom(kEngine, kRedStop, 110U, 0U, 1U, 2U * kSecond));
    ASSERT_GT(bench.network.revision(), before);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);
    ASSERT_NE(snapshot.bus(0U), nullptr);
    ASSERT_EQ(snapshot.bus(0U)->activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults[0].code.spn, 110U);

    ASSERT_NE(snapshot.bus(1U), nullptr);
    ASSERT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.bus(1U)->activeFaults[0].code.spn, 4096U);
}

TEST(J1939NodeTests, ABlockFilesItsBusUnderTheChannelItsFramesArriveOn)
{
    NetworkBench bench;
    Driver driver{bench.node};

    const std::array<CanFrame, 1> frames{
        onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 3U)};
    driver.run(frames);

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 1U);
    EXPECT_EQ(snapshot.buses[0].channel, 3U);
}

TEST(J1939NodeTests, TwoBlocksOnOneChannelReadOneBus)
{
    // Two databases on the same wire: the same ECUs, and one bus to the panels - not two copies of
    // it, which would list every code twice.
    J1939Network network;
    J1939Node first{nullptr, "J1939 a"};
    J1939Node second{nullptr, "J1939 b"};

    for (J1939Node* node : {&first, &second}) {
        node->setNetwork(&network);
        ASSERT_TRUE(node->prepare(64U).succeeded());
    }

    Driver driveFirst{first};
    Driver driveSecond{second};

    const std::array<CanFrame, 1> frames{faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond)};
    driveFirst.run(frames);
    driveSecond.run(frames);

    const J1939NetworkSnapshot snapshot = network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 1U);
    EXPECT_EQ(snapshot.buses[0].nodes.size(), 1U);
    EXPECT_EQ(snapshot.buses[0].activeFaults.size(), 1U);
}

TEST(J1939NodeTests, TheLampsAreEveryBussLampsAtOnce)
{
    TwoBusBench bench;

    // The engine lights the stop lamp on CAN 1 and the body controller the amber one on CAN 2.
    bench.onPowertrain(diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond));
    bench.onBody(diagnosticFrom(kPgnDm1, kBody, kAmberWarning, kSecond));
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));

    // The body's amber goes out, and the engine's red does not go with it.
    bench.onBody(diagnosticFrom(kPgnDm1, kBody, 0x00U, 2U * kSecond));
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_FALSE(bench.lit(kJ1939LampWarningVariable));

    // Both buses ask for the stop lamp, and one of them stops asking: still lit, for the other.
    bench.onBody(diagnosticFrom(kPgnDm1, kBody, kRedStop, 3U * kSecond));
    bench.onPowertrain(diagnosticFrom(kPgnDm1, kEngine, 0x00U, 3U * kSecond));
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));

    // Then the other: out.
    bench.onBody(diagnosticFrom(kPgnDm1, kBody, 0x00U, 4U * kSecond));
    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));

    // And lit again by the engine - whose block last saw the lamp lit, and must not take what it
    // saw then for what the lamp says now: the other block has put it out since.
    bench.onPowertrain(diagnosticFrom(kPgnDm1, kEngine, kRedStop, 5U * kSecond));
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
}

TEST(J1939NodeTests, ABusThatFallsSilentLetsGoOfItsOwnCodesAndLampsAndNotTheOthers)
{
    TwoBusBench bench;

    bench.onPowertrain(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond));
    bench.onBody(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond));

    // CAN 1 falls silent - its adapter unplugged - while CAN 2 goes on talking.
    for (std::uint64_t second = 2U; second <= 5U; ++second) {
        bench.drivePowertrain.run(std::span<const CanFrame>{});
        bench.hostNs += kSecond;
        bench.onBody(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, second * kSecond));
    }
    bench.drivePowertrain.run(std::span<const CanFrame>{});

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);

    // The silent bus has let go of what it was saying; the other has not been touched.
    EXPECT_TRUE(snapshot.bus(0U)->activeFaults.empty());
    EXPECT_TRUE(snapshot.bus(0U)->reporting.empty());
    EXPECT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);

    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));
}

TEST(J1939NodeTests, StoppingPutsOutTheLampsOfEveryBusAsEachBlockFinishes)
{
    TwoBusBench bench;

    bench.onPowertrain(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond));
    bench.onBody(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond));

    // The engine finishes its blocks one at a time: the bus whose block has not finished yet still
    // lights what it lights, until it has.
    bench.body.finish();
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_FALSE(bench.lit(kJ1939LampWarningVariable));

    bench.powertrain.finish();
    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));

    // Both buses are still there for the J1939 panel to read, and neither reports anything.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);

    for (const J1939BusView& bus : snapshot.buses) {
        EXPECT_TRUE(bus.activeFaults.empty()) << "CAN " << bus.channel + 1;
        EXPECT_TRUE(bus.reporting.empty()) << "CAN " << bus.channel + 1;
        EXPECT_EQ(bus.diagnostics.size(), 1U) << "CAN " << bus.channel + 1;
    }
}

TEST(J1939NodeTests, TheBusesOfASnapshotAreInChannelOrderWhicheverPublishedFirst)
{
    // CAN 2 is heard first - its ECUs were on before the measurement's first pass reached CAN 1 -
    // and a panel still lists CAN 1 above it.
    J1939Network network;

    J1939NetworkNode body;
    body.address = kBody;
    network.publish({body}, {}, {}, {}, {}, 1U);

    J1939NetworkNode engine;
    engine.address = kEngine;
    network.publish({engine}, {}, {}, {}, {}, 0U);

    const J1939NetworkSnapshot snapshot = network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);
    EXPECT_EQ(snapshot.buses[0].channel, 0U);
    EXPECT_EQ(snapshot.buses[0].nodes.front().address, kEngine);
    EXPECT_EQ(snapshot.buses[1].channel, 1U);
    EXPECT_EQ(snapshot.buses[1].nodes.front().address, kBody);
}

TEST(J1939NodeTests, ClearingForgetsEveryBusAndWhatEveryBlockLit)
{
    J1939Network network;
    network.publish({J1939NetworkNode{}}, {}, {}, {}, {}, 0U);
    network.publish({J1939NetworkNode{}}, {}, {}, {}, {}, 1U);
    ASSERT_EQ(network.snapshot().buses.size(), 2U);

    // A block of the last measurement left the stop lamp lit.
    const int lastRun = 0;
    ASSERT_EQ(network.combineLamps(&lastRun, {1, 0, 0, 0}),
              (std::array<std::int8_t, 4>{1, 0, 0, 0}));

    network.clear();
    EXPECT_TRUE(network.snapshot().buses.empty());

    // A block of the new one that lights nothing is all there is: the last run's lamp is gone.
    const int thisRun = 0;
    EXPECT_EQ(network.combineLamps(&thisRun, {0, 0, 0, 0}),
              (std::array<std::int8_t, 4>{0, 0, 0, 0}));
}

TEST(J1939NodeTests, TwoJ1939BlocksOfOneGraphReportTheirOwnBusesThroughTheEngine)
{
    // The whole path, as a project builds it: a CAN Source and a J1939 block on each of two
    // channels, the catalog handing both blocks the engine's one network and its variables.
    CanEngine engine;

    for (std::uint8_t channel = 0U; channel < 2U; ++channel) {
        CanChannelConfig config;
        config.deviceHandle = "virtual:" + std::to_string(channel);
        config.applicationChannel = channel;
        config.timing.bitrate = 250'000;
        ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());
    }

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "can_1",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "can_2",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(1)}},
    });
    description.addNode(NodeDescription{.id = "j1939_powertrain", .typeName = "j1939.decoder"});
    description.addNode(NodeDescription{.id = "j1939_body", .typeName = "j1939.decoder"});
    description.addEdge(EdgeDescription{"can_1", 0, "j1939_powertrain", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "j1939_body", 0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    ASSERT_TRUE(description.validate(catalog).succeeded());
    engine.setGraphDescription(description, catalog);

    const Result started = engine.start();
    ASSERT_TRUE(started.succeeded()) << std::string{started.message()};

    const auto waitUntil = [](const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return done();
    };

    const auto lamp = [&engine](std::string_view name) {
        return engine.variables().value(std::string{name}) > 0.5;
    };

    // The engine lights the stop lamp on CAN 1 and the body controller the amber one on CAN 2.
    ASSERT_TRUE(
        engine.transmit(0, diagnosticFrom(kPgnDm1, kEngine, kRedStop, kSecond)).succeeded());
    ASSERT_TRUE(
        engine.transmit(1, diagnosticFrom(kPgnDm1, kBody, kAmberWarning, kSecond)).succeeded());

    EXPECT_TRUE(waitUntil([&] {
        return engine.j1939Network().snapshot().buses.size() == 2U && lamp(kJ1939LampStopVariable)
               && lamp(kJ1939LampWarningVariable);
    }));

    const J1939NetworkSnapshot both = engine.j1939Network().snapshot();
    ASSERT_EQ(both.buses.size(), 2U);
    ASSERT_NE(both.bus(0U), nullptr);
    ASSERT_NE(both.bus(1U), nullptr);
    EXPECT_EQ(both.bus(0U)->nodes.front().address, kEngine);
    EXPECT_EQ(both.bus(1U)->nodes.front().address, kBody);

    // The engine's lamp goes out on CAN 1; the body's on CAN 2 stays lit.
    ASSERT_TRUE(engine.transmit(0, diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond)).succeeded());

    EXPECT_TRUE(waitUntil([&] { return !lamp(kJ1939LampStopVariable); }));
    EXPECT_TRUE(lamp(kJ1939LampWarningVariable));

    engine.stop();

    EXPECT_FALSE(lamp(kJ1939LampWarningVariable));
    EXPECT_EQ(engine.j1939Network().snapshot().buses.size(), 2U);
}

// ---------------------------------------------------------------------------
// One block, two buses
// ---------------------------------------------------------------------------
//
// A recording of two buses, replayed into one J1939 block, is one stream with both buses in it:
// the frames say which they are on, and an address is a seat on one bus. The ECU at 0 on CAN 1 and
// the ECU at 0 on CAN 2 are two ECUs, with two NAMEs and two lists of faults; a transfer under way
// on one bus is not finished by the packets of the other; and each bus is silent or talking in its
// own right, on a clock of its own. The block used to read the two as one bus, under the channel of
// the first frame it saw.

namespace {

/// An Address Claimed from `source` on `channel`, for an ECU whose function is `function`.
[[nodiscard]] CanFrame
claimOnChannel(std::uint8_t source, std::uint8_t function, std::uint8_t channel, std::uint64_t atNs)
{
    J1939Name name;
    name.industryGroup = 1U;
    name.function = function;
    name.identityNumber = source;

    std::vector<std::uint8_t> bytes(kJ1939NameBytes);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((name.value() >> (8U * index)) & 0xFFU);
    }

    CanFrame frame = frameOf(j1939Identifier(kPgnAddressClaimed, source), std::move(bytes));
    frame.channel = channel;
    frame.timestampNs = atNs;
    return frame;
}

/// One J1939 block, as the engine wires one - a network, the system variables - and the machine's
/// clock in the test's hand. Frames of any number of buses go in through run().
struct OneBlockBench final {
    OneBlockBench()
        : node{nullptr, "J1939"}
        , driver{node}
    {
        node.setNetwork(&network);
        node.setSystemVariables(&variables);
        node.setHostClock([this] { return hostNs; });
        EXPECT_TRUE(node.prepare(64U).succeeded());
    }

    /// One pass of the block with these frames in it, in this order.
    void run(const std::vector<CanFrame>& frames) { driver.run(frames); }

    /// One pass with nothing in it.
    void idle() { driver.run(std::span<const CanFrame>{}); }

    [[nodiscard]] bool lit(std::string_view lamp) const
    {
        return variables.value(std::string{lamp}) > 0.5;
    }

    [[nodiscard]] std::uint64_t statistic(std::string_view label) const
    {
        for (const NodeStatistic& statistic : node.statistics()) {
            if (statistic.label == label) {
                return statistic.value;
            }
        }

        return ~std::uint64_t{0};
    }

    J1939Network network;
    SystemVariables variables;
    std::uint64_t hostNs{900U * kSecond};

    J1939Node node;
    Driver driver;
};

} // namespace

TEST(J1939NodeTests, TheSameAddressOnTwoBusesIsTwoEcusAndNotAContest)
{
    OneBlockBench bench;

    // The engine at 0 on CAN 1, and a body controller that also sits at 0 on CAN 2.
    bench.run(
        {claimOnChannel(kEngine, 0U, 0U, kSecond), claimOnChannel(kEngine, 26U, 1U, kSecond)});

    EXPECT_EQ(bench.node.channels(), (std::vector<std::uint8_t>{0U, 1U}));

    // Each seat has the ECU that claimed it on its own bus. Read as one bus these are one address
    // contested, and one of two ECUs loses it.
    ASSERT_EQ(bench.node.addresses(0U).nodes().size(), 1U);
    ASSERT_EQ(bench.node.addresses(1U).nodes().size(), 1U);
    ASSERT_TRUE(bench.node.addresses(0U).nodes()[0].name.has_value());
    ASSERT_TRUE(bench.node.addresses(1U).nodes()[0].name.has_value());
    EXPECT_EQ(bench.node.addresses(0U).nodes()[0].name->function, 0U);
    EXPECT_EQ(bench.node.addresses(1U).nodes()[0].name->function, 26U);

    // And the hand-over carries both, each under its own channel.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);
    ASSERT_NE(snapshot.bus(0U), nullptr);
    ASSERT_NE(snapshot.bus(1U), nullptr);
    EXPECT_EQ(snapshot.bus(0U)->nodes[0].name->function, 0U);
    EXPECT_EQ(snapshot.bus(1U)->nodes[0].name->function, 26U);

    // Two ECUs, because there are two.
    EXPECT_EQ(bench.statistic("ECUs seen"), 2U);
}

TEST(J1939NodeTests, TheDm1sOfTheSameAddressOnTwoBusesAreNotEachOthersAndTheLampsAreBoth)
{
    OneBlockBench bench;

    // CAN 1's engine lights the stop lamp; the ECU at 0 on CAN 2 says it is fine.
    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U),
               onChannel(diagnosticFrom(kPgnDm1, kEngine, 0x00U, kSecond), 1U)});

    J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults.size(), 1U);
    EXPECT_TRUE(snapshot.bus(1U)->activeFaults.empty());
    EXPECT_EQ(snapshot.bus(0U)->reporting, std::vector<std::uint8_t>{kEngine});
    EXPECT_EQ(snapshot.bus(1U)->reporting, std::vector<std::uint8_t>{kEngine});
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_FALSE(bench.lit(kJ1939LampWarningVariable));

    // CAN 2's ECU lights the amber lamp while CAN 1's goes on saying red: both are lit.
    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, 2U * kSecond), 0U),
               onChannel(faultFrom(kEngine, kAmberWarning, 110U, 0U, 1U, 2U * kSecond), 1U)});

    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));
    EXPECT_EQ(bench.statistic("Active faults"), 2U);

    // CAN 1's ECU recovers: its lamp goes out and the other bus's stays.
    bench.run({onChannel(diagnosticFrom(kPgnDm1, kEngine, 0x00U, 3U * kSecond), 0U),
               onChannel(faultFrom(kEngine, kAmberWarning, 110U, 0U, 1U, 3U * kSecond), 1U)});

    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));
    EXPECT_EQ(bench.statistic("Active faults"), 1U);
}

TEST(J1939NodeTests, EachBusOfABlockIsAgedByItsOwnClock)
{
    OneBlockBench bench;

    // Two interfaces stamp their frames with clocks of their own, and a recording of both keeps
    // each. CAN 2's is four thousand seconds ahead of CAN 1's.
    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U),
               onChannel(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, 5000U * kSecond), 1U)});
    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, 2U * kSecond), 0U),
               onChannel(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, 5001U * kSecond), 1U)});

    // Both repeat their DM1 a second apart on their own clocks, and so both are reporting. Aged by
    // the newest time of either, the engine's would be an hour out of date and the lamp gone out.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);
    EXPECT_TRUE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));
}

TEST(J1939NodeTests, ABusThatFallsSilentWhileTheOtherTalksIsSilentAndLetsGoOfItsLamp)
{
    OneBlockBench bench;

    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U),
               onChannel(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, 5000U * kSecond), 1U)});
    ASSERT_TRUE(bench.lit(kJ1939LampStopVariable));
    ASSERT_TRUE(bench.lit(kJ1939LampWarningVariable));

    // From here only CAN 2 talks, a second at a time. The pass that has its frames in it is not a
    // pass in which CAN 1 said anything, and the machine's clock says how long it has not.
    for (std::uint64_t second = 1U; second <= 6U; ++second) {
        bench.hostNs += kSecond;
        bench.run({onChannel(
            faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, (5000U + second) * kSecond), 1U)});
    }

    // CAN 1's engine has not repeated its DM1 for six seconds: its code and its lamp are gone, and
    // CAN 2's, which is still repeating, is not.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    EXPECT_TRUE(snapshot.bus(0U)->activeFaults.empty());
    EXPECT_TRUE(snapshot.bus(0U)->reporting.empty());
    EXPECT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);
    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));
    EXPECT_TRUE(bench.lit(kJ1939LampWarningVariable));

    // What the silent ECU last said is still what a panel finds.
    EXPECT_EQ(snapshot.bus(0U)->diagnostics.size(), 1U);
}

TEST(J1939NodeTests, ATransferOnOneBusIsNotFinishedByThePacketsOfAnother)
{
    OneBlockBench bench;

    // The engine at 0 on CAN 1 begins a DM1 of three codes: the announcement and the first packet.
    bench.run({onChannel(bamFrame(kPgnDm1, 14U, 2U, kEngine), 0U),
               onChannel(dataFrame(1U, {0x40U, 0x00U, 100U, 0U, 1U, 2U, 110U}, kEngine), 0U)});

    // The second packet comes from the ECU at 0 on CAN 2, which is another ECU and not the end of
    // this transfer.
    const CanFrame second = dataFrame(2U, {0U, 3U, 1U, 190U, 0U, 4U, 7U}, kEngine);
    bench.run({onChannel(second, 1U)});

    EXPECT_TRUE(bench.node.diagnostics(0U).empty());
    EXPECT_TRUE(bench.node.diagnostics(1U).empty());

    // From CAN 1 it is.
    bench.run({onChannel(second, 0U)});

    ASSERT_EQ(bench.node.diagnostics(0U).size(), 1U);
    EXPECT_EQ(bench.node.diagnostics(0U)[0].faults.size(), 3U);
    EXPECT_TRUE(bench.node.diagnostics(1U).empty());
}

TEST(J1939NodeTests, StoppingPutsOutTheLampsOfEveryBusOfABlockAndKeepsWhatTheyLastSaid)
{
    OneBlockBench bench;

    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U),
               onChannel(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond), 1U)});
    ASSERT_TRUE(bench.lit(kJ1939LampStopVariable));
    ASSERT_TRUE(bench.lit(kJ1939LampWarningVariable));

    bench.node.finish();

    EXPECT_FALSE(bench.lit(kJ1939LampStopVariable));
    EXPECT_FALSE(bench.lit(kJ1939LampWarningVariable));

    // Neither bus reports anything any more, and both still say what their ECUs last said.
    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 2U);

    for (const J1939BusView& bus : snapshot.buses) {
        EXPECT_TRUE(bus.activeFaults.empty());
        EXPECT_TRUE(bus.reporting.empty());
        EXPECT_EQ(bus.diagnostics.size(), 1U);
    }
}

TEST(J1939NodeTests, ABusIsMadeFromItsFirstFrameAndTheBusesAreInChannelOrder)
{
    OneBlockBench bench;

    // Nothing heard, so no bus: the accessors answer "nobody" and not a null pointer.
    EXPECT_TRUE(bench.node.channels().empty());
    EXPECT_TRUE(bench.node.addresses(0U).nodes().empty());
    EXPECT_TRUE(bench.node.diagnostics(0U).empty());
    EXPECT_TRUE(bench.node.addresses().nodes().empty());
    EXPECT_TRUE(bench.node.diagnostics().empty());

    // The channels come in any order, and the buses are kept in the order of their numbers.
    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 3U)});
    bench.run({onChannel(faultFrom(kBody, kAmberWarning, 4096U, 4U, 1U, kSecond), 1U)});
    bench.run({onChannel(diagnosticFrom(kPgnDm1, kBrakes, 0x00U, kSecond), 2U)});

    EXPECT_EQ(bench.node.channels(), (std::vector<std::uint8_t>{1U, 2U, 3U}));

    // A channel nothing was heard from has no bus in the block or in the hand-over.
    EXPECT_TRUE(bench.node.addresses(0U).nodes().empty());

    const J1939NetworkSnapshot snapshot = bench.network.snapshot();
    ASSERT_EQ(snapshot.buses.size(), 3U);
    EXPECT_EQ(snapshot.buses[0].channel, 1U);
    EXPECT_EQ(snapshot.buses[1].channel, 2U);
    EXPECT_EQ(snapshot.buses[2].channel, 3U);

    // And the block's own accessors without a channel are about the first, which is CAN 2's.
    ASSERT_EQ(bench.node.addresses().nodes().size(), 1U);
    EXPECT_EQ(bench.node.addresses().nodes()[0].address, kBody);
    ASSERT_EQ(bench.node.diagnostics().size(), 1U);
    EXPECT_EQ(bench.node.diagnostics()[0].sourceAddress, kBody);
}

TEST(J1939NodeTests, ABusThatTalksInFramesThatAreNotJ1939IsStillABusThatTalks)
{
    // An 11-bit frame says nothing J1939 and says the bus is alive: it is a frame, and the clock
    // of the bus ran. A bus with nothing but those reports nothing, and is not made to look silent.
    OneBlockBench bench;

    CanFrame standard;
    standard.identifier = 0x7E0U;
    standard.format = CanFrameFormat::Standard;
    standard.length = 8U;
    standard.dlc = 8U;
    standard.timestampNs = kSecond;
    standard.channel = 1U;

    bench.run({onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U), standard});

    EXPECT_EQ(bench.node.channels(), (std::vector<std::uint8_t>{0U, 1U}));
    EXPECT_TRUE(bench.node.addresses(1U).nodes().empty());

    // Nothing of CAN 2's was published: it has no ECU and no code to show.
    EXPECT_EQ(bench.network.snapshot().buses.size(), 1U);
}

TEST(J1939NodeTests, OneBlockReadingARecordingOfTwoBusesReportsBothThroughTheEngine)
{
    // The whole path: a recording with two buses in it, replayed into one J1939 block. The log
    // keeps the channel of each frame, so the block gets one stream with both buses in it.
    //
    // A file of its own, unique to this process, that goes with the test however it ends.
    struct Recording final {
        std::filesystem::path path{
            torquebus::tests::uniqueTempPath("torquebus_two_buses", 1, ".tblog")};

        ~Recording()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } recording;

    const std::filesystem::path& path = recording.path;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(path.string()).succeeded());

        const std::array<CanFrame, 4> recorded{
            onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond), 0U),
            onChannel(faultFrom(kEngine, kAmberWarning, 4096U, 4U, 1U, kSecond), 1U),
            onChannel(faultFrom(kEngine, kRedStop, 100U, 1U, 1U, 2U * kSecond), 0U),
            onChannel(faultFrom(kEngine, kAmberWarning, 4096U, 4U, 1U, 2U * kSecond), 1U)};
        ASSERT_TRUE(writer.append(recorded).succeeded());
        ASSERT_TRUE(writer.flush().succeeded());
    }

    CanEngine engine;

    // A replay needs no channel of its own; the engine wants one to run.
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.applicationChannel = 0U;
    config.timing.bitrate = 250'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "replay",
        .typeName = "log.source",
        .parameters = {{"path", ParameterValue::fromText(path.string())},
                       {"speed", ParameterValue::fromReal(1000.0)}},
    });
    description.addNode(NodeDescription{.id = "j1939", .typeName = "j1939.decoder"});
    description.addEdge(EdgeDescription{"replay", 0, "j1939", 0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    ASSERT_TRUE(description.validate(catalog).succeeded());
    engine.setGraphDescription(description, catalog);

    const Result started = engine.start();
    ASSERT_TRUE(started.succeeded()) << std::string{started.message()};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    J1939NetworkSnapshot snapshot;

    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = engine.j1939Network().snapshot();

        if (snapshot.buses.size() == 2U && snapshot.bus(0U) != nullptr
            && snapshot.bus(1U) != nullptr && !snapshot.bus(0U)->activeFaults.empty()
            && !snapshot.bus(1U)->activeFaults.empty()) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    engine.stop();

    ASSERT_EQ(snapshot.buses.size(), 2U);
    ASSERT_NE(snapshot.bus(0U), nullptr);
    ASSERT_NE(snapshot.bus(1U), nullptr);

    // The same address on both, and a different code on each.
    ASSERT_EQ(snapshot.bus(0U)->activeFaults.size(), 1U);
    ASSERT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults[0].sourceAddress, kEngine);
    EXPECT_EQ(snapshot.bus(1U)->activeFaults[0].sourceAddress, kEngine);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults[0].code.spn, 100U);
    EXPECT_EQ(snapshot.bus(1U)->activeFaults[0].code.spn, 4096U);
}

TEST(J1939NodeTests, OneBlockReadingTwoLiveBusesThroughAFrameMergeReportsBothThroughTheEngine)
{
    // The whole path with live buses, as a project draws it: a CAN Channel on each of two channels,
    // a Frame Merge, and one J1939 block behind it. An input takes one wire, so before the merge
    // two live buses were two blocks - and the recording above was the only way to one.
    CanEngine engine;

    for (std::uint8_t channel = 0U; channel < 2U; ++channel) {
        CanChannelConfig config;
        config.deviceHandle = "virtual:" + std::to_string(channel);
        config.applicationChannel = channel;
        config.timing.bitrate = 250'000;
        ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());
    }

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "can_1",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "can_2",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(1)}},
    });
    description.addNode(NodeDescription{.id = "merge", .typeName = "can.merge"});
    description.addNode(NodeDescription{.id = "j1939", .typeName = "j1939.decoder"});
    description.addEdge(EdgeDescription{"can_1", 0, "merge", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "merge", 1});
    description.addEdge(EdgeDescription{"merge", 0, "j1939", 0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    ASSERT_TRUE(description.validate(catalog).succeeded());
    engine.setGraphDescription(description, catalog);

    const Result started = engine.start();
    ASSERT_TRUE(started.succeeded()) << std::string{started.message()};

    // The same address on both buses and a different code on each, and an ECU that is only on
    // CAN 2.
    ASSERT_TRUE(
        engine.transmit(0, faultFrom(kEngine, kRedStop, 100U, 1U, 1U, kSecond)).succeeded());
    ASSERT_TRUE(
        engine.transmit(1, faultFrom(kEngine, kAmberWarning, 4096U, 4U, 1U, kSecond)).succeeded());
    ASSERT_TRUE(engine.transmit(1, diagnosticFrom(kPgnDm1, kBody, 0x00U, kSecond)).succeeded());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    J1939NetworkSnapshot snapshot;

    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = engine.j1939Network().snapshot();

        if (snapshot.buses.size() == 2U && snapshot.bus(0U) != nullptr
            && snapshot.bus(1U) != nullptr && snapshot.bus(0U)->nodes.size() == 1U
            && snapshot.bus(1U)->nodes.size() == 2U && !snapshot.bus(0U)->activeFaults.empty()
            && !snapshot.bus(1U)->activeFaults.empty()) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    // What the merge said of itself, while the measurement is still running: the statistics are
    // taken a window at a time, so they are waited for as well.
    const auto passedByTheMerge = [&engine]() -> std::uint64_t {
        for (const CanEngine::NodeReport& report : engine.nodeStatistics()) {
            if (report.typeName == "can.merge") {
                for (const NodeStatistic& statistic : report.statistics) {
                    if (statistic.label == "Frames passed") {
                        return statistic.value;
                    }
                }
            }
        }

        return 0U;
    };

    while (passedByTheMerge() < 3U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    const std::uint64_t merged = passedByTheMerge();

    engine.stop();

    ASSERT_EQ(snapshot.buses.size(), 2U);
    ASSERT_NE(snapshot.bus(0U), nullptr);
    ASSERT_NE(snapshot.bus(1U), nullptr);

    // Each ECU is on the bus its frames came from: the engine is at 0 on both, and the body
    // controller is on CAN 2 only.
    ASSERT_EQ(snapshot.bus(0U)->nodes.size(), 1U);
    ASSERT_EQ(snapshot.bus(1U)->nodes.size(), 2U);
    EXPECT_EQ(snapshot.bus(0U)->nodes[0].address, kEngine);
    EXPECT_EQ(snapshot.bus(1U)->nodes[0].address, kEngine);
    EXPECT_EQ(snapshot.bus(1U)->nodes[1].address, kBody);

    ASSERT_EQ(snapshot.bus(0U)->activeFaults.size(), 1U);
    ASSERT_EQ(snapshot.bus(1U)->activeFaults.size(), 1U);
    EXPECT_EQ(snapshot.bus(0U)->activeFaults[0].code.spn, 100U);
    EXPECT_EQ(snapshot.bus(1U)->activeFaults[0].code.spn, 4096U);

    // And the merge counted what went through it - three frames, though the other two buses may
    // have shown more, and at least the ones sent.
    EXPECT_GE(merged, 3U);
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

TEST(J1939NodeTests, ThreeBitAndSixtyFourBitSpecialValuesAndUnsigned64BitScaling)
{
    // 3-bit SAE J1939-71 §5.1.4: 0b110 (6) = Error, 0b111 (7) = Not Available.
    EXPECT_FALSE(isJ1939SpecialValue(5U, 3U));
    EXPECT_TRUE(isJ1939SpecialValue(6U, 3U));
    EXPECT_TRUE(isJ1939SpecialValue(7U, 3U));

    // 64-bit SAE J1939-71 §5.1.4: >= 0xFE00000000000000ULL is special.
    EXPECT_FALSE(isJ1939SpecialValue(0x8000'0000'0000'0000ULL, 64U));
    EXPECT_TRUE(isJ1939SpecialValue(0xFE00'0000'0000'0000ULL, 64U));
    EXPECT_TRUE(isJ1939SpecialValue(0xFFFF'FFFF'FFFF'FFFFULL, 64U));

    // 64-bit unsigned signal with bit 63 set must decode as positive, not negative.
    CanSignal u64;
    u64.name = "Counter64";
    u64.bitLength = 64U;
    u64.isSigned = false;
    u64.factor = 1.0;

    J1939Node node{databaseWithSignal(u64), "J1939"};
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> highBitFrame{
        frameOf(j1939Identifier(kTemperature, kEngine), {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x80U})};
    const std::span<const DecodedSignal> decoded = driver.run(highBitFrame);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_GT(decoded[0].value, 0.0);

    const std::array<CanFrame, 1> naFrame{
        frameOf(j1939Identifier(kTemperature, kEngine),
                {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU})};
    const std::span<const DecodedSignal> naDecoded = driver.run(naFrame);
    ASSERT_EQ(naDecoded.size(), 1U);
    EXPECT_TRUE(std::isnan(naDecoded[0].value));
}

TEST(J1939NodeTests, IdenticalDm1BroadcastsDoNotDirtyTheNetworkSnapshot)
{
    J1939Network network;
    EXPECT_FALSE(network.snapshot().published);

    J1939Node node{nullptr, "J1939"};
    node.setNetwork(&network);
    ASSERT_TRUE(node.prepare(64U).succeeded());

    Driver driver{node};
    const std::array<CanFrame, 1> dm1{
        frameOf(j1939Identifier(kPgnDm1, kEngine), {0x40U, 0x00U, 100U, 0U, 1U, 5U, 0xFFU, 0xFFU})};

    driver.run(dm1);
    const std::uint64_t revAfterFirst = network.revision();
    EXPECT_TRUE(network.snapshot().published);

    // Repeating the same 1 Hz DM1 broadcast must not republish the snapshot.
    driver.run(dm1);
    EXPECT_EQ(network.revision(), revAfterFirst);

    // Clearing the network resets published to false while incrementing revision.
    network.clear();
    EXPECT_FALSE(network.snapshot().published);
    EXPECT_GT(network.revision(), revAfterFirst);
}
