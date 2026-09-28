// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/tinyml/TinyMlEcuNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace torquebus {

TinyMlEcuNode::TinyMlEcuNode(TinyMlModel model, TinyMlEcuConfig config, std::string name)
    : m_model{std::move(model)}
    , m_config{config}
    , m_name{std::move(name)}
    , m_arena{std::max(config.arenaCapacityBytes, m_model.requiredArenaBytes())}
{
    if (m_config.inferenceStride == 0) {
        m_config.inferenceStride = 1;
    }
    setupTelemetryDefinitions();
}

void TinyMlEcuNode::setupTelemetryDefinitions()
{
    m_telemetryMessage = CanMessage{};
    m_telemetryMessage.name = "TinyML_Telemetry";
    m_telemetryMessage.identifier = m_config.outputCanId;
    m_telemetryMessage.length = 8;
    m_telemetryMessage.transmitter = "TINYML_ECU";

    // Signal 0: RegimeClass (Byte 0, uint8, 0..4)
    CanSignal regimeSig;
    regimeSig.name = "RegimeClass";
    regimeSig.startBit = 0;
    regimeSig.bitLength = 8;
    regimeSig.byteOrder = ByteOrder::Intel;
    regimeSig.isSigned = false;
    regimeSig.factor = 1.0;
    regimeSig.offset = 0.0;
    regimeSig.minimum = 0.0;
    regimeSig.maximum = 4.0;
    regimeSig.unit = "class";
    regimeSig.valueNames = {
        {0, "Idle"},
        {1, "Cruise"},
        {2, "HighLoad"},
        {3, "ThermalStress"},
        {4, "Anomaly"},
    };

    // Signal 1: Confidence (Byte 1, uint8, 0..100 %)
    CanSignal confidenceSig;
    confidenceSig.name = "Confidence";
    confidenceSig.startBit = 8;
    confidenceSig.bitLength = 8;
    confidenceSig.byteOrder = ByteOrder::Intel;
    confidenceSig.isSigned = false;
    confidenceSig.factor = 1.0;
    confidenceSig.offset = 0.0;
    confidenceSig.minimum = 0.0;
    confidenceSig.maximum = 100.0;
    confidenceSig.unit = "%";

    // Signal 2: AnomalyScore (Bytes 2..3, uint16 LE, factor 0.1 %)
    CanSignal anomalySig;
    anomalySig.name = "AnomalyScore";
    anomalySig.startBit = 16;
    anomalySig.bitLength = 16;
    anomalySig.byteOrder = ByteOrder::Intel;
    anomalySig.isSigned = false;
    anomalySig.factor = 0.1;
    anomalySig.offset = 0.0;
    anomalySig.minimum = 0.0;
    anomalySig.maximum = 100.0;
    anomalySig.unit = "%";

    // Signal 3: ThermalHealth (Bytes 4..5, uint16 LE, factor 0.1 %)
    CanSignal healthSig;
    healthSig.name = "ThermalHealth";
    healthSig.startBit = 32;
    healthSig.bitLength = 16;
    healthSig.byteOrder = ByteOrder::Intel;
    healthSig.isSigned = false;
    healthSig.factor = 0.1;
    healthSig.offset = 0.0;
    healthSig.minimum = 0.0;
    healthSig.maximum = 100.0;
    healthSig.unit = "%";

    // Signal 4: InferenceTimeUs (Bytes 6..7, uint16 LE, factor 1 us)
    CanSignal latencySig;
    latencySig.name = "InferenceTimeUs";
    latencySig.startBit = 48;
    latencySig.bitLength = 16;
    latencySig.byteOrder = ByteOrder::Intel;
    latencySig.isSigned = false;
    latencySig.factor = 1.0;
    latencySig.offset = 0.0;
    latencySig.minimum = 0.0;
    latencySig.maximum = 65535.0;
    latencySig.unit = "us";

    m_telemetrySignals[0] = std::move(regimeSig);
    m_telemetrySignals[1] = std::move(confidenceSig);
    m_telemetrySignals[2] = std::move(anomalySig);
    m_telemetrySignals[3] = std::move(healthSig);
    m_telemetrySignals[4] = std::move(latencySig);
}

Result TinyMlEcuNode::prepare(std::size_t maximumBatchSize)
{
    if (m_model.layers().empty()) {
        return Result::error(ErrorCode::InvalidArgument,
                             "TinyML Virtual ECU '" + m_name
                                 + "' has an empty neural network model");
    }

    const std::size_t requiredBytes = m_model.requiredArenaBytes();
    if (m_config.arenaCapacityBytes < requiredBytes) {
        return Result::error(ErrorCode::InvalidState,
                             "TinyML Virtual ECU '" + m_name + "' arena capacity ("
                                 + std::to_string(m_config.arenaCapacityBytes)
                                 + " B) is smaller than model requirement ("
                                 + std::to_string(requiredBytes) + " B)");
    }

    m_arena.configure(m_config.arenaCapacityBytes);
    setupTelemetryDefinitions();

    const std::size_t effectiveBatch = std::max<std::size_t>(maximumBatchSize, 64U);
    m_outgoingFrames.clear();
    m_outgoingFrames.reserve(effectiveBatch);
    m_outgoingSignals.clear();
    m_outgoingSignals.reserve(effectiveBatch * kSignalsPerInference);

    m_lastFeatures = TinyMlFeatureVector{};
    m_lastResult = TinyMlInferenceResult{};
    m_lastSpeedTimestampNs = 0;
    m_expectedSpeedIntervalSec = 0.1F;
    m_hasPreviousSpeed = false;
    m_strideCounter = 0;

    m_framesAnalysed = 0;
    m_inferencesRun = 0;
    m_anomaliesDetected = 0;
    m_lastInferenceUs = 0;

    if (m_variables != nullptr) {
        m_varAnomalyScore = m_variables->resolve("tinyml.anomaly_score");
        m_varConfidence = m_variables->resolve("tinyml.confidence");
        m_varRegime = m_variables->resolve("tinyml.regime");
        m_varThermalHealth = m_variables->resolve("tinyml.thermal_health");
        m_varInferenceUs = m_variables->resolve("tinyml.inference_us");
        m_varInferences = m_variables->resolve("tinyml.inferences");
        m_varAnomalies = m_variables->resolve("tinyml.anomalies");
        m_varInjectFault = m_variables->resolve("tinyml.inject_fault");

        m_variables->set(m_varAnomalyScore, 0.0);
        m_variables->set(m_varConfidence, 100.0);
        m_variables->set(m_varRegime, 0.0);
        m_variables->set(m_varThermalHealth, 100.0);
        m_variables->set(m_varInferenceUs, 0.0);
        m_variables->set(m_varInferences, 0.0);
        m_variables->set(m_varAnomalies, 0.0);
    }

    return Result::ok();
}

void TinyMlEcuNode::process(NodeContext& context)
{
    m_outgoingFrames.clear();
    m_outgoingSignals.clear();

    const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
    if (incoming.empty()) {
        return;
    }

    for (const CanFrame& frame : incoming) {
        if (frame.identifier == m_config.outputCanId) {
            continue;
        }

        ++m_framesAnalysed;
        bool shouldInfer = false;

        if (frame.identifier == m_config.speedCanId && frame.length >= 2) {
            const auto rawSpeed = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(frame.data[0])
                | static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.data[1]) << 8U));
            const float newSpeedKmh = static_cast<float>(rawSpeed) * 0.1F;

            if (m_hasPreviousSpeed && frame.timestampNs > m_lastSpeedTimestampNs) {
                const float dtSec =
                    static_cast<float>(frame.timestampNs - m_lastSpeedTimestampNs) * 1.0e-9F;
                if (dtSec > 1.0e-4F && dtSec < 5.0F) {
                    const float instantAccel = (newSpeedKmh - m_lastFeatures.speedKmh) / dtSec;
                    m_lastFeatures.accelerationKmhPerSec =
                        0.6F * m_lastFeatures.accelerationKmhPerSec + 0.4F * instantAccel;

                    const float jitter = std::fabs(dtSec - m_expectedSpeedIntervalSec)
                                         / std::max(m_expectedSpeedIntervalSec, 0.01F);
                    m_lastFeatures.timingJitterRatio = std::clamp(
                        0.7F * m_lastFeatures.timingJitterRatio + 0.3F * jitter, 0.0F, 2.0F);
                    m_expectedSpeedIntervalSec =
                        std::clamp(0.9F * m_expectedSpeedIntervalSec + 0.1F * dtSec, 0.005F, 1.0F);
                }
            } else {
                m_hasPreviousSpeed = true;
            }

            m_lastSpeedTimestampNs = frame.timestampNs;
            m_lastFeatures.speedKmh = newSpeedKmh;

            const float expectedTemp = 70.0F + 0.12F * m_lastFeatures.speedKmh;
            m_lastFeatures.thermalDeltaDegC = m_lastFeatures.engineTempDegC - expectedTemp;

            m_lastFeatures.busStressFactor *= 0.85F;
            if (frame.dlc != frame.length || newSpeedKmh > 220.0F) {
                m_lastFeatures.busStressFactor =
                    std::min(1.5F, m_lastFeatures.busStressFactor + 0.75F);
            }
            shouldInfer = true;
        } else if (frame.identifier == m_config.tempCanId && frame.length >= 1) {
            const float newTempDegC = static_cast<float>(frame.data[0]) - 40.0F;
            m_lastFeatures.engineTempDegC = newTempDegC;

            const float expectedTemp = 70.0F + 0.12F * m_lastFeatures.speedKmh;
            m_lastFeatures.thermalDeltaDegC = newTempDegC - expectedTemp;

            if (newTempDegC > 105.0F || newTempDegC < -20.0F) {
                m_lastFeatures.busStressFactor =
                    std::min(1.5F, m_lastFeatures.busStressFactor + (newTempDegC - 100.0F) * 0.04F);
            }
            shouldInfer = true;
        } else if (m_config.speedCanId == 0U && frame.length >= 1) {
            m_lastFeatures.speedKmh = static_cast<float>(frame.data[0]);
            shouldInfer = true;
        }

        if (!shouldInfer) {
            continue;
        }

        if (++m_strideCounter < m_config.inferenceStride) {
            continue;
        }
        m_strideCounter = 0;

        TinyMlFeatureVector activeFeatures = m_lastFeatures;
        if (m_variables != nullptr && m_varInjectFault != SystemVariables::kUnknown) {
            const double faultLevel = m_variables->value(m_varInjectFault);
            if (faultLevel > 0.05) {
                const auto intensity = static_cast<float>(std::clamp(faultLevel, 0.0, 2.0));
                activeFeatures.engineTempDegC += 32.0F * intensity;
                activeFeatures.thermalDeltaDegC += 28.0F * intensity;
                activeFeatures.timingJitterRatio =
                    std::max(activeFeatures.timingJitterRatio, 0.85F * intensity);
                activeFeatures.busStressFactor =
                    std::max(activeFeatures.busStressFactor, 0.95F * intensity);
            }
        }

        const auto startTime = std::chrono::steady_clock::now();
        TinyMlInferenceResult result{};
        if (m_model.evaluate(activeFeatures, m_arena, m_config.anomalyThresholdPercent, result)
                .failed()) {
            continue;
        }
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - startTime)
                                   .count();
        const auto inferenceUs = static_cast<std::uint16_t>(
            std::clamp<std::int64_t>((elapsedNs + 999) / 1000, 1, 65535));

        m_lastResult = result;
        m_lastInferenceUs = inferenceUs;
        ++m_inferencesRun;
        if (result.isAnomaly) {
            ++m_anomaliesDetected;
        }

        if (m_outgoingFrames.size() < m_outgoingFrames.capacity()) {
            CanFrame telemetry{};
            telemetry.identifier = m_config.outputCanId;
            telemetry.channel = m_config.transmitChannel;
            telemetry.direction = CanDirection::Tx;
            telemetry.format = (m_config.outputCanId > kMaxStandardIdentifier)
                                   ? CanFrameFormat::Extended
                                   : CanFrameFormat::Standard;
            telemetry.dlc = 8;
            telemetry.length = 8;
            telemetry.timestampNs = frame.timestampNs;

            const auto regimeByte = static_cast<std::uint8_t>(result.regime);
            const auto confidenceByte = static_cast<std::uint8_t>(
                std::clamp<long>(std::lround(result.confidencePercent), 0L, 100L));
            const auto anomalyRaw = static_cast<std::uint16_t>(
                std::clamp<long>(std::lround(result.anomalyScorePercent * 10.0F), 0L, 1000L));
            const auto healthRaw = static_cast<std::uint16_t>(
                std::clamp<long>(std::lround(result.thermalHealthPercent * 10.0F), 0L, 1000L));

            telemetry.data[0] = regimeByte;
            telemetry.data[1] = confidenceByte;
            telemetry.data[2] = static_cast<std::uint8_t>(anomalyRaw & 0xFFU);
            telemetry.data[3] = static_cast<std::uint8_t>((anomalyRaw >> 8U) & 0xFFU);
            telemetry.data[4] = static_cast<std::uint8_t>(healthRaw & 0xFFU);
            telemetry.data[5] = static_cast<std::uint8_t>((healthRaw >> 8U) & 0xFFU);
            telemetry.data[6] = static_cast<std::uint8_t>(inferenceUs & 0xFFU);
            telemetry.data[7] = static_cast<std::uint8_t>((inferenceUs >> 8U) & 0xFFU);

            m_outgoingFrames.push_back(telemetry);
        }

        appendDecodedSignals(frame.timestampNs, result, inferenceUs);
        publishToSystemVariables(result, inferenceUs);
    }

    if (!m_outgoingFrames.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoingFrames});
    }
    if (!m_outgoingSignals.empty()) {
        context.publish<DecodedSignal>(1, std::span<const DecodedSignal>{m_outgoingSignals});
    }
}

void TinyMlEcuNode::appendDecodedSignals(std::uint64_t timestampNs,
                                         const TinyMlInferenceResult& result,
                                         std::uint16_t inferenceUs) noexcept
{
    if (m_outgoingSignals.size() + kSignalsPerInference > m_outgoingSignals.capacity()) {
        return;
    }

    const auto pushSignal = [&](std::size_t index, double physicalValue, std::int64_t rawValue) {
        DecodedSignal sample{};
        sample.timestampNs = timestampNs;
        sample.message = &m_telemetryMessage;
        sample.signal = &m_telemetrySignals[index];
        sample.value = physicalValue;
        sample.raw = rawValue;
        sample.identifier = m_config.outputCanId;
        sample.channel = m_config.transmitChannel;
        sample.truncated = false;
        m_outgoingSignals.push_back(sample);
    };

    const auto regimeRaw = static_cast<std::int64_t>(result.regime);
    const auto confidenceRaw = static_cast<std::int64_t>(std::lround(result.confidencePercent));
    const auto anomalyRaw =
        static_cast<std::int64_t>(std::lround(result.anomalyScorePercent * 10.0F));
    const auto healthRaw =
        static_cast<std::int64_t>(std::lround(result.thermalHealthPercent * 10.0F));

    pushSignal(0, static_cast<double>(regimeRaw), regimeRaw);
    pushSignal(1, static_cast<double>(result.confidencePercent), confidenceRaw);
    pushSignal(2, static_cast<double>(result.anomalyScorePercent), anomalyRaw);
    pushSignal(3, static_cast<double>(result.thermalHealthPercent), healthRaw);
    pushSignal(4, static_cast<double>(inferenceUs), static_cast<std::int64_t>(inferenceUs));
}

void TinyMlEcuNode::publishToSystemVariables(const TinyMlInferenceResult& result,
                                             std::uint16_t inferenceUs) noexcept
{
    if (m_variables == nullptr) {
        return;
    }

    m_variables->set(m_varAnomalyScore, static_cast<double>(result.anomalyScorePercent));
    m_variables->set(m_varConfidence, static_cast<double>(result.confidencePercent));
    m_variables->set(m_varRegime, static_cast<double>(static_cast<std::uint8_t>(result.regime)));
    m_variables->set(m_varThermalHealth, static_cast<double>(result.thermalHealthPercent));
    m_variables->set(m_varInferenceUs, static_cast<double>(inferenceUs));
    m_variables->set(m_varInferences, static_cast<double>(m_inferencesRun));
    m_variables->set(m_varAnomalies, static_cast<double>(m_anomaliesDetected));
}

std::vector<NodeStatistic> TinyMlEcuNode::statistics() const
{
    return {
        {"Frames analysed", m_framesAnalysed},
        {"Inferences run", m_inferencesRun},
        {"Anomalies detected", m_anomaliesDetected},
        {"Anomaly score (%)",
         static_cast<std::uint64_t>(std::lround(m_lastResult.anomalyScorePercent))},
        {"Regime class", static_cast<std::uint64_t>(m_lastResult.regime)},
        {"Confidence (%)", static_cast<std::uint64_t>(std::lround(m_lastResult.confidencePercent))},
        {"Thermal health (%)",
         static_cast<std::uint64_t>(std::lround(m_lastResult.thermalHealthPercent))},
        {"Last inference (us)", m_lastInferenceUs},
        {"Arena bytes used", m_arena.highWaterMarkBytes()},
    };
}

} // namespace torquebus
