// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/tinyml/TinyMlEcuNode.h"

#include "core/j1939/J1939Id.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace torquebus {
namespace {

// The PGNs a J1939 vehicle is read from.
constexpr std::uint32_t kPgnEec2 = 0x0'F003U; // engine load, SPN 92
constexpr std::uint32_t kPgnEt1 = 0x0'FEEEU; // engine coolant temperature, SPN 110
constexpr std::uint32_t kPgnCcvs1 = 0x0'FEF1U; // wheel-based vehicle speed, SPN 84
constexpr std::uint32_t kPgnAmb = 0x0'FEF5U; // ambient air temperature, SPN 171

// The built-in model was set against the 11-bit example vehicle, whose coolant idles at 70 degC and
// rises 0.12 degC per km/h. A heavy-duty diesel's thermostat holds 85 to 90 degC at standstill, and
// fed those numbers as they are the model reads every J1939 engine as 15 to 20 degC too hot and
// reports thermal stress - at idle, in a healthy engine, from the first second. So a J1939 coolant
// temperature is moved down by the difference between the two idles on its way in, which puts a
// healthy engine where the model's own healthy vehicle is and leaves every degree above that
// meaning what it meant when the model was set.
constexpr float kModelCoolantNominalDegC = 70.0F;
constexpr float kJ1939CoolantNominalDegC = 88.0F;
constexpr float kNominalDegCPerKmh = 0.12F;

// SPN 92 runs to 250 %, and above 100 the engine is being asked for more than it has at that speed.
constexpr float kEngineOverloadPercent = 100.0F;

// What the bus stress feature is made of on a J1939 bus. A monitored signal that says "error" or
// "not available" is an ECU telling the bus it cannot be believed, which is what the feature is
// for; each such frame adds this much, and every valid speed frame takes 15 % off, so a sensor that
// comes back stops counting within a second and one that does not keeps the bus at the ceiling.
constexpr float kIndicatorStress = 0.4F;
constexpr float kStressCeiling = 1.5F;
constexpr float kStressDecayPerSpeedFrame = 0.85F;

// The predictive trouble code. The model has to be sure for a while before an ECU says so on the
// bus, and has to stay sure of the opposite for longer before it takes it back: a lamp that comes
// and goes with every inference is worse than one that is late.
constexpr float kDtcAnomalyPercent = 80.0F;
constexpr float kDtcRecoveryPercent = 60.0F;
constexpr float kDtcCriticalHealthPercent = 30.0F;
constexpr std::uint64_t kDtcConfirmNs = 5'000'000'000ULL;
constexpr std::uint64_t kDtcRecoverNs = 10'000'000'000ULL;

// J1939-73: a DM1 repeats every second while it says anything, and is sent at once when it changes.
constexpr std::uint64_t kDm1PeriodNs = 1'000'000'000ULL;

/// SPN 110, engine coolant temperature: what the model is watching.
constexpr std::uint32_t kPredictiveSpn = 110U;

[[nodiscard]] constexpr std::uint64_t elapsedNs(std::uint64_t now, std::uint64_t since) noexcept
{
    // A replay that loops sends time backwards, and an unsigned difference of that is centuries.
    return now >= since ? now - since : 0U;
}

} // namespace

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
    m_telemetryMessage.format =
        (m_config.j1939Mode || m_config.outputCanId > kMaxStandardIdentifier)
            ? CanFrameFormat::Extended
            : CanFrameFormat::Standard;
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

    // An identifier that does not fit 11 bits is a J1939 bus whether or not anybody said so; the
    // catalog says so too, and this is for the node built by hand.
    if (m_config.outputCanId > kMaxStandardIdentifier) {
        m_config.j1939Mode = true;
    }
    setupTelemetryDefinitions();

    // The predictive DM1 goes out under the same address as the telemetry: the ECU is one ECU. The
    // identifier it has to ignore on the way in follows from it, and not from a constant that
    // happens to match the default.
    m_dm1Identifier =
        j1939Identifier(kPgnDm1, static_cast<std::uint8_t>(m_config.outputCanId & 0xFFU));

    m_ambientAllowanceDegC = 0.0F;
    m_j1939MessageCounter = 0;
    m_anomalySinceNs.reset();
    m_recoverySinceNs.reset();
    m_lastDm1Ns.reset();
    m_predictiveDtcActive = false;
    m_predictiveDtcRed = false;

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
        // Its own telemetry and its own trouble code come back to it off the bus, and a model that
        // took its own output for the vehicle would be listening to itself.
        if (frame.identifier == m_config.outputCanId
            || (m_config.j1939Mode && frame.identifier == m_dm1Identifier)) {
            continue;
        }

        ++m_framesAnalysed;

        const bool extended = frame.isExtended() || frame.identifier > kMaxStandardIdentifier;

        bool shouldInfer = false;
        if (extended || m_config.j1939Mode) {
            shouldInfer = observeJ1939(frame);
        }

        if (!shouldInfer) {
            shouldInfer = observeLegacy(frame);
        }

        if (!shouldInfer) {
            continue;
        }

        if (++m_strideCounter < m_config.inferenceStride) {
            continue;
        }
        m_strideCounter = 0;

        infer(frame);
    }

    if (!m_outgoingFrames.empty()) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoingFrames});
    }
    if (!m_outgoingSignals.empty()) {
        context.publish<DecodedSignal>(1, std::span<const DecodedSignal>{m_outgoingSignals});
    }
}

TinyMlFeatureVector TinyMlEcuNode::activeFeatures() const
{
    TinyMlFeatureVector features = m_lastFeatures;

    if (m_variables == nullptr || m_varInjectFault == SystemVariables::kUnknown) {
        return features;
    }

    // A fault somebody injected from the Dashboard: heat, a thermal delta, jitter and stress,
    // scaled.
    const double faultLevel = m_variables->value(m_varInjectFault);
    if (faultLevel > 0.05) {
        const auto intensity = static_cast<float>(std::clamp(faultLevel, 0.0, 2.0));
        features.engineTempDegC += 32.0F * intensity;
        features.thermalDeltaDegC += 28.0F * intensity;
        features.timingJitterRatio = std::max(features.timingJitterRatio, 0.85F * intensity);
        features.busStressFactor = std::max(features.busStressFactor, 0.95F * intensity);
    }

    return features;
}

void TinyMlEcuNode::infer(const CanFrame& cause)
{
    const auto startTime = std::chrono::steady_clock::now();
    TinyMlInferenceResult result{};
    if (m_model.evaluate(activeFeatures(), m_arena, m_config.anomalyThresholdPercent, result)
            .failed()) {
        return;
    }
    const auto elapsedTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - startTime)
                                 .count();
    const auto inferenceUs =
        static_cast<std::uint16_t>(std::clamp<std::int64_t>((elapsedTime + 999) / 1000, 1, 65535));

    m_lastResult = result;
    m_lastInferenceUs = inferenceUs;
    ++m_inferencesRun;
    if (result.isAnomaly) {
        ++m_anomaliesDetected;
    }

    if (m_outgoingFrames.size() < m_outgoingFrames.capacity()) {
        if (m_config.j1939Mode) {
            appendJ1939Telemetry(cause, result, inferenceUs);
        } else {
            appendLegacyTelemetry(cause, result, inferenceUs);
        }
    }

    appendDecodedSignals(cause.timestampNs, result, inferenceUs);
    publishToSystemVariables(result, inferenceUs);
}

void TinyMlEcuNode::appendLegacyTelemetry(const CanFrame& cause,
                                          const TinyMlInferenceResult& result,
                                          std::uint16_t inferenceUs)
{
    CanFrame telemetry{};
    telemetry.identifier = m_config.outputCanId;
    telemetry.channel = m_config.transmitChannel;
    telemetry.direction = CanDirection::Tx;
    telemetry.format = CanFrameFormat::Standard;
    telemetry.dlc = 8;
    telemetry.length = 8;
    telemetry.timestampNs = cause.timestampNs;

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

void TinyMlEcuNode::observeSpeed(float speedKmh, std::uint64_t timestampNs) noexcept
{
    if (m_hasPreviousSpeed && timestampNs > m_lastSpeedTimestampNs) {
        const float dtSec = static_cast<float>(timestampNs - m_lastSpeedTimestampNs) * 1.0e-9F;
        if (dtSec > 1.0e-4F && dtSec < 5.0F) {
            const float instantAccel = (speedKmh - m_lastFeatures.speedKmh) / dtSec;
            m_lastFeatures.accelerationKmhPerSec =
                0.6F * m_lastFeatures.accelerationKmhPerSec + 0.4F * instantAccel;

            const float jitter = std::fabs(dtSec - m_expectedSpeedIntervalSec)
                                 / std::max(m_expectedSpeedIntervalSec, 0.01F);
            m_lastFeatures.timingJitterRatio =
                std::clamp(0.7F * m_lastFeatures.timingJitterRatio + 0.3F * jitter, 0.0F, 2.0F);
            m_expectedSpeedIntervalSec =
                std::clamp(0.9F * m_expectedSpeedIntervalSec + 0.1F * dtSec, 0.005F, 1.0F);
        }
    } else {
        m_hasPreviousSpeed = true;
    }

    m_lastSpeedTimestampNs = timestampNs;
    m_lastFeatures.speedKmh = speedKmh;
    updateThermalDelta();
}

void TinyMlEcuNode::addStress(float amount) noexcept
{
    m_lastFeatures.busStressFactor =
        std::clamp(m_lastFeatures.busStressFactor + amount, 0.0F, kStressCeiling);
}

void TinyMlEcuNode::updateThermalDelta() noexcept
{
    const float expected = kModelCoolantNominalDegC + (kNominalDegCPerKmh * m_lastFeatures.speedKmh)
                           + m_ambientAllowanceDegC;
    m_lastFeatures.thermalDeltaDegC = m_lastFeatures.engineTempDegC - expected;
}

bool TinyMlEcuNode::observeLegacy(const CanFrame& frame) noexcept
{
    if (frame.identifier == m_config.speedCanId && frame.length >= 2) {
        const auto rawSpeed = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(frame.data[0])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.data[1]) << 8U));
        const float newSpeedKmh = static_cast<float>(rawSpeed) * 0.1F;

        observeSpeed(newSpeedKmh, frame.timestampNs);

        m_lastFeatures.busStressFactor *= 0.85F;
        if (frame.dlc != frame.length || newSpeedKmh > 220.0F) {
            m_lastFeatures.busStressFactor = std::min(1.5F, m_lastFeatures.busStressFactor + 0.75F);
        }
        return true;
    }

    if (frame.identifier == m_config.tempCanId && frame.length >= 1) {
        const float newTempDegC = static_cast<float>(frame.data[0]) - 40.0F;
        m_lastFeatures.engineTempDegC = newTempDegC;
        updateThermalDelta();

        if (newTempDegC > 105.0F || newTempDegC < -20.0F) {
            m_lastFeatures.busStressFactor =
                std::min(1.5F, m_lastFeatures.busStressFactor + (newTempDegC - 100.0F) * 0.04F);
        }
        return true;
    }

    if (m_config.speedCanId == 0U && frame.length >= 1) {
        m_lastFeatures.speedKmh = static_cast<float>(frame.data[0]);
        return true;
    }

    return false;
}

bool TinyMlEcuNode::observeJ1939(const CanFrame& frame) noexcept
{
    // The PGN says what the frame is whoever sent it, which is what lets one ECU watch a bench
    // where the engine answers from any address.
    const std::uint32_t pgn = j1939Decompose(frame.identifier).pgn();

    switch (pgn) {
    case kPgnCcvs1: { // wheel-based vehicle speed, SPN 84: bytes 2 and 3, 1/256 km/h
        if (frame.length < 3) {
            return false;
        }

        // Every valid speed frame takes some of the stress off, the way the 11-bit path does, and
        // that is what lets a bus that had a bad moment stop being one.
        m_lastFeatures.busStressFactor *= kStressDecayPerSpeedFrame;

        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(frame.data[1])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.data[2]) << 8U));

        // 0xFB00 and up is not a speed: the standard keeps it for "error" and "not available".
        if (raw > 0xFAFFU) {
            addStress(kIndicatorStress);
            return true;
        }

        const float speedKmh = static_cast<float>(raw) / 256.0F;
        observeSpeed(speedKmh, frame.timestampNs);

        if (speedKmh > 220.0F) {
            addStress(0.75F);
        }
        return true;
    }

    case kPgnEt1: { // engine coolant temperature, SPN 110: byte 1, 1 degC, offset -40
        if (frame.length < 1) {
            return false;
        }

        if (frame.data[0] > 0xFAU) {
            addStress(kIndicatorStress);
            return true;
        }

        const float coolantDegC = static_cast<float>(frame.data[0]) - 40.0F;

        m_lastFeatures.engineTempDegC =
            coolantDegC - (kJ1939CoolantNominalDegC - kModelCoolantNominalDegC);
        updateThermalDelta();

        // In the engine's own degrees: 105 is where a J1939 engine starts to be in trouble, and
        // what the model makes of that is its business.
        if (coolantDegC > 105.0F) {
            addStress((coolantDegC - 100.0F) * 0.04F);
        }
        return true;
    }

    case kPgnEec2: { // engine percent load, SPN 92: byte 3, 1 %
        if (frame.length < 3) {
            return false;
        }

        if (frame.data[2] > 0xFAU) {
            addStress(kIndicatorStress);
            return true;
        }

        // SPN 92 runs to 250 %: the load relative to what the engine can give at this speed, and a
        // value above 100 is it being asked for more than it has. That is out of profile. 85 % is a
        // diesel doing its job - pedal to the floor from a standstill is 92 % - and it used to
        // count, which made every hard acceleration an anomaly with 0 % thermal health and a red
        // stop lamp.
        const float load = static_cast<float>(frame.data[2]);
        if (load > kEngineOverloadPercent) {
            addStress((load - kEngineOverloadPercent) * 0.04F);
        }
        return true;
    }

    case kPgnAmb: { // ambient air temperature, SPN 171: bytes 4 and 5, 0.03125 degC, offset -273
        if (frame.length < 5) {
            return false;
        }

        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(frame.data[3])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.data[4]) << 8U));
        if (raw > 0xFAFFU) {
            return false;
        }

        // A hot day lets the coolant run hotter before it means anything; a cold one does not make
        // a thermostat-held engine hotter than it is, so nothing is taken off for it. Not a reason
        // to run the model on its own: it changes what normal is, not what the engine is doing.
        const float ambientDegC = (static_cast<float>(raw) * 0.03125F) - 273.0F;
        m_ambientAllowanceDegC = std::clamp(0.5F * (ambientDegC - 25.0F), 0.0F, 15.0F);
        updateThermalDelta();
        return false;
    }

    default:
        return false;
    }
}

void TinyMlEcuNode::appendJ1939Telemetry(const CanFrame& cause,
                                         const TinyMlInferenceResult& result,
                                         std::uint16_t inferenceUs)
{
    // Proprietary B, PGN 0xFF00 (0x18FF0080 from address 0x80 unless told otherwise). Bytes:
    //   1  thermal health, 0.4 %/bit        2  anomaly score, 0.4 %/bit
    //   3  confidence, 0.4 %/bit            4  bits 1-3 risk level, 4-5 model status, 6-8 regime
    //   5  message counter                  6  age of the input, 0.1 s/bit
    //   7-8  inference time, microseconds
    CanFrame telemetry{};
    telemetry.identifier = m_config.outputCanId;
    telemetry.channel = m_config.transmitChannel;
    telemetry.direction = CanDirection::Tx;
    telemetry.format = CanFrameFormat::Extended;
    telemetry.dlc = 8;
    telemetry.length = 8;
    telemetry.timestampNs = cause.timestampNs;

    const auto percentByte = [](float percent) {
        return static_cast<std::uint8_t>(std::clamp<long>(std::lround(percent / 0.4F), 0L, 250L));
    };

    // The same bands the cluster's risk ring is drawn in: green under about a third, amber to two
    // thirds, red above.
    std::uint8_t risk = 0;
    if (result.anomalyScorePercent >= 80.0F) {
        risk = 3;
    } else if (result.anomalyScorePercent >= 60.0F) {
        risk = 2;
    } else if (result.anomalyScorePercent >= 30.0F) {
        risk = 1;
    }

    constexpr std::uint8_t kModelRunning = 1;
    const auto regime = static_cast<std::uint8_t>(result.regime);

    // The regime travels beside the risk level and not instead of it. They are different questions
    // - what the vehicle is doing, and how worried the model is about it - and a cluster that took
    // the second for the first would say "engine idling" of a vehicle at 100 km/h whenever the
    // model was calm.
    telemetry.data[0] = percentByte(result.thermalHealthPercent);
    telemetry.data[1] = percentByte(result.anomalyScorePercent);
    telemetry.data[2] = percentByte(result.confidencePercent);
    telemetry.data[3] = static_cast<std::uint8_t>((risk & 0x07U) | ((kModelRunning & 0x03U) << 3U)
                                                  | ((regime & 0x07U) << 5U));

    // A byte counts 0 to 250 on a J1939 bus and a 16-bit field to 0xFAFF: above that is where the
    // standard keeps "error" and "not available", and a counter that walked into it would be read
    // as a sensor failing twice every cycle.
    telemetry.data[4] = m_j1939MessageCounter;
    m_j1939MessageCounter = static_cast<std::uint8_t>((m_j1939MessageCounter + 1U) % 251U);
    telemetry.data[5] = 1; // the frame that caused this is at most one 0.1 s tick old

    const std::uint16_t timeUs = std::min<std::uint16_t>(inferenceUs, 0xFAFFU);
    telemetry.data[6] = static_cast<std::uint8_t>(timeUs & 0xFFU);
    telemetry.data[7] = static_cast<std::uint8_t>((timeUs >> 8U) & 0xFFU);

    m_outgoingFrames.push_back(telemetry);

    updatePredictiveDtc(cause, result);
}

void TinyMlEcuNode::updatePredictiveDtc(const CanFrame& cause, const TinyMlInferenceResult& result)
{
    const bool wasActive = m_predictiveDtcActive;
    const bool red = result.thermalHealthPercent < kDtcCriticalHealthPercent;

    advancePredictiveDtc(cause.timestampNs, result);
    announcePredictiveDtc(cause, wasActive, red);
}

void TinyMlEcuNode::advancePredictiveDtc(std::uint64_t now, const TinyMlInferenceResult& result)
{
    // --- The predictive trouble code ---------------------------------------------------------
    //
    //   anomaly >= 80 % for 5 s, or thermal health under 30 % at once -> SPN 110, FMI 15 (amber),
    //   or FMI 16 with the red lamp as well when it is the health that is failing.
    //   Taken back when the anomaly has been under 60 % for 10 s.
    //
    // Between 60 and 80 % nothing is decided either way, and nothing that was started is carried
    // over a gap: five seconds means five seconds in a row.
    const bool red = result.thermalHealthPercent < kDtcCriticalHealthPercent;
    const bool failing = result.anomalyScorePercent >= kDtcAnomalyPercent || red;

    if (failing) {
        m_recoverySinceNs.reset();
        if (!m_anomalySinceNs.has_value()) {
            m_anomalySinceNs = now;
        }
        if (red || elapsedNs(now, *m_anomalySinceNs) >= kDtcConfirmNs) {
            m_predictiveDtcActive = true;
        }
        return;
    }

    m_anomalySinceNs.reset();

    if (!m_predictiveDtcActive) {
        return;
    }

    if (result.anomalyScorePercent >= kDtcRecoveryPercent) {
        m_recoverySinceNs.reset();
        return;
    }

    if (!m_recoverySinceNs.has_value()) {
        m_recoverySinceNs = now;
    }
    if (elapsedNs(now, *m_recoverySinceNs) >= kDtcRecoverNs) {
        m_predictiveDtcActive = false;
    }
}

void TinyMlEcuNode::announcePredictiveDtc(const CanFrame& cause, bool wasActive, bool red)
{
    // What is said about it, and when: at once when it starts, ends or changes colour, and then
    // once a second for as long as it is true. A DM1 at the rate of the inferences - thirty a
    // second - would be a bus-load problem of the model's own making and would bury the real ones.
    const std::uint64_t now = cause.timestampNs;

    const bool changed =
        m_predictiveDtcActive != wasActive || (m_predictiveDtcActive && red != m_predictiveDtcRed);
    const bool due = !m_lastDm1Ns.has_value() || elapsedNs(now, *m_lastDm1Ns) >= kDm1PeriodNs;

    if (m_predictiveDtcActive && (changed || due)) {
        appendDm1(cause, true, red, red ? 16U : 15U);
        m_predictiveDtcRed = red;
        m_lastDm1Ns = now;
    } else if (!m_predictiveDtcActive && wasActive) {
        // The one DM1 that says it is over. Without it a receiver learns that from silence, and
        // waits for the silence to be long enough to mean something.
        appendDm1(cause, false, false, 0U);
        m_predictiveDtcRed = false;
        m_lastDm1Ns.reset();
    }
}

void TinyMlEcuNode::appendDm1(const CanFrame& cause, bool active, bool red, std::uint8_t fmi)
{
    if (m_outgoingFrames.size() >= m_outgoingFrames.capacity()) {
        return;
    }

    CanFrame dm1{};
    dm1.identifier = m_dm1Identifier;
    dm1.channel = m_config.transmitChannel;
    dm1.direction = CanDirection::Tx;
    dm1.format = CanFrameFormat::Extended;
    dm1.dlc = 8;
    dm1.length = 8;
    dm1.timestampNs = cause.timestampNs;

    // J1939-73's "no active fault": every lamp off, a code of all zeroes, the rest not used.
    dm1.data[1] = 0xFF;
    dm1.data[6] = 0xFF;
    dm1.data[7] = 0xFF;

    if (active) {
        // Lamps: protect 1-2, amber warning 3-4, red stop 5-6, malfunction 7-8 - 01 is on.
        dm1.data[0] = red ? 0x14U : 0x04U;
        dm1.data[2] = static_cast<std::uint8_t>(kPredictiveSpn & 0xFFU);
        dm1.data[3] = static_cast<std::uint8_t>((kPredictiveSpn >> 8U) & 0xFFU);
        dm1.data[4] = static_cast<std::uint8_t>(((kPredictiveSpn >> 11U) & 0xE0U) | (fmi & 0x1FU));
        dm1.data[5] = 1; // occurrence count 1, conversion method 0 (version 4)
    }

    m_outgoingFrames.push_back(dm1);
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
