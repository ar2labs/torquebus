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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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

    ASSERT_EQ(emittedFrames.size(), 3U);
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
