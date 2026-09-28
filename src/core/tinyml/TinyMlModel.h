// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Embedded neural network inference inside a fixed static tensor arena.
//
// An automotive ECU running TinyML (Cortex-M4/M7, AURIX TriCore, S32K) does not
// call `malloc` on the CAN receive interrupt or the 10 ms control task. Following
// the TensorFlow Lite for Microcontrollers (TFLite Micro) execution model, all
// intermediate activation tensors and scratch buffers are carved out of a
// single pre-allocated contiguous byte arena (`TinyMlArena`) at prepare time.
//
// On the hot path (`evaluate()`), the network reads normalized features, steps
// through its quantized (`int8`) or floating-point dense layers inside the
// arena, and returns the operating regime classification, anomaly score, and
// virtual sensor estimate with zero heap allocations.

#pragma once

#include "core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// Operating regime classified by the built-in powertrain & bus TinyML model.
enum class TinyMlRegime : std::uint8_t {
    Idle = 0,
    Cruise = 1,
    HighLoad = 2,
    ThermalStress = 3,
    Anomaly = 4,
};

/// Human-readable label for a classified operating regime.
[[nodiscard]] constexpr std::string_view toString(TinyMlRegime regime) noexcept
{
    switch (regime) {
    case TinyMlRegime::Idle:
        return "Idle";
    case TinyMlRegime::Cruise:
        return "Cruise";
    case TinyMlRegime::HighLoad:
        return "HighLoad";
    case TinyMlRegime::ThermalStress:
        return "ThermalStress";
    case TinyMlRegime::Anomaly:
        return "Anomaly";
    }
    return "Unknown";
}

/// Activation function applied at the end of a dense layer.
enum class TinyMlActivation : std::uint8_t {
    Linear = 0,
    ReLU = 1,
    LeakyReLU = 2,
    Sigmoid = 3,
    Tanh = 4,
    Softmax = 5,
};

/// Fixed-capacity contiguous memory arena for zero-allocation inference.
///
/// Mirrors TFLite Micro's `tensor_arena`: allocated once during `prepare()`,
/// reset in O(1) at the start of each inference pass by rewinding the bump
/// pointer, and queried via `highWaterMarkBytes()` so the Statistics panel and
/// Canvas HUD can report the exact SRAM footprint of the virtual ECU.
class TinyMlArena final {
public:
    static constexpr std::size_t kDefaultCapacityBytes = 4096;
    static constexpr std::size_t kAlignment = 16;

    explicit TinyMlArena(std::size_t capacityBytes = kDefaultCapacityBytes);

    /// Resizes the backing storage and resets usage counters. Only called
    /// during node preparation before the measurement starts.
    void configure(std::size_t capacityBytes);

    /// Rewinds the bump pointer for the next inference pass in O(1).
    void resetPass() noexcept { m_offset = 0; }

    /// Carves a 16-byte-aligned span of `count` floats out of the arena.
    /// Returns an empty span if the arena is exhausted.
    [[nodiscard]] std::span<float> allocateFloats(std::size_t count) noexcept;

    [[nodiscard]] std::size_t capacityBytes() const noexcept { return m_storage.size(); }
    [[nodiscard]] std::size_t currentPassBytes() const noexcept { return m_offset; }
    [[nodiscard]] std::size_t highWaterMarkBytes() const noexcept { return m_highWaterMark; }

private:
    std::vector<std::uint8_t> m_storage;
    std::size_t m_offset{0};
    std::size_t m_highWaterMark{0};
};

/// One dense layer ($y = \sigma(W x + b)$) with symmetric `int8` quantization.
///
/// Weights are stored as signed 8-bit integers (`int8_t`) accompanied by a
/// per-layer floating-point dequantization scale (`weightScale`), cutting
/// flash/ROM weight storage by 4x compared to `float32` while accumulating in
/// 32-bit float inside the tensor arena.
struct TinyMlDenseLayer final {
    std::string name;
    std::size_t inputSize{0};
    std::size_t outputSize{0};
    TinyMlActivation activation{TinyMlActivation::ReLU};

    /// Dequantization scale: real weight = quantizedWeight[i] * weightScale.
    float weightScale{1.0F / 64.0F};

    /// Row-major `outputSize * inputSize` quantized weights.
    std::vector<std::int8_t> quantizedWeights;

    /// Per-neuron bias in floating point (`outputSize` elements).
    std::vector<float> biases;
};

/// Input feature vector extracted from the CAN bus stream for inference.
struct TinyMlFeatureVector final {
    static constexpr std::size_t kFeatureCount = 6;

    /// 0: Vehicle speed in km/h.
    float speedKmh{0.0F};
    /// 1: Longitudinal acceleration in km/h/s.
    float accelerationKmhPerSec{0.0F};
    /// 2: Engine / powertrain temperature in degC.
    float engineTempDegC{70.0F};
    /// 3: Excess thermal rise relative to speed steady-state model in degC.
    float thermalDeltaDegC{0.0F};
    /// 4: Normalized inter-frame arrival jitter [0..1+].
    float timingJitterRatio{0.0F};
    /// 5: Out-of-profile signal/bus stress indicator [0..1+].
    float busStressFactor{0.0F};

    [[nodiscard]] std::array<float, kFeatureCount> toArray() const noexcept
    {
        return {speedKmh,
                accelerationKmhPerSec,
                engineTempDegC,
                thermalDeltaDegC,
                timingJitterRatio,
                busStressFactor};
    }
};

/// Output produced by a single TinyML inference pass.
struct TinyMlInferenceResult final {
    TinyMlRegime regime{TinyMlRegime::Idle};

    /// Probability of the winning regime class in percent [0.0 .. 100.0].
    float confidencePercent{100.0F};

    /// Class probabilities for all 5 regimes in [0.0 .. 1.0].
    std::array<float, 5> classProbabilities{1.0F, 0.0F, 0.0F, 0.0F, 0.0F};

    /// Continuous anomaly score in percent [0.0 .. 100.0].
    float anomalyScorePercent{0.0F};

    /// Virtual sensor estimate: predicted powertrain thermal health [0.0 .. 100.0].
    float thermalHealthPercent{100.0F};

    /// True when `anomalyScorePercent` exceeds the node's anomaly threshold or
    /// the winning regime is `TinyMlRegime::Anomaly`.
    bool isAnomaly{false};
};

/// Embedded multi-layer neural network model with `int8` quantized weights.
class TinyMlModel final {
public:
    static constexpr std::size_t kInputFeatures = TinyMlFeatureVector::kFeatureCount;
    static constexpr std::size_t kOutputDimensions =
        7; // 5 regime logits + anomaly + thermal health

    TinyMlModel() = default;

    /// Returns the pre-trained automotive powertrain & CAN bus anomaly model
    /// shipped with TorqueBus Studio.
    [[nodiscard]] static TinyMlModel builtinPowertrainModel();

    /// Parses a custom `.tbusml` text model definition (layers, int8 weights,
    /// scales, and normalization constants).
    [[nodiscard]] static Result parse(std::string_view text, TinyMlModel& out);

    /// Loads a `.tbusml` model file from disk.
    [[nodiscard]] static Result loadFromFile(const std::string& path, TinyMlModel& out);

    /// Minimum arena capacity in bytes required to execute this model without
    /// exhausting the bump allocator.
    [[nodiscard]] std::size_t requiredArenaBytes() const noexcept;

    /// Total number of quantized parameters (weights + biases) across all layers.
    [[nodiscard]] std::size_t parameterCount() const noexcept;

    /// Total flash/ROM footprint of the quantized weights + biases in bytes.
    [[nodiscard]] std::size_t weightBytes() const noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::span<const TinyMlDenseLayer> layers() const noexcept { return m_layers; }

    /// Runs a forward pass of the neural network inside `arena`.
    ///
    /// Allocates nothing on the heap. Returns `ErrorCode::InvalidState` only
    /// if `arena` was configured smaller than `requiredArenaBytes()`.
    [[nodiscard]] Result evaluate(const TinyMlFeatureVector& features,
                                  TinyMlArena& arena,
                                  float anomalyThresholdPercent,
                                  TinyMlInferenceResult& out) const noexcept;

private:
    std::string m_name{"automotive_powertrain_int8"};
    std::array<float, kInputFeatures> m_featureMean{45.0F, 0.0F, 80.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, kInputFeatures> m_featureScale{
        1.0F / 45.0F, 1.0F / 15.0F, 1.0F / 20.0F, 1.0F / 12.0F, 1.0F, 1.0F};
    std::vector<TinyMlDenseLayer> m_layers;
};

} // namespace torquebus
