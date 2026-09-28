// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A Virtual ECU node that runs embedded TinyML neural network inference on
// live CAN traffic (`tinyml.ecu`).
//
// Like a physical automotive AI/Edge gateway or smart sensor ECU, this block
// listens to raw CAN frames on its input port, extracts a sliding-window
// feature vector (vehicle speed, longitudinal acceleration, engine coolant
// temperature, thermal deviation from nominal load, and frame inter-arrival
// jitter), executes a quantized int8 neural network inside a fixed static
// `TinyMlArena`, and publishes both:
//
//   - Port 0 (`frames`, PortType::Frames): an 8-byte CAN telemetry frame
//     (default ID 0x105 / 261) carrying the classified operating regime,
//     confidence, anomaly score, virtual thermal health sensor, and inference
//     latency so downstream ECUs, loggers, and the CAN Trace see it on the bus.
//   - Port 1 (`signals`, PortType::Signals): five decoded physical signals
//     (`AnomalyScore`, `Confidence`, `RegimeClass`, `ThermalHealth`,
//     `InferenceTimeUs`) ready to wire directly into a `signal.plot` block.

#pragma once

#include "core/can/CanFrame.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"
#include "core/database/DecodedSignal.h"
#include "core/pipeline/PipelineNode.h"
#include "core/tinyml/TinyMlModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// Configuration parameters for `TinyMlEcuNode`.
struct TinyMlEcuConfig final {
    std::uint8_t transmitChannel{0};
    std::uint32_t speedCanId{0x101};
    std::uint32_t tempCanId{0x102};
    std::uint32_t outputCanId{0x105};
    float anomalyThresholdPercent{65.0F};
    std::uint32_t inferenceStride{1};
    std::size_t arenaCapacityBytes{TinyMlArena::kDefaultCapacityBytes};
};

class TinyMlEcuNode final : public IPipelineNode {
public:
    static constexpr std::size_t kSignalsPerInference = 5;

    explicit TinyMlEcuNode(TinyMlModel model = TinyMlModel::builtinPowertrainModel(),
                           TinyMlEcuConfig config = {},
                           std::string name = "TinyML Virtual ECU");

    [[nodiscard]] std::string_view typeName() const noexcept override { return "tinyml.ecu"; }
    [[nodiscard]] std::string displayName() const override { return m_name; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// Binds optional system variables so Dashboard widgets and the Canvas
    /// simulation overlay can read live TinyML telemetry or inject fault stress.
    void setSystemVariables(SystemVariables* variables) noexcept { m_variables = variables; }

    /// Pre-allocates the static tensor arena and worst-case output buffers so
    /// `process()` performs zero heap allocations on the measurement thread.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override;

    [[nodiscard]] const TinyMlModel& model() const noexcept { return m_model; }
    [[nodiscard]] const TinyMlEcuConfig& config() const noexcept { return m_config; }
    [[nodiscard]] const TinyMlArena& arena() const noexcept { return m_arena; }
    [[nodiscard]] const TinyMlInferenceResult& lastResult() const noexcept { return m_lastResult; }
    [[nodiscard]] const TinyMlFeatureVector& lastFeatures() const noexcept
    {
        return m_lastFeatures;
    }

    [[nodiscard]] std::uint64_t framesAnalysed() const noexcept { return m_framesAnalysed; }
    [[nodiscard]] std::uint64_t inferencesRun() const noexcept { return m_inferencesRun; }
    [[nodiscard]] std::uint64_t anomaliesDetected() const noexcept { return m_anomaliesDetected; }

private:
    void setupTelemetryDefinitions();
    void appendDecodedSignals(std::uint64_t timestampNs,
                              const TinyMlInferenceResult& result,
                              std::uint16_t inferenceUs) noexcept;
    void publishToSystemVariables(const TinyMlInferenceResult& result,
                                  std::uint16_t inferenceUs) noexcept;

    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
    static constexpr std::array<PortDescriptor, 2> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
        PortDescriptor{"signals", PortType::Signals},
    };

    TinyMlModel m_model;
    TinyMlEcuConfig m_config;
    std::string m_name;
    TinyMlArena m_arena;

    SystemVariables* m_variables{nullptr};
    SystemVariables::Handle m_varAnomalyScore{SystemVariables::kUnknown};
    SystemVariables::Handle m_varConfidence{SystemVariables::kUnknown};
    SystemVariables::Handle m_varRegime{SystemVariables::kUnknown};
    SystemVariables::Handle m_varThermalHealth{SystemVariables::kUnknown};
    SystemVariables::Handle m_varInferenceUs{SystemVariables::kUnknown};
    SystemVariables::Handle m_varInferences{SystemVariables::kUnknown};
    SystemVariables::Handle m_varAnomalies{SystemVariables::kUnknown};
    SystemVariables::Handle m_varInjectFault{SystemVariables::kUnknown};

    /// Long-lived signal definitions referenced by `DecodedSignal` spans on
    /// output port 1 (`PortType::Signals`).
    CanMessage m_telemetryMessage;
    std::array<CanSignal, kSignalsPerInference> m_telemetrySignals{};

    /// Pre-allocated output buffers reused every pass (Rule #12).
    std::vector<CanFrame> m_outgoingFrames;
    std::vector<DecodedSignal> m_outgoingSignals;

    /// Sliding feature state updated as CAN frames arrive.
    TinyMlFeatureVector m_lastFeatures{};
    TinyMlInferenceResult m_lastResult{};
    std::uint64_t m_lastSpeedTimestampNs{0};
    float m_expectedSpeedIntervalSec{0.1F};
    bool m_hasPreviousSpeed{false};
    std::uint32_t m_strideCounter{0};

    /// Node telemetry counters exposed through `statistics()`.
    std::uint64_t m_framesAnalysed{0};
    std::uint64_t m_inferencesRun{0};
    std::uint64_t m_anomaliesDetected{0};
    std::uint64_t m_lastInferenceUs{0};
};

} // namespace torquebus
