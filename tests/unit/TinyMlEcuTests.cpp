// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include <gtest/gtest.h>

#include "core/can/CanEngine.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/DecodedSignal.h"
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

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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
