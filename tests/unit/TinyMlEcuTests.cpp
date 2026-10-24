// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include <gtest/gtest.h>

#include "core/can/CanEngine.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/DecodedSignal.h"
#include "core/j1939/J1939Name.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/pipeline/nodes/SignalNodes.h"
#include "core/plot/SignalSeries.h"
#include "core/tinyml/TinyMlEcuNode.h"
#include "core/tinyml/TinyMlModel.h"
#include "core/trace/TraceStore.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

class VectorFrameSource final : public IPipelineNode {
public:
    explicit VectorFrameSource(std::vector<CanFrame> frames)
        : m_frames{std::move(frames)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.frames"; }
    [[nodiscard]] std::string displayName() const override { return "frames"; }
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
        PortDescriptor{"frames", PortType::Frames},
    };
    std::vector<CanFrame> m_frames;
};

[[nodiscard]] CanFrame makeSpeedFrame(double speedKmh, std::uint64_t timestampNs)
{
    CanFrame frame{};
    frame.identifier = 0x101;
    frame.dlc = 2;
    frame.length = 2;
    frame.timestampNs = timestampNs;
    const auto raw = static_cast<std::uint16_t>(speedKmh * 10.0 + 0.5);
    frame.data[0] = static_cast<std::uint8_t>(raw & 0xFFU);
    frame.data[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    return frame;
}

[[nodiscard]] CanFrame makeTempFrame(int tempDegC, std::uint64_t timestampNs)
{
    CanFrame frame{};
    frame.identifier = 0x102;
    frame.dlc = 1;
    frame.length = 1;
    frame.timestampNs = timestampNs;
    frame.data[0] = static_cast<std::uint8_t>(std::clamp(tempDegC + 40, 0, 255));
    return frame;
}

[[nodiscard]] CanFrame makeJ1939SpeedFrame(double speedKmh, std::uint64_t timestampNs)
{
    CanFrame frame{};
    frame.identifier = 0x18FEF100; // CCVS1
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 8;
    frame.length = 8;
    frame.timestampNs = timestampNs;
    std::fill(frame.data.begin(), frame.data.end(), std::uint8_t{0xFF});
    const auto raw = static_cast<std::uint16_t>(speedKmh * 256.0 + 0.5);
    frame.data[1] = static_cast<std::uint8_t>(raw & 0xFFU);
    frame.data[2] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    return frame;
}

[[nodiscard]] CanFrame makeJ1939TempFrame(int tempDegC, std::uint64_t timestampNs)
{
    CanFrame frame{};
    frame.identifier = 0x18FEEE00; // ET1
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 8;
    frame.length = 8;
    frame.timestampNs = timestampNs;
    std::fill(frame.data.begin(), frame.data.end(), std::uint8_t{0xFF});
    frame.data[0] = static_cast<std::uint8_t>(std::clamp(tempDegC + 40, 0, 250));
    return frame;
}

} // namespace

TEST(TinyMlEcuTests, StaticTensorArenaAllocatesAlignedSlicesAndResetsInConstantTime)
{
    TinyMlArena arena{1024};
    EXPECT_EQ(arena.capacityBytes(), 1024U);
    EXPECT_EQ(arena.currentPassBytes(), 0U);

    const std::span<float> a = arena.allocateFloats(7);
    ASSERT_EQ(a.size(), 7U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a.data()) % TinyMlArena::kAlignment, 0U);

    const std::span<float> b = arena.allocateFloats(12);
    ASSERT_EQ(b.size(), 12U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b.data()) % TinyMlArena::kAlignment, 0U);

    const std::size_t firstPassBytes = arena.currentPassBytes();
    EXPECT_GT(firstPassBytes, 0U);
    EXPECT_EQ(arena.highWaterMarkBytes(), firstPassBytes);

    arena.resetPass();
    EXPECT_EQ(arena.currentPassBytes(), 0U);
    EXPECT_EQ(arena.highWaterMarkBytes(), firstPassBytes);

    // Asking for more floats than the arena holds returns an empty span rather
    // than touching the heap.
    const std::span<float> exhausted = arena.allocateFloats(10'000);
    EXPECT_TRUE(exhausted.empty());
}

TEST(TinyMlEcuTests, BuiltinPowertrainModelClassifiesRegimesAndDetectsAnomalies)
{
    const TinyMlModel model = TinyMlModel::builtinPowertrainModel();
    EXPECT_EQ(model.layers().size(), 3U);
    EXPECT_GT(model.parameterCount(), 150U);
    EXPECT_GT(model.weightBytes(), 150U);

    TinyMlArena arena{TinyMlArena::kDefaultCapacityBytes};

    // 1. Stationary / Idle
    {
        TinyMlFeatureVector idle{};
        idle.speedKmh = 0.0F;
        idle.accelerationKmhPerSec = 0.0F;
        idle.engineTempDegC = 74.0F;
        idle.thermalDeltaDegC = 0.0F;
        idle.timingJitterRatio = 0.01F;
        idle.busStressFactor = 0.0F;

        TinyMlInferenceResult result{};
        ASSERT_TRUE(model.evaluate(idle, arena, 65.0F, result).succeeded());
        EXPECT_EQ(result.regime, TinyMlRegime::Idle);
        EXPECT_FALSE(result.isAnomaly);
        EXPECT_LT(result.anomalyScorePercent, 35.0F);
        EXPECT_GT(result.thermalHealthPercent, 75.0F);
    }

    // 2. Nominal Cruise (45 km/h, 76 C)
    {
        TinyMlFeatureVector cruise{};
        cruise.speedKmh = 45.0F;
        cruise.accelerationKmhPerSec = 0.5F;
        cruise.engineTempDegC = 76.0F;
        cruise.thermalDeltaDegC = 0.0F;
        cruise.timingJitterRatio = 0.02F;
        cruise.busStressFactor = 0.0F;

        TinyMlInferenceResult result{};
        ASSERT_TRUE(model.evaluate(cruise, arena, 65.0F, result).succeeded());
        EXPECT_EQ(result.regime, TinyMlRegime::Cruise);
        EXPECT_FALSE(result.isAnomaly);
        EXPECT_LT(result.anomalyScorePercent, 35.0F);
    }

    // 3. High Load (115 km/h + strong acceleration)
    {
        TinyMlFeatureVector highLoad{};
        highLoad.speedKmh = 115.0F;
        highLoad.accelerationKmhPerSec = 18.0F;
        highLoad.engineTempDegC = 86.0F;
        highLoad.thermalDeltaDegC = 2.0F;
        highLoad.timingJitterRatio = 0.02F;
        highLoad.busStressFactor = 0.0F;

        TinyMlInferenceResult result{};
        ASSERT_TRUE(model.evaluate(highLoad, arena, 65.0F, result).succeeded());
        EXPECT_EQ(result.regime, TinyMlRegime::HighLoad);
        EXPECT_FALSE(result.isAnomaly);
    }

    // 3.5. Thermal Stress (high coolant temp +15 C above nominal curve, low jitter)
    {
        TinyMlFeatureVector thermal{};
        thermal.speedKmh = 60.0F;
        thermal.accelerationKmhPerSec = 0.5F;
        thermal.engineTempDegC = 106.0F;
        thermal.thermalDeltaDegC = 15.0F;
        thermal.timingJitterRatio = 0.02F;
        thermal.busStressFactor = 0.0F;

        TinyMlInferenceResult result{};
        ASSERT_TRUE(model.evaluate(thermal, arena, 65.0F, result).succeeded());
        EXPECT_EQ(result.regime, TinyMlRegime::ThermalStress);
    }

    // 4. Severe CAN timing jitter & thermal anomaly
    {
        TinyMlFeatureVector anomaly{};
        anomaly.speedKmh = 60.0F;
        anomaly.accelerationKmhPerSec = 2.0F;
        anomaly.engineTempDegC = 118.0F;
        anomaly.thermalDeltaDegC = 32.0F;
        anomaly.timingJitterRatio = 0.95F;
        anomaly.busStressFactor = 0.95F;

        TinyMlInferenceResult result{};
        ASSERT_TRUE(model.evaluate(anomaly, arena, 65.0F, result).succeeded());
        EXPECT_EQ(result.regime, TinyMlRegime::Anomaly);
        EXPECT_TRUE(result.isAnomaly);
        EXPECT_GE(result.anomalyScorePercent, 65.0F);
        EXPECT_LT(result.thermalHealthPercent, 45.0F);
    }
}

TEST(TinyMlEcuTests, CustomTbusMlModelFormatParsesAndValidatesDimensions)
{
    constexpr std::string_view kCustomModel = R"(
        # Custom single-layer TinyML test model (6 inputs -> 7 outputs)
        TBUSML 1
        model_name custom_linear_head
        feature_mean 0 0 0 0 0 0
        feature_scale 1 1 1 1 1 1

        layer output_head 6 7 linear 0.1
        weights 10 0 0 0 0 0
        weights 0 10 0 0 0 0
        weights 0 0 10 0 0 0
        weights 0 0 0 10 0 0
        weights 0 0 0 0 10 10
        weights 0 0 0 10 20 20
        weights -5 -5 -5 -10 -10 -10
        biases 0.5 0.1 0.0 0.0 0.0 -1.0 1.5
    )";

    TinyMlModel custom;
    const Result parsed = TinyMlModel::parse(kCustomModel, custom);
    SCOPED_TRACE(::testing::Message() << std::string{parsed.message()});
    ASSERT_TRUE(parsed.succeeded());
    EXPECT_EQ(custom.name(), "custom_linear_head");
    ASSERT_EQ(custom.layers().size(), 1U);

    TinyMlArena arena{1024};
    TinyMlFeatureVector features{};
    features.engineTempDegC = 0.0F;
    TinyMlInferenceResult result{};
    ASSERT_TRUE(custom.evaluate(features, arena, 65.0F, result).succeeded());
    EXPECT_EQ(result.regime, TinyMlRegime::Idle);

    // Out-of-range int8 weight (> 127) is rejected naming the line number.
    TinyMlModel rejected;
    const Result badWeight =
        TinyMlModel::parse("TBUSML 1\nlayer l1 6 7 linear 0.1\nweights 200 0 0 0 0 0\n", rejected);
    EXPECT_TRUE(badWeight.failed());
    EXPECT_NE(std::string{badWeight.message()}.find("Line 3"), std::string::npos);
}

TEST(TinyMlEcuTests, TinyMlEcuNodeEmitsBothCanTelemetryFramesAndDecodedSignals)
{
    SystemVariables variables;

    std::vector<CanFrame> busFrames{
        makeSpeedFrame(0.0, 100'000'000ULL),
        makeTempFrame(74, 150'000'000ULL),
        makeSpeedFrame(45.0, 200'000'000ULL),
    };

    std::vector<CanFrame> emittedFrames;
    std::vector<DecodedSignal> emittedSignals;

    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<VectorFrameSource>(std::move(busFrames)));

    auto ecu = std::make_unique<TinyMlEcuNode>();
    ecu->setSystemVariables(&variables);
    const NodeId tinyml = graph.addNode(std::move(ecu));

    const NodeId frameSink = graph.addNode(std::make_unique<FrameSinkNode>(
        [&](std::span<const CanFrame> batch) {
            emittedFrames.insert(emittedFrames.end(), batch.begin(), batch.end());
        },
        "frame_sink"));

    const NodeId signalSink = graph.addNode(std::make_unique<SignalSinkNode>(
        [&](std::span<const DecodedSignal> batch) {
            emittedSignals.insert(emittedSignals.end(), batch.begin(), batch.end());
        },
        "signal_sink"));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{tinyml, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{tinyml, 0}, PortRef{frameSink, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{tinyml, 1}, PortRef{signalSink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    // 3 monitored frames -> 3 inferences -> 3 telemetry frames (0x105) and 15 signals.
    ASSERT_EQ(emittedFrames.size(), 3U);
    EXPECT_EQ(emittedFrames.back().identifier, 0x105U);
    EXPECT_EQ(emittedFrames.back().length, 8U);

    ASSERT_EQ(emittedSignals.size(), 3U * TinyMlEcuNode::kSignalsPerInference);
    EXPECT_EQ(emittedSignals[0].name(), "RegimeClass");
    EXPECT_EQ(emittedSignals[1].name(), "Confidence");
    EXPECT_EQ(emittedSignals[2].name(), "AnomalyScore");
    EXPECT_EQ(emittedSignals[3].name(), "ThermalHealth");
    EXPECT_EQ(emittedSignals[4].name(), "InferenceTimeUs");

    // Now inject a fault through SystemVariables and run another pass.
    variables.set("tinyml.inject_fault", 1.0);
    emittedFrames.clear();
    emittedSignals.clear();

    graph.execute();

    ASSERT_FALSE(emittedFrames.empty());
    EXPECT_EQ(emittedFrames.back().data[0], static_cast<std::uint8_t>(TinyMlRegime::Anomaly));
    EXPECT_GE(variables.value("tinyml.anomaly_score"), 65.0);
    EXPECT_EQ(variables.value("tinyml.regime"),
              static_cast<double>(static_cast<std::uint8_t>(TinyMlRegime::Anomaly)));
}

TEST(TinyMlEcuTests, CatalogRegistersTinyMlEcuAndRunsEndToEndOnVirtualBus)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    const NodeTypeInfo* info = catalog.find("tinyml.ecu");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->displayName, "TinyML Virtual ECU");
    EXPECT_EQ(info->category, "Simulation");
    ASSERT_EQ(info->inputs.size(), 1U);
    ASSERT_EQ(info->outputs.size(), 2U);
    EXPECT_EQ(info->outputs[0].type, PortType::Frames);
    EXPECT_EQ(info->outputs[1].type, PortType::Signals);

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "can_1",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "tinyml_ecu",
        .typeName = "tinyml.ecu",
        .parameters = {{"channel", ParameterValue::fromInteger(0)},
                       {"outputCanId", ParameterValue::fromInteger(0x105)}},
    });
    description.addNode(NodeDescription{
        .id = "tx_1",
        .typeName = "can.transmit",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "plot_1",
        .typeName = "signal.plot",
    });

    description.addEdge(EdgeDescription{"can_1", 0, "tinyml_ecu", 0});
    description.addEdge(EdgeDescription{"tinyml_ecu", 0, "tx_1", 0});
    description.addEdge(EdgeDescription{"tinyml_ecu", 1, "plot_1", 0});

    ASSERT_TRUE(description.validate(catalog).succeeded());
    engine.setGraphDescription(description, catalog);

    ASSERT_TRUE(engine.start().succeeded());

    const CanFrame speedFrame = makeSpeedFrame(52.0, 100'000'000ULL);
    ASSERT_TRUE(engine.transmit(0, speedFrame).succeeded());

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    engine.stop();

    EXPECT_GT(engine.plotStore().seriesCount(), 0U);
    EXPECT_GT(engine.variables().value("tinyml.inferences"), 0.0);
}

TEST(TinyMlEcuTests, J1939ModeProcessesCcvs1AndEt1AndEmitsProprietaryBFrame)
{
    SystemVariables variables;

    std::vector<CanFrame> busFrames{
        makeJ1939SpeedFrame(0.0, 100'000'000ULL),
        makeJ1939TempFrame(74, 150'000'000ULL),
        makeJ1939SpeedFrame(45.0, 200'000'000ULL),
    };

    std::vector<CanFrame> emittedFrames;
    std::vector<DecodedSignal> emittedSignals;

    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<VectorFrameSource>(std::move(busFrames)));

    TinyMlEcuConfig config;
    config.j1939Mode = true;
    config.outputCanId = 0x18FF0080;

    auto ecu = std::make_unique<TinyMlEcuNode>(TinyMlModel::builtinPowertrainModel(), config);
    ecu->setSystemVariables(&variables);
    const NodeId tinyml = graph.addNode(std::move(ecu));

    const NodeId frameSink = graph.addNode(std::make_unique<FrameSinkNode>(
        [&](std::span<const CanFrame> batch) {
            emittedFrames.insert(emittedFrames.end(), batch.begin(), batch.end());
        },
        "frame_sink"));

    const NodeId signalSink = graph.addNode(std::make_unique<SignalSinkNode>(
        [&](std::span<const DecodedSignal> batch) {
            emittedSignals.insert(emittedSignals.end(), batch.begin(), batch.end());
        },
        "signal_sink"));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{tinyml, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{tinyml, 0}, PortRef{frameSink, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{tinyml, 1}, PortRef{signalSink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    // The claim of its address first, then one telemetry frame for each of the three frames it
    // read.
    ASSERT_EQ(emittedFrames.size(), 4U);
    EXPECT_EQ(emittedFrames.front().identifier, 0x18EEFF80U);
    EXPECT_EQ(emittedFrames.back().identifier, 0x18FF0080U);
    EXPECT_EQ(emittedFrames.back().format, CanFrameFormat::Extended);
    EXPECT_EQ(emittedFrames.back().length, 8U);

    // Byte 1: ThermalHealth (0.4 %/bit)
    EXPECT_GT(emittedFrames.back().data[0], 0);
    // Byte 4: bits 4-5 is ModelStatus = 1 (running)
    const std::uint8_t status = (emittedFrames.back().data[3] >> 3U) & 0x03U;
    EXPECT_EQ(status, 1U);
}

TEST(TinyMlEcuTests, J1939ModeEmitsPredictiveDm1OnOverheating)
{
    SystemVariables variables;
    const SystemVariables::Handle injectFault = variables.resolve("tinyml.inject_fault");
    variables.set(injectFault, 1.5);

    std::vector<CanFrame> busFrames{
        makeJ1939SpeedFrame(60.0, 100'000'000ULL),
        makeJ1939TempFrame(118, 200'000'000ULL),
    };

    std::vector<CanFrame> emittedFrames;
    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<VectorFrameSource>(std::move(busFrames)));

    TinyMlEcuConfig config;
    config.j1939Mode = true;
    config.outputCanId = 0x18FF0080;

    auto ecu = std::make_unique<TinyMlEcuNode>(TinyMlModel::builtinPowertrainModel(), config);
    ecu->setSystemVariables(&variables);
    const NodeId tinyml = graph.addNode(std::move(ecu));

    const NodeId frameSink = graph.addNode(std::make_unique<FrameSinkNode>(
        [&](std::span<const CanFrame> batch) {
            emittedFrames.insert(emittedFrames.end(), batch.begin(), batch.end());
        },
        "frame_sink"));

    ASSERT_TRUE(graph.connect(PortRef{source, 0}, PortRef{tinyml, 0}).succeeded());
    ASSERT_TRUE(graph.connect(PortRef{tinyml, 0}, PortRef{frameSink, 0}).succeeded());
    ASSERT_TRUE(graph.compile().succeeded());

    graph.execute();

    const auto dm1It = std::find_if(emittedFrames.begin(),
                                    emittedFrames.end(),
                                    [](const CanFrame& f) { return f.identifier == 0x18FECA80U; });
    ASSERT_NE(dm1It, emittedFrames.end());
    EXPECT_EQ(dm1It->length, 8U);
    EXPECT_EQ(dm1It->data[2], 110);
}

// ---------------------------------------------------------------------------
// A J1939 bus
// ---------------------------------------------------------------------------

namespace {

/// The TinyML block on its own, driven a batch at a time, which is what lets a test change the
/// world between two of them - a fault injected, then taken away - and read what came out of each.
class TinyDriver final {
public:
    explicit TinyDriver(TinyMlEcuConfig config, SystemVariables* variables = nullptr)
        : m_node{TinyMlModel::builtinPowertrainModel(), config}
    {
        m_node.setSystemVariables(variables);
        const Result prepared = m_node.prepare(256U);
        EXPECT_TRUE(prepared.succeeded()) << std::string{prepared.message()};
    }

    /// What the block put on the bus for this batch.
    std::vector<CanFrame> run(std::span<const CanFrame> frames)
    {
        m_inputs[0] = PortBatch{frames};
        m_outputs[0] = PortBatch{};
        m_outputs[1] = PortBatch{};

        NodeContext context{m_inputs, m_outputs};
        m_node.process(context);

        const std::span<const CanFrame> out = m_outputs[0].as<CanFrame>();
        return std::vector<CanFrame>{out.begin(), out.end()};
    }

    [[nodiscard]] const TinyMlEcuNode& node() const { return m_node; }

    /// What the engine does at the start of the next measurement: the same block, prepared again.
    void restart()
    {
        const Result prepared = m_node.prepare(256U);
        EXPECT_TRUE(prepared.succeeded()) << std::string{prepared.message()};
    }

private:
    TinyMlEcuNode m_node;
    std::array<PortBatch, 1> m_inputs{};
    std::array<PortBatch, 2> m_outputs{};
};

[[nodiscard]] TinyMlEcuConfig j1939Config(std::uint32_t outputId = 0x18FF0080U)
{
    TinyMlEcuConfig config;
    config.j1939Mode = true;
    config.outputCanId = outputId;
    return config;
}

constexpr std::uint64_t kMs = 1'000'000ULL;

/// Percent from a J1939 byte at 0.4 %/bit.
[[nodiscard]] double percent(std::uint8_t byte)
{
    return static_cast<double>(byte) * 0.4;
}

[[nodiscard]] std::vector<CanFrame> withIdentifier(const std::vector<CanFrame>& frames,
                                                   std::uint32_t identifier)
{
    std::vector<CanFrame> found;
    for (const CanFrame& frame : frames) {
        if (frame.identifier == identifier) {
            found.push_back(frame);
        }
    }
    return found;
}

} // namespace

TEST(TinyMlEcuTests, AHealthyJ1939EngineAtItsThermostatIsNotAnAlarm)
{
    // The model was set against a vehicle whose coolant idles at 70 degC. A heavy-duty diesel holds
    // 88, and fed that as it was the model called every healthy truck a thermal emergency from the
    // first frame - "ThermalStress", 100 % anomaly, 0 % health - and the cluster lit its stop lamp.
    TinyDriver driver{j1939Config()};

    std::vector<CanFrame> sent;
    for (std::uint64_t step = 0; step < 20; ++step) {
        const std::vector<CanFrame> speed{
            makeJ1939SpeedFrame(0.0, (100 + step * 100) * kMs),
            makeJ1939TempFrame(88, (150 + step * 100) * kMs),
        };
        const std::vector<CanFrame> out = driver.run(speed);
        sent.insert(sent.end(), out.begin(), out.end());
    }

    const std::vector<CanFrame> telemetry = withIdentifier(sent, 0x18FF0080U);
    ASSERT_FALSE(telemetry.empty());

    const CanFrame& last = telemetry.back();
    EXPECT_GT(percent(last.data[0]), 70.0) << "thermal health";
    EXPECT_LT(percent(last.data[1]), 40.0) << "anomaly score";
    EXPECT_LT(last.data[3] >> 5U, 3) << "regime";

    // And it did not accuse the engine on the bus either.
    EXPECT_TRUE(withIdentifier(sent, 0x18FECA80U).empty());
}

TEST(TinyMlEcuTests, AJ1939EngineThatOverheatsIsStillAnAlarm)
{
    // The other half of the calibration above, which a model made deaf would pass: 118 degC at 80
    // km/h is not a healthy engine, and has to look like one that is not.
    TinyDriver driver{j1939Config()};

    std::vector<CanFrame> sent;
    for (std::uint64_t step = 0; step < 10; ++step) {
        const std::vector<CanFrame> frames{
            makeJ1939SpeedFrame(80.0, (100 + step * 100) * kMs),
            makeJ1939TempFrame(118, (150 + step * 100) * kMs),
        };
        const std::vector<CanFrame> out = driver.run(frames);
        sent.insert(sent.end(), out.begin(), out.end());
    }

    const std::vector<CanFrame> telemetry = withIdentifier(sent, 0x18FF0080U);
    ASSERT_FALSE(telemetry.empty());

    const CanFrame& last = telemetry.back();
    EXPECT_GE(last.data[3] >> 5U, 3) << "regime";
    EXPECT_GT(percent(last.data[1]), 50.0) << "anomaly score";
}

TEST(TinyMlEcuTests, TheJ1939TelemetryCarriesTheRegimeNextToTheRiskLevel)
{
    // They are different questions: what the vehicle is doing, and how worried the model is about
    // it. Both travel in byte 4, the risk level in bits 1-3 and the regime in bits 6-8.
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                       makeJ1939TempFrame(88, 150 * kMs)};
    const std::vector<CanFrame> out = driver.run(frames);

    const std::vector<CanFrame> telemetry = withIdentifier(out, 0x18FF0080U);
    ASSERT_FALSE(telemetry.empty());

    const auto regime = static_cast<std::uint8_t>(driver.node().lastResult().regime);
    EXPECT_EQ(telemetry.back().data[3] >> 5U, regime);
    EXPECT_EQ((telemetry.back().data[3] >> 3U) & 0x03U, 1U) << "model status: running";
}

TEST(TinyMlEcuTests, TheJ1939MessageCounterNeverEntersTheRangeOfErrorAndNotAvailable)
{
    // A byte counts 0 to 250 on a J1939 bus; above that is where the standard keeps "error" and
    // "not available", and a counter that walked through 254 and 255 was read as a sensor failing
    // twice every lap.
    TinyDriver driver{j1939Config()};

    std::uint8_t highest = 0;
    bool wrapped = false;
    std::uint8_t previous = 0;

    for (std::uint64_t step = 0; step < 600; ++step) {
        const std::vector<CanFrame> frames{makeJ1939SpeedFrame(30.0, (100 + step * 100) * kMs)};
        for (const CanFrame& frame : withIdentifier(driver.run(frames), 0x18FF0080U)) {
            highest = std::max(highest, frame.data[4]);
            wrapped = wrapped || frame.data[4] < previous;
            previous = frame.data[4];
        }
    }

    EXPECT_EQ(highest, 250U);
    EXPECT_TRUE(wrapped);
}

TEST(TinyMlEcuTests, ThePredictiveDm1IsSentOnceASecondAndNotOnceAnInference)
{
    // J1939-73 has a DM1 every second. Thirty a second - one per inference - would be a load on the
    // bus of the model's own making, and a trace nobody could read.
    SystemVariables variables;
    variables.set("tinyml.inject_fault", 1.5); // health under 30 %: at once, no five-second wait

    TinyDriver driver{j1939Config(), &variables};

    std::vector<CanFrame> sent;
    for (std::uint64_t step = 0; step < 50; ++step) { // five seconds of bus time at 10 Hz
        const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, (100 + step * 100) * kMs),
                                           makeJ1939TempFrame(95, (150 + step * 100) * kMs)};
        const std::vector<CanFrame> out = driver.run(frames);
        sent.insert(sent.end(), out.begin(), out.end());
    }

    const std::vector<CanFrame> dm1 = withIdentifier(sent, 0x18FECA80U);
    ASSERT_FALSE(dm1.empty());
    EXPECT_GE(dm1.size(), 4U);
    EXPECT_LE(dm1.size(), 6U);

    // SPN 110, FMI 16 with the red lamp as well as the amber: the health is what is failing.
    EXPECT_EQ(dm1.front().data[0], 0x14U);
    EXPECT_EQ(dm1.front().data[2], 110U);
    EXPECT_EQ(dm1.front().data[4] & 0x1FU, 16U);
}

TEST(TinyMlEcuTests, WhenTheFaultGoesAwayOneCleanDm1SaysSo)
{
    // Without it a receiver learns that the fault is over from silence, and has to wait for the
    // silence to be long enough to mean something.
    SystemVariables variables;
    TinyDriver driver{j1939Config(), &variables};

    std::vector<CanFrame> sent;
    std::uint64_t at = 100;

    const auto drive = [&](int seconds) {
        for (int step = 0; step < seconds * 10; ++step) {
            const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, at * kMs),
                                               makeJ1939TempFrame(95, (at + 50) * kMs)};
            const std::vector<CanFrame> out = driver.run(frames);
            sent.insert(sent.end(), out.begin(), out.end());
            at += 100;
        }
    };

    variables.set("tinyml.inject_fault", 1.5);
    drive(3);
    ASSERT_FALSE(withIdentifier(sent, 0x18FECA80U).empty());

    variables.set("tinyml.inject_fault", 0.0);
    sent.clear();
    drive(15); // under 60 % for ten seconds of bus time takes the code back

    const std::vector<CanFrame> dm1 = withIdentifier(sent, 0x18FECA80U);
    ASSERT_FALSE(dm1.empty());

    // The last of them is the clean one: every lamp dark, SPN, FMI and count all zero.
    EXPECT_EQ(dm1.back().data[0], 0x00U);
    EXPECT_EQ(dm1.back().data[2], 0U);
    EXPECT_EQ(dm1.back().data[3], 0U);
    EXPECT_EQ(dm1.back().data[4], 0U);
    EXPECT_EQ(dm1.back().data[5], 0U);

    // And nothing follows it: the DM1 stops being sent.
    sent.clear();
    drive(5);
    EXPECT_TRUE(withIdentifier(sent, 0x18FECA80U).empty());
}

TEST(TinyMlEcuTests, TheTroubleCodeIsSentFromTheAddressOfTheTelemetry)
{
    // The ECU is one ECU. A telemetry address of 0x81 gives a DM1 from 0x81 - and the block ignores
    // that one when it comes back off the bus, rather than the one a constant happened to name.
    SystemVariables variables;
    variables.set("tinyml.inject_fault", 1.5);

    TinyDriver driver{j1939Config(0x18FF0081U), &variables};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                       makeJ1939TempFrame(95, 150 * kMs)};
    const std::vector<CanFrame> out = driver.run(frames);

    EXPECT_FALSE(withIdentifier(out, 0x18FF0081U).empty());
    EXPECT_FALSE(withIdentifier(out, 0x18FECA81U).empty());
    EXPECT_TRUE(withIdentifier(out, 0x18FECA80U).empty());

    // Its own DM1 coming back is not a frame to analyse.
    const std::uint64_t analysed = driver.node().framesAnalysed();
    CanFrame own = withIdentifier(out, 0x18FECA81U).front();
    driver.run(std::vector<CanFrame>{own});
    EXPECT_EQ(driver.node().framesAnalysed(), analysed);
}

// ---------------------------------------------------------------------------
// The claim of its address
// ---------------------------------------------------------------------------

TEST(TinyMlEcuTests, OnAJ1939BusTheEcuClaimsItsAddressBeforeItSaysAnythingElse)
{
    // Nothing is sent from an address that has not been claimed, and a panel names an ECU by its
    // claim and by nothing else: without one this ECU was "SA 128" and a number.
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                       makeJ1939TempFrame(88, 150 * kMs)};
    const std::vector<CanFrame> out = driver.run(frames);

    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().identifier, 0x18EEFF80U) << "the claim comes first";

    const std::vector<CanFrame> claims = withIdentifier(out, 0x18EEFF80U);
    ASSERT_EQ(claims.size(), 1U);

    const CanFrame& claim = claims.front();
    EXPECT_EQ(claim.format, CanFrameFormat::Extended);
    EXPECT_EQ(claim.length, 8U);
    EXPECT_EQ(claim.dlc, 8U);
    EXPECT_EQ(claim.direction, CanDirection::Tx);

    // The time of the frame that woke it: an ECU with no clock of its own takes the bus's.
    EXPECT_EQ(claim.timestampNs, 100 * kMs);

    // On-highway, a System Monitor, and the address it speaks from as its serial number.
    const std::optional<J1939Name> name = j1939NameFromClaim(claim);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->industryGroup, 1U);
    EXPECT_EQ(name->function, 33U);
    EXPECT_EQ(name->identityNumber, 0x80U);
    EXPECT_FALSE(name->arbitraryAddressCapable);

    // And then what it came to say.
    EXPECT_FALSE(withIdentifier(out, 0x18FF0080U).empty());
}

TEST(TinyMlEcuTests, TheClaimIsSentOncePerMeasurementAndAgainWhenTheNextOneStarts)
{
    TinyDriver driver{j1939Config()};

    const auto claimsFor = [&driver](std::uint64_t atMs) {
        const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, atMs * kMs),
                                           makeJ1939TempFrame(88, (atMs + 50) * kMs)};
        return withIdentifier(driver.run(frames), 0x18EEFF80U).size();
    };

    EXPECT_EQ(claimsFor(100), 1U);
    EXPECT_EQ(claimsFor(200), 0U);
    EXPECT_EQ(claimsFor(300), 0U);

    // The ECU is switched on again, and claims again.
    driver.restart();

    EXPECT_EQ(claimsFor(100), 1U);
    EXPECT_EQ(claimsFor(200), 0U);
}

TEST(TinyMlEcuTests, TheClaimIsMadeFromTheAddressOfTheTelemetryAndNamesItBySerialNumber)
{
    // The ECU is one ECU. At 0x81 it claims 0x81, and is not the NAME of the one at 0x80: two of
    // them on a bus are two ECUs and not a contest for an address.
    TinyDriver first{j1939Config(0x18FF0080U)};
    TinyDriver second{j1939Config(0x18FF0081U)};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs)};
    const std::vector<CanFrame> outFirst = first.run(frames);
    const std::vector<CanFrame> outSecond = second.run(frames);

    const std::vector<CanFrame> claimFirst = withIdentifier(outFirst, 0x18EEFF80U);
    const std::vector<CanFrame> claimSecond = withIdentifier(outSecond, 0x18EEFF81U);

    ASSERT_EQ(claimFirst.size(), 1U);
    ASSERT_EQ(claimSecond.size(), 1U);
    EXPECT_TRUE(withIdentifier(outSecond, 0x18EEFF80U).empty());

    const std::optional<J1939Name> nameFirst = j1939NameFromClaim(claimFirst.front());
    const std::optional<J1939Name> nameSecond = j1939NameFromClaim(claimSecond.front());
    ASSERT_TRUE(nameFirst.has_value());
    ASSERT_TRUE(nameSecond.has_value());

    EXPECT_EQ(nameSecond->identityNumber, 0x81U);
    EXPECT_NE(nameFirst->value(), nameSecond->value());
}

TEST(TinyMlEcuTests, ItsOwnClaimComingBackOffTheBusIsNotAFrameToAnalyseAndNotAnotherClaim)
{
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                       makeJ1939TempFrame(88, 150 * kMs)};
    const std::vector<CanFrame> out = driver.run(frames);

    const std::uint64_t analysed = driver.node().framesAnalysed();
    const CanFrame own = withIdentifier(out, 0x18EEFF80U).front();

    const std::vector<CanFrame> answer = driver.run(std::vector<CanFrame>{own});

    EXPECT_EQ(driver.node().framesAnalysed(), analysed);
    EXPECT_TRUE(answer.empty());
}

TEST(TinyMlEcuTests, WhateverItFirstHearsOnTheBusMakesItClaim)
{
    // Not only the frames the model runs on: an ECU claims because the bus is there. Here all it
    // hears is another ECU's claim, which is no reason to say nothing about its own.
    TinyDriver driver{j1939Config()};

    CanFrame engine{};
    engine.identifier = 0x18EEFF00U; // Address Claimed from address 0
    engine.format = CanFrameFormat::Extended;
    engine.dlc = 8;
    engine.length = 8;
    engine.timestampNs = 100 * kMs;

    const std::vector<CanFrame> out = driver.run(std::vector<CanFrame>{engine});

    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out.front().identifier, 0x18EEFF80U);
}

namespace {

/// A Request (PGN 0xEA00) for `pgn`, from a service tool at 0xF9, to `destination`.
[[nodiscard]] CanFrame requestFor(std::uint32_t pgn,
                                  std::uint8_t destination,
                                  std::uint64_t atNs,
                                  std::uint8_t source = 0xF9U)
{
    CanFrame frame{};
    frame.identifier = j1939Identifier(kPgnRequest, source, destination);
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 3;
    frame.length = 3;
    frame.timestampNs = atNs;
    frame.data[0] = static_cast<std::uint8_t>(pgn & 0xFFU);
    frame.data[1] = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
    frame.data[2] = static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU);
    return frame;
}

/// A frame of the bus that is none of this block's business - Engine Hours - and carries the time
/// of the bus: what a block with no clock of its own is told the time by.
[[nodiscard]] CanFrame busTime(std::uint64_t atNs)
{
    CanFrame frame{};
    frame.identifier = 0x18FEE500U;
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 8;
    frame.length = 8;
    frame.timestampNs = atNs;
    return frame;
}

/// How long the NAME of the block as it comes - on-highway, a System Monitor at 0x80 - waits to
/// answer a request to everyone for the claims of the bus.
[[nodiscard]] std::uint64_t defaultClaimDelayNs()
{
    J1939Name name;
    name.industryGroup = 1U;
    name.function = 33U;
    name.identityNumber = 0x80U;

    return j1939ClaimDelayNs(name);
}

/// An Address Claimed from `source`, under `name`.
[[nodiscard]] CanFrame claimOf(std::uint8_t source, const J1939Name& name, std::uint64_t atNs)
{
    CanFrame frame{};
    frame.identifier = j1939Identifier(kPgnAddressClaimed, source);
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 8;
    frame.length = 8;
    frame.timestampNs = atNs;

    for (std::size_t byte = 0; byte < 8U; ++byte) {
        frame.data[byte] = static_cast<std::uint8_t>((name.value() >> (8U * byte)) & 0xFFU);
    }

    return frame;
}

/// The value of one of the block's statistics, or nothing when it has none of that name.
[[nodiscard]] std::optional<std::uint64_t> statistic(const TinyMlEcuNode& node,
                                                     std::string_view label)
{
    for (const NodeStatistic& entry : node.statistics()) {
        if (entry.label == label) {
            return entry.value;
        }
    }

    return std::nullopt;
}

constexpr std::uint32_t kOwnClaim = 0x18EEFF80U;
constexpr std::uint32_t kCannotClaim = 0x18EEFFFEU;

} // namespace

TEST(TinyMlEcuTests, ARequestForTheClaimsOfTheBusIsAnsweredWithItsClaim)
{
    // How a tool connected to a running bus learns who is on it: it asks, and every ECU answers.
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::uint64_t analysed = driver.node().framesAnalysed();

    const auto claimsAnswering = [&driver](const CanFrame& request) {
        return withIdentifier(driver.run(std::vector<CanFrame>{request}), kOwnClaim).size();
    };

    // To this ECU: answered at once. To every ECU: answered after the time J1939-81 gives it, which
    // is the next test.
    EXPECT_EQ(claimsAnswering(requestFor(kPgnAddressClaimed, 0x80U, 300 * kMs)), 1U);

    // To another ECU, or for something that is not a claim and that nobody asked this ECU for: not
    // this ECU's to answer.
    EXPECT_EQ(claimsAnswering(requestFor(kPgnAddressClaimed, 0x00U, 400 * kMs)), 0U);
    EXPECT_EQ(claimsAnswering(requestFor(kPgnDm2, kJ1939GlobalAddress, 500 * kMs)), 0U);

    // And none of them is a frame the model reads.
    EXPECT_EQ(driver.node().framesAnalysed(), analysed);
}

TEST(TinyMlEcuTests, ARequestToEveryoneWaitsItsTimeAndGoesOutWithTheFirstFrameAsLateAsItIsDue)
{
    // J1939-81: ECUs asked at one instant do not all answer at one instant. The time comes from the
    // NAME - so two ECUs differ and a measurement repeats - and the block, which has no clock of
    // its own, reads it off the frames of the bus.
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::uint64_t delay = defaultClaimDelayNs();
    ASSERT_GT(delay, 0U);
    ASSERT_LT(delay, 154U * 1'000'000ULL);

    const std::uint64_t asked = 200 * kMs;
    EXPECT_TRUE(
        driver
            .run(std::vector<CanFrame>{requestFor(kPgnAddressClaimed, kJ1939GlobalAddress, asked)})
            .empty());

    // A frame of the bus a moment before it is due: nothing yet.
    EXPECT_TRUE(driver.run(std::vector<CanFrame>{busTime(asked + delay - 1U)}).empty());

    // The first one that is as late as the answer is due: out, stamped with the time it was due.
    const std::vector<CanFrame> out = driver.run(std::vector<CanFrame>{busTime(asked + delay)});
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out.front().identifier, kOwnClaim);
    EXPECT_EQ(out.front().timestampNs, asked + delay);

    // Once.
    EXPECT_TRUE(driver.run(std::vector<CanFrame>{busTime(asked + 10U * delay)}).empty());
}

TEST(TinyMlEcuTests, TwoBlocksAskedAtOnceAnswerAtTheTimesOfTheirOwnNames)
{
    // At 0x80 the delay is a few milliseconds and at 0x81 close to a hundred: the same request, and
    // the ECUs do not come at the same instant.
    TinyDriver first{j1939Config(0x18FF0080U)};
    TinyDriver second{j1939Config(0x18FF0081U)};

    const std::vector<CanFrame> startUp{makeJ1939SpeedFrame(60.0, 100 * kMs)};
    (void)first.run(startUp);
    (void)second.run(startUp);

    const std::vector<CanFrame> request{
        requestFor(kPgnAddressClaimed, kJ1939GlobalAddress, 1000 * kMs)};
    (void)first.run(request);
    (void)second.run(request);

    // Ten milliseconds on: the first has answered and the second has not.
    const std::vector<CanFrame> soon{busTime(1010 * kMs)};
    EXPECT_EQ(withIdentifier(first.run(soon), 0x18EEFF80U).size(), 1U);
    EXPECT_TRUE(second.run(soon).empty());

    // A hundred and fifty on, it has too.
    const std::vector<CanFrame> later{busTime(1150 * kMs)};
    EXPECT_EQ(withIdentifier(second.run(later), 0x18EEFF81U).size(), 1U);
}

TEST(TinyMlEcuTests, ARequestItSentItselfComingBackOffTheBusIsNotAnsweredByIt)
{
    // The bus echoes what is transmitted, so an ECU hears its own requests: from its own address,
    // to everyone or to itself, for what it has, they are not questions for it.
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    EXPECT_TRUE(
        driver
            .run(std::vector<CanFrame>{requestFor(kPgnDm1, kJ1939GlobalAddress, 300 * kMs, 0x80U)})
            .empty());
    EXPECT_TRUE(
        driver.run(std::vector<CanFrame>{requestFor(kPgnAddressClaimed, 0x80U, 400 * kMs, 0x80U)})
            .empty());

    EXPECT_EQ(statistic(driver.node(), "Requests answered"), std::optional<std::uint64_t>{0U});
    EXPECT_EQ(statistic(driver.node(), "Requests refused"), std::optional<std::uint64_t>{0U});
}

TEST(TinyMlEcuTests, AFloodOfRequestsForTheClaimsOfTheBusLeavesRoomForOnlyAFewAnswersToWait)
{
    // Asked of everyone, an answer waits for its time. Asked two hundred times at one instant, it
    // must not wait two hundred times over: the room to wait in is limited, what is in it is
    // answered, and only what was answered is counted.
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::vector<CanFrame> flood(
        200U, requestFor(kPgnAddressClaimed, kJ1939GlobalAddress, 200 * kMs));
    EXPECT_TRUE(driver.run(flood).empty());

    // Long after every one of them is due.
    const std::vector<CanFrame> out = driver.run(std::vector<CanFrame>{busTime(1000 * kMs)});
    const std::vector<CanFrame> claims = withIdentifier(out, kOwnClaim);

    EXPECT_GT(claims.size(), 8U);
    EXPECT_LT(claims.size(), 64U);
    EXPECT_EQ(statistic(driver.node(), "Requests answered"),
              std::optional<std::uint64_t>{claims.size()});
}

TEST(TinyMlEcuTests, AnEcuWithALowerNameTakesTheAddressAndTheBlockFallsSilentOnTheBus)
{
    // J1939-81: two ECUs claiming one address, and the lower NAME keeps it. This block cannot move
    // to another address, so it does what such an ECU does - and does not go on talking from an
    // address that is somebody else's, which on a real bus would be two ECUs answering as one.
    SystemVariables variables;
    variables.set("tinyml.inject_fault", 1.5); // so that it has a trouble code to send as well
    TinyDriver driver{j1939Config(), &variables};

    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    // An engine controller claims 0x80 with a NAME lower than an on-highway System Monitor's.
    J1939Name lower;
    lower.identityNumber = 7U;

    const std::uint64_t lost = 200 * kMs;
    const std::vector<CanFrame> contest =
        driver.run(std::vector<CanFrame>{claimOf(0x80U, lower, lost)});
    EXPECT_TRUE(contest.empty());

    // It says that it cannot claim an address - from the null address, under its own NAME - after
    // the time J1939-81 gives it.
    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{busTime(lost + defaultClaimDelayNs())});
    const std::vector<CanFrame> cannot = withIdentifier(answer, kCannotClaim);
    ASSERT_EQ(cannot.size(), 1U);
    EXPECT_EQ(answer.size(), 1U);

    const std::optional<J1939Name> name = j1939NameFromClaim(cannot.front());
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->function, 33U);
    EXPECT_EQ(name->identityNumber, 0x80U);

    // From then on nothing goes on the bus - no telemetry, no trouble code - and the model runs all
    // the same: what it finds still reaches the dashboard.
    const std::uint64_t inferences = driver.node().inferencesRun();
    std::vector<CanFrame> sent;

    for (std::uint64_t step = 0; step < 20; ++step) {
        const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, (300 + step * 100) * kMs),
                                           makeJ1939TempFrame(95, (350 + step * 100) * kMs)};
        const std::vector<CanFrame> out = driver.run(frames);
        sent.insert(sent.end(), out.begin(), out.end());
    }

    EXPECT_TRUE(sent.empty());
    EXPECT_GT(driver.node().inferencesRun(), inferences);

    // Asked for the claims of the bus, it says again that it has none - when its time comes.
    const std::uint64_t asked = 3000 * kMs;
    EXPECT_TRUE(
        driver
            .run(std::vector<CanFrame>{requestFor(kPgnAddressClaimed, kJ1939GlobalAddress, asked)})
            .empty());
    const std::vector<CanFrame> again =
        driver.run(std::vector<CanFrame>{busTime(asked + defaultClaimDelayNs())});
    ASSERT_EQ(again.size(), 1U);
    EXPECT_EQ(again.front().identifier, kCannotClaim);

    // And its statistics say why it has gone quiet.
    EXPECT_EQ(statistic(driver.node(), "J1939 address lost"), std::optional<std::uint64_t>{1U});
}
TEST(TinyMlEcuTests, AnEcuWithAHigherNameIsToldAgainThatTheAddressIsTaken)
{
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    // Arbitrary-address capable is the top bit of a NAME: such an ECU loses every tie, and moves.
    J1939Name higher;
    higher.arbitraryAddressCapable = true;
    higher.industryGroup = 1U;

    const std::vector<CanFrame> answer = driver.run(std::vector<CanFrame>{
        claimOf(0x80U, higher, 200 * kMs), makeJ1939SpeedFrame(60.0, 210 * kMs)});

    // It defends the address, and goes on talking from it.
    EXPECT_EQ(withIdentifier(answer, kOwnClaim).size(), 1U);
    EXPECT_TRUE(withIdentifier(answer, kCannotClaim).empty());
    EXPECT_FALSE(withIdentifier(answer, 0x18FF0080U).empty());
    EXPECT_EQ(statistic(driver.node(), "J1939 address lost"), std::optional<std::uint64_t>{0U});
}

TEST(TinyMlEcuTests, ARequestForItsTroubleCodeIsAnsweredWithItsDm1WhetherOrNotTheModelIsSure)
{
    SystemVariables variables;
    TinyDriver driver{j1939Config(), &variables};

    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                           makeJ1939TempFrame(88, 150 * kMs)});

    // The model is calm: J1939-73's "no active fault" - every lamp off, a code of zeroes.
    const std::vector<CanFrame> calm =
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm1, kJ1939GlobalAddress, 300 * kMs)});
    ASSERT_EQ(calm.size(), 1U);
    EXPECT_EQ(calm.front().identifier, 0x18FECA80U);
    EXPECT_EQ(calm.front().data[0], 0x00U);
    EXPECT_EQ(calm.front().data[2], 0x00U);

    // Told to be sure of trouble, it answers with the code it is saying.
    variables.set("tinyml.inject_fault", 1.5);
    for (std::uint64_t step = 0; step < 6; ++step) {
        (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, (400 + step * 100) * kMs),
                                               makeJ1939TempFrame(95, (450 + step * 100) * kMs)});
    }

    const std::vector<CanFrame> sure =
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm1, 0x80U, 2000 * kMs)});
    const std::vector<CanFrame> dm1 = withIdentifier(sure, 0x18FECA80U);
    ASSERT_EQ(dm1.size(), 1U);
    EXPECT_EQ(dm1.front().data[0] & 0x0CU, 0x04U); // the amber lamp, and the red one with it or not
    EXPECT_EQ(dm1.front().data[2], 110U); // SPN 110, engine coolant temperature

    // FMI 15 under the amber lamp alone, and FMI 16 when the red one is lit as well.
    const bool redLit = (dm1.front().data[0] & 0x30U) == 0x10U;
    EXPECT_EQ(dm1.front().data[4] & 0x1FU, redLit ? 16U : 15U);

    EXPECT_EQ(statistic(driver.node(), "Requests answered"), std::optional<std::uint64_t>{2U});
}

TEST(TinyMlEcuTests, ARequestForItsTelemetryBeforeItHasSentAnyIsRefusedAndNotAnsweredWithNothing)
{
    // The first thing this ECU hears is the question: it has not analysed anything, so there is no
    // frame of its telemetry to send again - and an answer made of zeroes would be a lie.
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> out =
        driver.run(std::vector<CanFrame>{requestFor(0xFF00U, 0x80U, 100 * kMs)});

    EXPECT_TRUE(withIdentifier(out, 0x18FF0080U).empty());

    const std::vector<CanFrame> refusals = withIdentifier(out, 0x18E8F980U);
    ASSERT_EQ(refusals.size(), 1U);
    EXPECT_EQ(refusals.front().data[5], 0x00U); // PGN 0xFF00, least significant byte first
    EXPECT_EQ(refusals.front().data[6], 0xFFU);

    EXPECT_EQ(statistic(driver.node(), "Requests refused"), std::optional<std::uint64_t>{1U});
}

TEST(TinyMlEcuTests, ARequestForItsTelemetryIsAnsweredWithTheLatestFrame)
{
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> out = driver.run(std::vector<CanFrame>{
        makeJ1939SpeedFrame(60.0, 100 * kMs), makeJ1939TempFrame(88, 150 * kMs)});
    const std::vector<CanFrame> telemetry = withIdentifier(out, 0x18FF0080U);
    ASSERT_FALSE(telemetry.empty());

    // Proprietary B, PGN 0xFF00: asked for, it is what it was last, from the time of the request.
    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{requestFor(0xFF00U, kJ1939GlobalAddress, 400 * kMs)});

    ASSERT_EQ(answer.size(), 1U);
    EXPECT_EQ(answer.front().identifier, 0x18FF0080U);
    EXPECT_EQ(answer.front().timestampNs, 400 * kMs);
    EXPECT_TRUE(std::equal(telemetry.back().data.begin(),
                           telemetry.back().data.begin() + 8,
                           answer.front().data.begin()));
}

TEST(TinyMlEcuTests, ARequestToTheBlockForSomethingItDoesNotSendIsRefusedAndOneToEveryoneIsNot)
{
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    // The tool at 0xF9 asks the block for its DM2, which it has not got: J1939-21 has it say so.
    const std::vector<CanFrame> refused =
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm2, 0x80U, 200 * kMs)});

    ASSERT_EQ(refused.size(), 1U);
    EXPECT_EQ(refused.front().identifier, 0x18E8F980U);
    EXPECT_EQ(refused.front().timestampNs, 200 * kMs);

    const std::array<std::uint8_t, 8> expected{
        0x01U, 0xFFU, 0xFFU, 0xFFU, 0xF9U, 0xCBU, 0xFEU, 0x00U};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), refused.front().data.begin()));

    // Asked of everyone, or of somebody else: nothing to say.
    EXPECT_TRUE(
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm2, kJ1939GlobalAddress, 300 * kMs)})
            .empty());
    EXPECT_TRUE(driver.run(std::vector<CanFrame>{requestFor(kPgnDm2, 0x00U, 400 * kMs)}).empty());

    // From a tool that has no address - the null one, which is what this tool asks from - nobody is
    // there to be told, and the refusal goes to everyone.
    const std::vector<CanFrame> anonymous =
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm2, 0x80U, 500 * kMs, kJ1939NullAddress)});

    ASSERT_EQ(anonymous.size(), 1U);
    EXPECT_EQ(anonymous.front().identifier, 0x18E8FF80U);
    EXPECT_EQ(anonymous.front().data[4], kJ1939NullAddress);

    EXPECT_EQ(statistic(driver.node(), "Requests refused"), std::optional<std::uint64_t>{2U});
    EXPECT_EQ(statistic(driver.node(), "Requests answered"), std::optional<std::uint64_t>{0U});
}

TEST(TinyMlEcuTests, AnEcuThatLostItsAddressAnswersOnlyTheRequestForTheClaimsOfTheBus)
{
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    J1939Name lower;
    lower.identityNumber = 7U;
    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, lower, 200 * kMs)});
    (void)driver.run(std::vector<CanFrame>{busTime(300 * kMs)}); // the "cannot claim" goes out

    // Nothing to say about a trouble code it has no address to send from, to one or to everyone.
    EXPECT_TRUE(
        driver.run(std::vector<CanFrame>{requestFor(kPgnDm1, kJ1939GlobalAddress, 400 * kMs)})
            .empty());

    // And to the request that asks every ECU for its claim: that it has none, when its time comes.
    (void)driver.run(
        std::vector<CanFrame>{requestFor(kPgnAddressClaimed, kJ1939GlobalAddress, 500 * kMs)});
    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{busTime(500 * kMs + defaultClaimDelayNs())});

    ASSERT_EQ(answer.size(), 1U);
    EXPECT_EQ(answer.front().identifier, kCannotClaim);

    // A request addressed to the address it lost is for the ECU that holds it now: this one has no
    // address, and says nothing.
    (void)driver.run(std::vector<CanFrame>{requestFor(kPgnAddressClaimed, 0x80U, 1000 * kMs)});
    EXPECT_TRUE(
        driver.run(std::vector<CanFrame>{busTime(1000 * kMs + defaultClaimDelayNs())}).empty());
}

TEST(TinyMlEcuTests, ANewMeasurementClaimsTheAddressAgainAfterOneThatLostIt)
{
    TinyDriver driver{j1939Config()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    J1939Name lower;
    lower.identityNumber = 7U;
    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, lower, 200 * kMs)});

    // The ECU is switched on again: it claims, and talks.
    driver.restart();

    const std::vector<CanFrame> out =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});
    EXPECT_EQ(withIdentifier(out, kOwnClaim).size(), 1U);
    EXPECT_FALSE(withIdentifier(out, 0x18FF0080U).empty());
}

// ---------------------------------------------------------------------------
// What it claims to be, and an ECU that can move
// ---------------------------------------------------------------------------

namespace {

/// A configuration for a block that can take another address when it loses its own.
[[nodiscard]] TinyMlEcuConfig movingConfig()
{
    TinyMlEcuConfig config = j1939Config();
    config.j1939ArbitraryAddress = true;
    return config;
}

/// A NAME lower than the System Monitor's, as an engine controller's is: it wins any contest with
/// it, whether the System Monitor can move or not.
[[nodiscard]] J1939Name winningName(std::uint32_t identity = 7U)
{
    J1939Name name;
    name.identityNumber = identity;
    return name;
}

/// The claim a block put on the bus from `source`, or nothing when it put none.
[[nodiscard]] std::optional<J1939Name> claimedFrom(const std::vector<CanFrame>& frames,
                                                   std::uint8_t source)
{
    const std::vector<CanFrame> claims =
        withIdentifier(frames, j1939Identifier(kPgnAddressClaimed, source));

    if (claims.size() != 1U) {
        return std::nullopt;
    }

    return j1939NameFromClaim(claims.front());
}

} // namespace

TEST(TinyMlEcuTests, ByDefaultItIsASystemMonitorThatCannotMoveAndTheCatalogSaysSo)
{
    TinyDriver driver{j1939Config()};
    const std::vector<CanFrame> out =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::optional<J1939Name> name = claimedFrom(out, 0x80U);
    ASSERT_TRUE(name.has_value());

    EXPECT_EQ(name->industryGroup, 1U);
    EXPECT_EQ(name->function, 33U);
    EXPECT_EQ(name->identityNumber, 0x80U);
    EXPECT_EQ(name->manufacturerCode, 0U);
    EXPECT_FALSE(name->arbitraryAddressCapable);
}

TEST(TinyMlEcuTests, WhatItClaimsToBeIsWhatItWasToldToBe)
{
    TinyMlEcuConfig config = movingConfig();
    config.j1939Function = 7U;
    config.j1939Identity = 1'234'567U;

    TinyDriver driver{config};
    const std::vector<CanFrame> out =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::optional<J1939Name> name = claimedFrom(out, 0x80U);
    ASSERT_TRUE(name.has_value());

    EXPECT_EQ(name->function, 7U);
    EXPECT_EQ(name->identityNumber, 1'234'567U);
    EXPECT_TRUE(name->arbitraryAddressCapable);

    // A serial number is 21 bits: what does not fit is not carried over into the manufacturer's.
    config.j1939Identity = 0xFFFF'FFFFU;
    TinyDriver wide{config};
    const std::optional<J1939Name> clipped =
        claimedFrom(wide.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)}), 0x80U);

    ASSERT_TRUE(clipped.has_value());
    EXPECT_EQ(clipped->identityNumber, 0x1F'FFFFU);
    EXPECT_EQ(clipped->manufacturerCode, 0U);
}

TEST(TinyMlEcuTests, AnEcuThatCanMoveTakesTheLowestFreeAddressWhenItLosesTheOneItHas)
{
    SystemVariables variables;
    variables.set("tinyml.inject_fault", 1.5); // so that it has a trouble code to send as well
    TinyDriver driver{movingConfig(), &variables};

    const std::vector<CanFrame> first =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});
    const std::optional<J1939Name> before = claimedFrom(first, 0x80U);
    ASSERT_TRUE(before.has_value());

    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});

    // It claims the next address, under the NAME it had: moving does not make it another ECU.
    ASSERT_EQ(answer.size(), 1U);
    const std::optional<J1939Name> after = claimedFrom(answer, 0x81U);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->value(), before->value());
    EXPECT_EQ(after->identityNumber, 0x80U);

    // It has not given up: nothing is said about having no address.
    EXPECT_TRUE(withIdentifier(answer, kCannotClaim).empty());
    EXPECT_EQ(statistic(driver.node(), "J1939 address lost"), std::optional<std::uint64_t>{0U});
    EXPECT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x81U});

    // And everything it says from now on is said from there: the telemetry and the trouble code.
    std::vector<CanFrame> sent;
    for (std::uint64_t step = 0; step < 20; ++step) {
        const std::vector<CanFrame> out =
            driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, (300 + step * 100) * kMs),
                                             makeJ1939TempFrame(95, (350 + step * 100) * kMs)});
        sent.insert(sent.end(), out.begin(), out.end());
    }

    EXPECT_FALSE(withIdentifier(sent, 0x18FF0081U).empty());
    EXPECT_FALSE(withIdentifier(sent, 0x18FECA81U).empty());
    EXPECT_TRUE(withIdentifier(sent, 0x18FF0080U).empty());
    EXPECT_TRUE(withIdentifier(sent, 0x18FECA80U).empty());
}

TEST(TinyMlEcuTests, WhenItMovesItSkipsEveryAddressSomebodyElseHasClaimed)
{
    TinyDriver driver{movingConfig()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    // Two other ECUs hold the next two addresses; their NAMEs are higher than the winner's and
    // lower than nothing in particular - they are simply there.
    (void)driver.run(std::vector<CanFrame>{claimOf(0x81U, winningName(20U), 150 * kMs),
                                           claimOf(0x82U, winningName(21U), 160 * kMs)});

    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});

    EXPECT_TRUE(claimedFrom(answer, 0x83U).has_value());
    EXPECT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x83U});
}

TEST(TinyMlEcuTests, AnEcuThatMovedMovesAgainWhenTheAddressItMovedToIsLostToo)
{
    TinyDriver driver{movingConfig()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});
    ASSERT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x81U});

    // Somebody with a lower NAME wants 0x81 as well. 0x80 is taken, so 0x82.
    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{claimOf(0x81U, winningName(8U), 300 * kMs)});

    EXPECT_TRUE(claimedFrom(answer, 0x82U).has_value());
    EXPECT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x82U});
}

TEST(TinyMlEcuTests, WithNoPlaceLeftItFallsSilentAsOneThatCannotMoveDoes)
{
    TinyDriver driver{movingConfig()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    // Every address from 129 to 247 is somebody's.
    std::vector<CanFrame> crowd;
    for (unsigned address = 129U; address <= 247U; ++address) {
        crowd.push_back(
            claimOf(static_cast<std::uint8_t>(address), winningName(100U + address), 150 * kMs));
    }
    (void)driver.run(crowd);

    const std::uint64_t lost = 200 * kMs;
    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), lost)});

    // Its NAME has the top bit set, so it waits as long as that NAME says before it says so.
    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{busTime(lost + 200U * 1'000'000ULL)});

    ASSERT_EQ(answer.size(), 1U);
    EXPECT_EQ(answer.front().identifier, kCannotClaim);
    EXPECT_EQ(statistic(driver.node(), "J1939 address lost"), std::optional<std::uint64_t>{1U});

    const std::vector<CanFrame> after = driver.run(std::vector<CanFrame>{
        makeJ1939SpeedFrame(60.0, 300 * kMs), makeJ1939TempFrame(95, 350 * kMs)});
    EXPECT_TRUE(after.empty());
}
TEST(TinyMlEcuTests, ItLooksForAPlaceFromTheBottomOfTheRangeAndMayTakeTheLastAddressOfIt)
{
    // An ECU that was given an address above the bottom of the range looks from the bottom: the
    // lowest free address is 128 whatever address it lost.
    TinyMlEcuConfig high = j1939Config(0x18FF0090U);
    high.j1939ArbitraryAddress = true;

    TinyDriver climbing{high};
    (void)climbing.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    const std::vector<CanFrame> lowest =
        climbing.run(std::vector<CanFrame>{claimOf(0x90U, winningName(), 200 * kMs)});
    EXPECT_TRUE(claimedFrom(lowest, 0x80U).has_value());

    // And the top of it is a place: with every address below it taken, 247 is the one left.
    TinyDriver crowded{movingConfig()};
    (void)crowded.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    std::vector<CanFrame> neighbours;
    for (unsigned address = 129U; address <= 246U; ++address) {
        neighbours.push_back(
            claimOf(static_cast<std::uint8_t>(address), winningName(100U + address), 150 * kMs));
    }
    (void)crowded.run(neighbours);

    const std::vector<CanFrame> last =
        crowded.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});
    EXPECT_TRUE(claimedFrom(last, 247U).has_value());
    EXPECT_EQ(statistic(crowded.node(), "J1939 address"), std::optional<std::uint64_t>{247U});
}

TEST(TinyMlEcuTests, AnEcuThatMovedAnswersARequestFromWhereItIsNow)
{
    TinyDriver driver{movingConfig()};
    const std::vector<CanFrame> started =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});
    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});

    // What its NAME says it waits, when asked of everyone.
    const std::optional<J1939Name> name = claimedFrom(started, 0x80U);
    ASSERT_TRUE(name.has_value());
    const std::uint64_t delay = j1939ClaimDelayNs(*name);

    const auto answers = [&driver, delay](std::uint8_t destination, std::uint8_t from) {
        const std::uint64_t asked = 300 * kMs;
        std::vector<CanFrame> out =
            driver.run(std::vector<CanFrame>{requestFor(kPgnAddressClaimed, destination, asked)});

        // And the time of the bus moving on, for the answers that wait.
        const std::vector<CanFrame> later =
            driver.run(std::vector<CanFrame>{busTime(asked + delay)});
        out.insert(out.end(), later.begin(), later.end());

        return withIdentifier(out, j1939Identifier(kPgnAddressClaimed, from)).size();
    };

    // To everyone, and to the address it has now: from the address it has now.
    EXPECT_EQ(answers(kJ1939GlobalAddress, 0x81U), 1U);
    EXPECT_EQ(answers(0x81U, 0x81U), 1U);

    // To the one it left, which belongs to somebody else now: not its to answer.
    EXPECT_EQ(answers(0x80U, 0x81U), 0U);
}
TEST(TinyMlEcuTests, AnEcuThatCanMoveStillDefendsTheAddressAgainstAHigherName)
{
    // Moving is for the one that loses. Against a NAME that is higher than its own - another ECU
    // that can move, with a bigger serial number - it keeps the address and says so again.
    TinyDriver driver{movingConfig()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});

    J1939Name higher;
    higher.arbitraryAddressCapable = true;
    higher.industryGroup = 1U;
    higher.function = 33U;
    higher.identityNumber = 0x1000U;

    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{claimOf(0x80U, higher, 200 * kMs)});

    EXPECT_EQ(withIdentifier(answer, kOwnClaim).size(), 1U);
    EXPECT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x80U});
}

TEST(TinyMlEcuTests, ANewMeasurementStartsAtTheAddressItWasMeantToHaveAndForgetsWhoWasThere)
{
    TinyDriver driver{movingConfig()};
    (void)driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});
    (void)driver.run(std::vector<CanFrame>{claimOf(0x81U, winningName(20U), 150 * kMs)});
    (void)driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});
    ASSERT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x82U});

    // Switched on again: it claims the address it was given, and the bus is a bus it knows nothing
    // about yet - the ECU at 0x81 of the last measurement is not one of this one.
    driver.restart();

    const std::vector<CanFrame> out =
        driver.run(std::vector<CanFrame>{makeJ1939SpeedFrame(60.0, 100 * kMs)});
    EXPECT_EQ(withIdentifier(out, kOwnClaim).size(), 1U);
    EXPECT_EQ(statistic(driver.node(), "J1939 address"), std::optional<std::uint64_t>{0x80U});

    const std::vector<CanFrame> answer =
        driver.run(std::vector<CanFrame>{claimOf(0x80U, winningName(), 200 * kMs)});
    EXPECT_TRUE(claimedFrom(answer, 0x81U).has_value());
}

TEST(TinyMlEcuTests, TheCatalogGivesTheBlockTheNameAndTheAbilityItWasGiven)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeParameters parameters;
    parameters.set("j1939", ParameterValue::fromBoolean(true));
    parameters.set("j1939Function", ParameterValue::fromInteger(7));
    parameters.set("j1939Identity", ParameterValue::fromInteger(99));
    parameters.set("j1939ArbitraryAddress", ParameterValue::fromBoolean(true));

    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> built;
    ASSERT_TRUE(catalog.create("tinyml.ecu", parameters, context, "tiny", built).succeeded());

    const auto* node = dynamic_cast<const TinyMlEcuNode*>(built.get());
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->config().j1939Function, 7U);
    ASSERT_TRUE(node->config().j1939Identity.has_value());
    EXPECT_EQ(*node->config().j1939Identity, 99U);
    EXPECT_TRUE(node->config().j1939ArbitraryAddress);

    // And a block that was told nothing keeps what it always was.
    NodeParameters nothing;
    nothing.set("j1939", ParameterValue::fromBoolean(true));
    std::unique_ptr<IPipelineNode> plain;
    ASSERT_TRUE(catalog.create("tinyml.ecu", nothing, context, "plain", plain).succeeded());

    const auto* plainNode = dynamic_cast<const TinyMlEcuNode*>(plain.get());
    ASSERT_NE(plainNode, nullptr);
    EXPECT_EQ(plainNode->config().j1939Function, 33U);
    EXPECT_FALSE(plainNode->config().j1939Identity.has_value());
    EXPECT_FALSE(plainNode->config().j1939ArbitraryAddress);
}

TEST(TinyMlEcuTests, AFunctionOrASerialNumberThatDoesNotFitItsFieldIsRefusedBeforeStart)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeParameters tooBigFunction;
    tooBigFunction.set("j1939Function", ParameterValue::fromInteger(256));
    const Result function = catalog.validateParameters("tinyml.ecu", tooBigFunction, "tiny");
    ASSERT_TRUE(function.failed());
    EXPECT_NE(std::string{function.message()}.find("J1939 Function"), std::string::npos)
        << std::string{function.message()};

    NodeParameters tooBigSerial;
    tooBigSerial.set("j1939Identity", ParameterValue::fromInteger(2'097'152));
    const Result serial = catalog.validateParameters("tinyml.ecu", tooBigSerial, "tiny");
    ASSERT_TRUE(serial.failed());
    EXPECT_NE(std::string{serial.message()}.find("J1939 Serial Number"), std::string::npos)
        << std::string{serial.message()};

    NodeParameters fits;
    fits.set("j1939Function", ParameterValue::fromInteger(255));
    fits.set("j1939Identity", ParameterValue::fromInteger(2'097'151));
    EXPECT_TRUE(catalog.validateParameters("tinyml.ecu", fits, "tiny").succeeded());
}

TEST(TinyMlEcuTests, TheElevenBitVehicleHasNoAddressesAndNothingToClaim)
{
    TinyDriver driver{TinyMlEcuConfig{}};

    const std::vector<CanFrame> frames{makeSpeedFrame(60.0, 100 * kMs),
                                       makeTempFrame(88, 150 * kMs)};
    const std::vector<CanFrame> out = driver.run(frames);

    ASSERT_FALSE(out.empty());

    for (const CanFrame& frame : out) {
        EXPECT_EQ(frame.format, CanFrameFormat::Standard);
    }
}

TEST(TinyMlEcuTests, ASensorThatReportsErrorOrNotAvailableIsBusStressAndRecovers)
{
    // A coolant temperature of 0xFF is not 215 degC and is not nothing: the ECU is saying it cannot
    // be believed, which is what the stress feature is for. It does not stick - every valid speed
    // frame takes some off - so a sensor that comes back stops counting.
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> fault{makeJ1939SpeedFrame(60.0, 100 * kMs), [] {
                                          CanFrame frame = makeJ1939TempFrame(60, 150 * kMs);
                                          frame.data[0] = 0xFFU;
                                          return frame;
                                      }()};
    driver.run(fault);
    const float stressed = driver.node().lastFeatures().busStressFactor;
    EXPECT_GT(stressed, 0.3F);

    for (std::uint64_t step = 1; step <= 30; ++step) {
        const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, (100 + step * 100) * kMs)};
        driver.run(frames);
    }

    EXPECT_LT(driver.node().lastFeatures().busStressFactor, 0.05F);
}

TEST(TinyMlEcuTests, ATelemetryIdentifierThatIsNotAnIdentifierIsRefusedBeforeStart)
{
    // 0x90FF0080 is how a .dbc writes the extended identifier 0x10FF0080: the database adds
    // 0x80000000 to say so. Used as it was it built a block that classified the bus perfectly and
    // put every telemetry frame on it with an identifier no driver accepts - each one a failed
    // transmit that nothing reported.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "tinyml_ecu",
        .typeName = "tinyml.ecu",
        .parameters = {{"outputCanId", ParameterValue::fromInteger(0x90FF0080LL)}},
    });

    const Result checked = description.validate(catalog);
    ASSERT_TRUE(checked.failed());
    EXPECT_NE(std::string{checked.message()}.find("not a CAN identifier"), std::string::npos)
        << std::string{checked.message()};
    EXPECT_NE(std::string{checked.message()}.find("tinyml_ecu"), std::string::npos);

    // And the right one is accepted.
    description.nodes().front().parameters.set("outputCanId",
                                               ParameterValue::fromInteger(0x18FF0080LL));
    EXPECT_TRUE(description.validate(catalog).succeeded());
}

TEST(TinyMlEcuTests, TheJ1939SwitchGivesTheJ1939IdentifierWhenNoneIsNamed)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    GraphDescription description;
    description.addNode(NodeDescription{
        .id = "can_1",
        .typeName = "can.source",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addNode(NodeDescription{
        .id = "tinyml_ecu",
        .typeName = "tinyml.ecu",
        .parameters = {{"j1939", ParameterValue::fromBoolean(true)}},
    });
    description.addNode(NodeDescription{
        .id = "tx_1",
        .typeName = "can.transmit",
        .parameters = {{"channel", ParameterValue::fromInteger(0)}},
    });
    description.addEdge(EdgeDescription{"can_1", 0, "tinyml_ecu", 0});
    description.addEdge(EdgeDescription{"tinyml_ecu", 0, "tx_1", 0});
    ASSERT_TRUE(description.validate(catalog).succeeded());

    engine.setGraphDescription(description, catalog);

    std::mutex mutex;
    std::vector<CanFrame> seen;
    engine.addFrameSink([&](std::span<const CanFrame> batch) {
        const std::lock_guard lock{mutex};
        seen.insert(seen.end(), batch.begin(), batch.end());
    });

    ASSERT_TRUE(engine.start().succeeded());
    ASSERT_TRUE(engine.transmit(0, makeJ1939SpeedFrame(50.0, 100 * kMs)).succeeded());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    engine.stop();

    const std::lock_guard lock{mutex};
    EXPECT_FALSE(withIdentifier(seen, 0x18FF0080U).empty());
}

namespace {

/// EEC2 from the engine: byte 3 is the load at the current speed, 1 % a bit and up to 250 %.
[[nodiscard]] CanFrame makeJ1939LoadFrame(int percent, std::uint64_t timestampNs)
{
    CanFrame frame{};
    frame.identifier = 0x0CF00300; // EEC2, priority 3, from address 0
    frame.format = CanFrameFormat::Extended;
    frame.dlc = 8;
    frame.length = 8;
    frame.timestampNs = timestampNs;
    std::fill(frame.data.begin(), frame.data.end(), std::uint8_t{0xFF});
    frame.data[2] = static_cast<std::uint8_t>(std::clamp(percent, 0, 250));
    return frame;
}

} // namespace

TEST(TinyMlEcuTests, ADieselAtFullThrottleIsWorkingAndNotFailing)
{
    // Pedal to the floor from a standstill is 92 % load and 15 km/h/s, and it used to count as bus
    // stress: the model called every hard acceleration an anomaly with no thermal health at all,
    // and the cluster lit its red stop lamp for it.
    TinyDriver driver{j1939Config()};

    std::vector<CanFrame> sent;
    double speed = 0.0;
    for (std::uint64_t tick = 0; tick < 60; ++tick) {
        speed = std::min(60.0, speed + 1.5);

        std::vector<CanFrame> frames{makeJ1939SpeedFrame(speed, (100 + tick * 100) * kMs),
                                     makeJ1939LoadFrame(92, (120 + tick * 100) * kMs),
                                     makeJ1939LoadFrame(92, (170 + tick * 100) * kMs)};
        if (tick % 10 == 0) {
            frames.push_back(makeJ1939TempFrame(88, (150 + tick * 100) * kMs));
        }

        const std::vector<CanFrame> out = driver.run(frames);
        sent.insert(sent.end(), out.begin(), out.end());
    }

    const std::vector<CanFrame> telemetry = withIdentifier(sent, 0x18FF0080U);
    ASSERT_FALSE(telemetry.empty());

    EXPECT_LT(percent(telemetry.back().data[1]), 40.0) << "anomaly score";
    EXPECT_GT(percent(telemetry.back().data[0]), 70.0) << "thermal health";
    EXPECT_TRUE(withIdentifier(sent, 0x18FECA80U).empty());
}

TEST(TinyMlEcuTests, AnEngineAskedForMoreThanItHasAtThatSpeedIsOutOfProfile)
{
    // SPN 92 goes to 250 %, and above 100 the engine is being asked for more than it can give.
    TinyDriver driver{j1939Config()};

    const std::vector<CanFrame> frames{makeJ1939SpeedFrame(60.0, 100 * kMs),
                                       makeJ1939LoadFrame(140, 120 * kMs),
                                       makeJ1939LoadFrame(140, 170 * kMs)};
    driver.run(frames);

    EXPECT_GT(driver.node().lastFeatures().busStressFactor, 0.5F);
}

TEST(TinyMlEcuTests, ALayerWhoseSizeIsNotBelievableIsRefusedBeforeAnythingIsReadFromIt)
{
    // The weights are inputSize * outputSize, both read from text. Two numbers whose
    // product wrapped past 64 bits passed the check that the file had enough weights,
    // and the forward pass then read the rest of the matrix from memory that was never
    // allocated.
    TinyMlModel model;

    // 2^32 * 2^32 wraps to exactly 0.
    const Result wrapping =
        TinyMlModel::parse("TBUSML 1\nlayer l 4294967296 4294967296 linear 0.1\n", model);
    ASSERT_TRUE(wrapping.failed());
    EXPECT_NE(std::string{wrapping.message()}.find("larger than this block runs"),
              std::string::npos)
        << wrapping.message();

    // Large but not wrapping: still not a network that runs in a CAN pass.
    const Result large = TinyMlModel::parse("TBUSML 1\nlayer l 6 100000 linear 0.1\n", model);
    EXPECT_TRUE(large.failed());

    const Result wide = TinyMlModel::parse("TBUSML 1\nlayer l 4000 4000 linear 0.1\n", model);
    EXPECT_TRUE(wide.failed()) << "16 million weights in one layer";

    // And the model that ships still loads, which is the other half of the rule.
    EXPECT_EQ(TinyMlModel::builtinPowertrainModel().layers().size(), 3U);
}
