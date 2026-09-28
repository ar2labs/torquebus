// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/tinyml/TinyMlModel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace torquebus {
namespace {

[[nodiscard]] constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] float sigmoid(float x) noexcept
{
    const float clamped = std::clamp(x, -20.0F, 20.0F);
    return 1.0F / (1.0F + std::exp(-clamped));
}

void applyActivation(TinyMlActivation activation, std::span<float> values) noexcept
{
    switch (activation) {
    case TinyMlActivation::Linear:
        return;
    case TinyMlActivation::ReLU:
        for (float& v : values) {
            v = std::max(0.0F, v);
        }
        return;
    case TinyMlActivation::LeakyReLU:
        for (float& v : values) {
            v = v >= 0.0F ? v : 0.1F * v;
        }
        return;
    case TinyMlActivation::Sigmoid:
        for (float& v : values) {
            v = sigmoid(v);
        }
        return;
    case TinyMlActivation::Tanh:
        for (float& v : values) {
            v = std::tanh(std::clamp(v, -10.0F, 10.0F));
        }
        return;
    case TinyMlActivation::Softmax: {
        if (values.empty()) {
            return;
        }
        float maxVal = values[0];
        for (const float v : values) {
            maxVal = std::max(maxVal, v);
        }
        float sum = 0.0F;
        for (float& v : values) {
            v = std::exp(std::clamp(v - maxVal, -25.0F, 0.0F));
            sum += v;
        }
        if (sum > 0.0F) {
            const float inv = 1.0F / sum;
            for (float& v : values) {
                v *= inv;
            }
        }
        return;
    }
    }
}

[[nodiscard]] bool parseActivation(std::string_view token, TinyMlActivation& out) noexcept
{
    if (token == "linear") {
        out = TinyMlActivation::Linear;
        return true;
    }
    if (token == "relu") {
        out = TinyMlActivation::ReLU;
        return true;
    }
    if (token == "leaky_relu") {
        out = TinyMlActivation::LeakyReLU;
        return true;
    }
    if (token == "sigmoid") {
        out = TinyMlActivation::Sigmoid;
        return true;
    }
    if (token == "tanh") {
        out = TinyMlActivation::Tanh;
        return true;
    }
    if (token == "softmax") {
        out = TinyMlActivation::Softmax;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// TinyMlArena
// ---------------------------------------------------------------------------

TinyMlArena::TinyMlArena(std::size_t capacityBytes)
{
    configure(capacityBytes);
}

void TinyMlArena::configure(std::size_t capacityBytes)
{
    const std::size_t aligned = alignUp(std::max<std::size_t>(capacityBytes, 256U), kAlignment);
    m_storage.assign(aligned, 0U);
    m_offset = 0;
    m_highWaterMark = 0;
}

std::span<float> TinyMlArena::allocateFloats(std::size_t count) noexcept
{
    if (count == 0) {
        return {};
    }

    const std::size_t alignedOffset = alignUp(m_offset, kAlignment);
    const std::size_t bytesNeeded = count * sizeof(float);
    if (alignedOffset + bytesNeeded > m_storage.size()) {
        return {};
    }

    auto* ptr = reinterpret_cast<float*>(m_storage.data() + alignedOffset);
    m_offset = alignedOffset + bytesNeeded;
    m_highWaterMark = std::max(m_highWaterMark, m_offset);
    return std::span<float>{ptr, count};
}

// ---------------------------------------------------------------------------
// TinyMlModel
// ---------------------------------------------------------------------------

TinyMlModel TinyMlModel::builtinPowertrainModel()
{
    TinyMlModel model;
    model.m_name = "automotive_powertrain_int8_v1";

    // Input normalization constants:
    //   x0 = (speedKmh - 45.0) / 45.0           -> [-1.0 at 0 km/h, 0.0 at 45 km/h, +1.0 at 90
    //   km/h] x1 = (accelKmhPerS - 0.0) / 15.0        -> [0.0 steady, +1.0 at 15 km/h/s] x2 =
    //   (engineTempC - 80.0) / 20.0        -> [-0.5 at 70 C, 0.0 at 80 C, +1.0 at 100 C] x3 =
    //   (thermalDeltaC - 0.0) / 12.0       -> [0.0 normal, +1.0 at +12 C above expected] x4 =
    //   timingJitterRatio                  -> [0.0 periodic, >= 0.5 bus timing anomaly] x5 =
    //   busStressFactor                    -> [0.0 normal, >= 0.5 fault/stress]
    model.m_featureMean = {45.0F, 0.0F, 80.0F, 0.0F, 0.0F, 0.0F};
    model.m_featureScale = {1.0F / 45.0F, 1.0F / 15.0F, 1.0F / 20.0F, 1.0F / 12.0F, 1.0F, 1.0F};

    // Layer 1: 6 -> 10 (ReLU), symmetric int8 quantized with scale = 1/32 (0.03125).
    // Neurons extract interpretable latent features:
    //   h0: Low-speed / idle detector        (-x0)
    //   h1: Moderate cruise speed detector   (+x0 above idle)
    //   h2: High-speed load detector         (+x0 above 65 km/h)
    //   h3: High acceleration detector       (|x1| surge)
    //   h4: Elevated coolant temp detector   (+x2 above 92 C)
    //   h5: Excess thermal delta detector    (+x3 above expected curve)
    //   h6: CAN bus timing jitter detector   (+x4)
    //   h7: Bus / injected stress detector   (+x5)
    //   h8: Normal operating envelope guard  (-x3 - x4 - x5)
    //   h9: Combined thermal + load coupling (+x0 + x2 + x3)
    TinyMlDenseLayer layer1;
    layer1.name = "dense_feature_extractor_int8";
    layer1.inputSize = 6;
    layer1.outputSize = 10;
    layer1.activation = TinyMlActivation::ReLU;
    layer1.weightScale = 1.0F / 32.0F;
    layer1.quantizedWeights = {
        // x0,   x1,   x2,   x3,   x4,   x5
        -64, -8, 0,   0,   0,   0, // h0: Idle / stationary
        48,  0,  0,   0,   -8,  -8, // h1: Moving / cruise ramp
        64,  16, 8,   0,   0,   0, // h2: High speed / high load
        8,   64, 0,   0,   0,   0, // h3: Hard acceleration
        0,   0,  64,  24,  0,   0, // h4: High absolute coolant temperature
        0,   0,  24,  64,  0,   16, // h5: Thermal runaway / excess delta
        0,   0,  0,   0,   80,  24, // h6: Bus timing jitter
        0,   0,  0,   8,   24,  80, // h7: Bus/injected fault stress
        0,   0,  -16, -32, -48, -48, // h8: Healthy baseline margin
        8,   4,  48,  48,  16,  24, // h9: Coupled thermal-mechanical load
    };
    layer1.biases = {
        -1.15F, // h0 active when x0 < -0.58 (speed < ~19 km/h)
        0.75F, // h1 active when x0 > -0.50 (speed > ~22 km/h)
        -0.85F, // h2 active when x0 > +0.42 (speed > ~64 km/h)
        -0.45F, // h3 active on strong positive acceleration
        -0.95F, // h4 active when temp > ~99 C
        -0.75F, // h5 active when thermalDelta > ~9 C
        -0.15F, // h6 active when jitter > 0.06
        -0.20F, // h7 active when stress > 0.08
        1.20F, // h8 active in normal conditions, suppressed by stress/jitter/overheat
        -1.20F, // h9 active under combined high speed + high temperature
    };

    // Layer 2: 10 -> 8 (Tanh), symmetric int8 quantized with scale = 1/32.
    TinyMlDenseLayer layer2;
    layer2.name = "dense_regime_embedding_int8";
    layer2.inputSize = 10;
    layer2.outputSize = 8;
    layer2.activation = TinyMlActivation::Tanh;
    layer2.weightScale = 1.0F / 32.0F;
    layer2.quantizedWeights = {
        // h0,  h1,  h2,  h3,  h4,  h5,  h6,  h7,  h8,  h9
        64,  -32, -32, -16, -24, -24, -24, -24, 16,  -16, // e0: Idle affinity
        -48, 48,  -40, -16, -24, -24, -24, -24, 24,  -16, // e1: Cruise affinity
        -32, 16,  64,  48,  -24, -24, -24, -24, 16,  -8, // e2: HighLoad affinity
        -16, -16, -8,  -8,  64,  64,  -32, -32, -24, 32, // e3: ThermalStress affinity
        -16, -8,  -8,  -8,  -16, 8,   80,  88,  -32, 8, // e4: Anomaly affinity
        -12, -8,  -8,  -8,  0,   16,  72,  80,  -36, 12, // e5: Anomaly severity embedding
        16,  12,  -4,  -4,  -48, -56, -24, -40, 40,  -32, // e6: Thermal health embedding
        0,   0,   8,   8,   32,  32,  32,  32,  -16, 32, // e7: Total system stress
    };
    layer2.biases = {
        -0.10F,
        -0.15F,
        -0.35F,
        -0.45F,
        -0.55F,
        -0.30F,
        0.45F,
        -0.20F,
    };

    // Layer 3 (Output Head): 8 -> 7 (Linear; head post-processing applies
    // Softmax to [0..4] for regime probabilities and Sigmoid to [5] and [6]).
    TinyMlDenseLayer layer3;
    layer3.name = "dense_output_head_int8";
    layer3.inputSize = 8;
    layer3.outputSize = kOutputDimensions;
    layer3.activation = TinyMlActivation::Linear;
    layer3.weightScale = 1.0F / 16.0F;
    layer3.quantizedWeights = {
        // e0,  e1,  e2,  e3,  e4,  e5,  e6,  e7
        56,  -24, -24, -24, -32, -16, 12,  -12, // out0: Idle logit
        -24, 56,  -20, -24, -32, -16, 12,  -8, // out1: Cruise logit
        -24, -8,  60,  -16, -28, -12, 0,   16, // out2: HighLoad logit
        -20, -20, -12, 64,  -12, 20,  -24, 20, // out3: ThermalStress logit
        -24, -24, -20, 8,   68,  40,  -28, 24, // out4: Anomaly logit
        -16, -16, 4,   28,  48,  56,  -24, 28, // out5: Anomaly score logit
        16,  16,  -4,  -36, -32, -32, 56,  -24, // out6: Thermal health logit
    };
    layer3.biases = {
        0.35F, // Idle baseline prior
        0.45F, // Cruise baseline prior
        -0.20F, // HighLoad prior
        -0.40F, // ThermalStress prior
        -0.60F, // Anomaly prior
        -0.90F, // Anomaly score bias (low in nominal conditions)
        1.10F, // Thermal health bias (high in nominal conditions)
    };

    model.m_layers.push_back(std::move(layer1));
    model.m_layers.push_back(std::move(layer2));
    model.m_layers.push_back(std::move(layer3));
    return model;
}

std::size_t TinyMlModel::requiredArenaBytes() const noexcept
{
    std::size_t maxNeurons = kInputFeatures;
    for (const TinyMlDenseLayer& layer : m_layers) {
        maxNeurons = std::max(maxNeurons, layer.inputSize);
        maxNeurons = std::max(maxNeurons, layer.outputSize);
    }
    // Ping-pong activation buffers + normalized input + output scratch, each
    // aligned to TinyMlArena::kAlignment.
    const std::size_t perBuffer = alignUp(maxNeurons * sizeof(float), TinyMlArena::kAlignment);
    return std::max<std::size_t>(perBuffer * 4U, 512U);
}

std::size_t TinyMlModel::parameterCount() const noexcept
{
    std::size_t total = 0;
    for (const TinyMlDenseLayer& layer : m_layers) {
        total += layer.quantizedWeights.size() + layer.biases.size();
    }
    return total;
}

std::size_t TinyMlModel::weightBytes() const noexcept
{
    std::size_t total = 0;
    for (const TinyMlDenseLayer& layer : m_layers) {
        total += layer.quantizedWeights.size() * sizeof(std::int8_t);
        total += layer.biases.size() * sizeof(float);
        total += sizeof(float); // weightScale
    }
    return total;
}

Result TinyMlModel::evaluate(const TinyMlFeatureVector& features,
                             TinyMlArena& arena,
                             float anomalyThresholdPercent,
                             TinyMlInferenceResult& out) const noexcept
{
    if (m_layers.empty()) {
        return Result::error(ErrorCode::InvalidArgument, "TinyML model has no layers");
    }

    arena.resetPass();

    std::size_t maxWidth = kInputFeatures;
    for (const TinyMlDenseLayer& layer : m_layers) {
        maxWidth = std::max(maxWidth, layer.inputSize);
        maxWidth = std::max(maxWidth, layer.outputSize);
    }

    std::span<float> bufferA = arena.allocateFloats(maxWidth);
    std::span<float> bufferB = arena.allocateFloats(maxWidth);
    if (bufferA.empty() || bufferB.empty()) {
        return Result::error(ErrorCode::InvalidState,
                             "TinyML tensor arena exhausted during activation allocation");
    }

    const std::array<float, kInputFeatures> raw = features.toArray();
    for (std::size_t i = 0; i < kInputFeatures; ++i) {
        bufferA[i] = std::clamp((raw[i] - m_featureMean[i]) * m_featureScale[i], -6.0F, 6.0F);
    }

    std::span<float> currentIn = bufferA.first(kInputFeatures);
    bool useB = true;

    for (const TinyMlDenseLayer& layer : m_layers) {
        if (currentIn.size() != layer.inputSize) {
            return Result::error(ErrorCode::InvalidArgument,
                                 "TinyML layer input dimension mismatch");
        }

        std::span<float> currentOut = (useB ? bufferB : bufferA).first(layer.outputSize);

        const float scale = layer.weightScale;
        const std::int8_t* weights = layer.quantizedWeights.data();

        for (std::size_t row = 0; row < layer.outputSize; ++row) {
            float acc = 0.0F;
            const std::size_t rowOffset = row * layer.inputSize;
            for (std::size_t col = 0; col < layer.inputSize; ++col) {
                const float w = static_cast<float>(weights[rowOffset + col]) * scale;
                acc += w * currentIn[col];
            }
            currentOut[row] = acc + layer.biases[row];
        }

        applyActivation(layer.activation, currentOut);
        currentIn = currentOut;
        useB = !useB;
    }

    if (currentIn.size() < kOutputDimensions) {
        return Result::error(ErrorCode::InvalidArgument,
                             "TinyML output head produced fewer than 7 values");
    }

    // Copy the 5 regime logits into an arena scratch slice and apply Softmax.
    std::span<float> probs = arena.allocateFloats(5);
    if (probs.empty()) {
        return Result::error(ErrorCode::InvalidState,
                             "TinyML tensor arena exhausted during softmax head");
    }

    for (std::size_t i = 0; i < 5; ++i) {
        probs[i] = currentIn[i];
    }
    applyActivation(TinyMlActivation::Softmax, probs);

    std::size_t bestClass = 0;
    float bestProb = probs[0];
    for (std::size_t i = 0; i < 5; ++i) {
        out.classProbabilities[i] = probs[i];
        if (probs[i] > bestProb) {
            bestProb = probs[i];
            bestClass = i;
        }
    }

    out.regime = static_cast<TinyMlRegime>(bestClass);
    out.confidencePercent = std::clamp(bestProb * 100.0F, 0.0F, 100.0F);
    out.anomalyScorePercent = std::clamp(sigmoid(currentIn[5]) * 100.0F, 0.0F, 100.0F);
    out.thermalHealthPercent = std::clamp(sigmoid(currentIn[6]) * 100.0F, 0.0F, 100.0F);
    out.isAnomaly = (out.anomalyScorePercent >= anomalyThresholdPercent)
                    || (out.regime == TinyMlRegime::Anomaly);

    return Result::ok();
}

Result TinyMlModel::parse(std::string_view text, TinyMlModel& out)
{
    TinyMlModel parsed;
    parsed.m_layers.clear();

    std::istringstream stream{std::string{text}};
    std::string line;
    std::size_t lineNumber = 0;

    bool hasHeader = false;

    while (std::getline(stream, line)) {
        ++lineNumber;

        const std::size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line.erase(commentPos);
        }

        std::istringstream tokens{line};
        std::string keyword;
        if (!(tokens >> keyword)) {
            continue;
        }

        if (keyword == "TBUSML") {
            int version = 0;
            if (!(tokens >> version) || version != 1) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber)
                                         + ": expected 'TBUSML 1' header");
            }
            hasHeader = true;
        } else if (keyword == "model_name") {
            if (!(tokens >> parsed.m_name)) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber)
                                         + ": model_name requires a name");
            }
        } else if (keyword == "feature_mean") {
            for (float& v : parsed.m_featureMean) {
                if (!(tokens >> v)) {
                    return Result::error(ErrorCode::InvalidArgument,
                                         "Line " + std::to_string(lineNumber)
                                             + ": feature_mean requires 6 numbers");
                }
            }
        } else if (keyword == "feature_scale") {
            for (float& v : parsed.m_featureScale) {
                if (!(tokens >> v)) {
                    return Result::error(ErrorCode::InvalidArgument,
                                         "Line " + std::to_string(lineNumber)
                                             + ": feature_scale requires 6 numbers");
                }
            }
        } else if (keyword == "layer") {
            TinyMlDenseLayer layer;
            std::string actName;
            if (!(tokens >> layer.name >> layer.inputSize >> layer.outputSize >> actName
                  >> layer.weightScale)) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    "Line " + std::to_string(lineNumber)
                        + ": layer requires <name> <in> <out> <activation> <scale>");
            }
            if (layer.inputSize == 0 || layer.outputSize == 0) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber)
                                         + ": layer dimensions must be positive");
            }
            if (!parseActivation(actName, layer.activation)) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber) + ": unknown activation '"
                                         + actName + "'");
            }
            parsed.m_layers.push_back(std::move(layer));
        } else if (keyword == "weights") {
            if (parsed.m_layers.empty()) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber)
                                         + ": weights before any layer declaration");
            }
            TinyMlDenseLayer& layer = parsed.m_layers.back();
            const std::size_t expected = layer.inputSize * layer.outputSize;
            while (layer.quantizedWeights.size() < expected) {
                int w = 0;
                if (!(tokens >> w)) {
                    break;
                }
                if (w < -128 || w > 127) {
                    return Result::error(ErrorCode::InvalidArgument,
                                         "Line " + std::to_string(lineNumber)
                                             + ": int8 weight out of [-128, 127] range");
                }
                layer.quantizedWeights.push_back(static_cast<std::int8_t>(w));
            }
        } else if (keyword == "biases") {
            if (parsed.m_layers.empty()) {
                return Result::error(ErrorCode::InvalidArgument,
                                     "Line " + std::to_string(lineNumber)
                                         + ": biases before any layer declaration");
            }
            TinyMlDenseLayer& layer = parsed.m_layers.back();
            while (layer.biases.size() < layer.outputSize) {
                float b = 0.0F;
                if (!(tokens >> b)) {
                    break;
                }
                layer.biases.push_back(b);
            }
        } else {
            return Result::error(ErrorCode::InvalidArgument,
                                 "Line " + std::to_string(lineNumber) + ": unknown directive '"
                                     + keyword + "'");
        }
    }

    if (!hasHeader) {
        return Result::error(ErrorCode::InvalidArgument,
                             "TinyML model file is missing the 'TBUSML 1' header");
    }
    if (parsed.m_layers.empty()) {
        return Result::error(ErrorCode::InvalidArgument, "TinyML model defines no dense layers");
    }
    if (parsed.m_layers.front().inputSize != kInputFeatures) {
        return Result::error(ErrorCode::InvalidArgument,
                             "First TinyML layer must accept " + std::to_string(kInputFeatures)
                                 + " features");
    }
    if (parsed.m_layers.back().outputSize != kOutputDimensions) {
        return Result::error(ErrorCode::InvalidArgument,
                             "Final TinyML layer must output " + std::to_string(kOutputDimensions)
                                 + " values");
    }

    for (std::size_t i = 0; i < parsed.m_layers.size(); ++i) {
        const TinyMlDenseLayer& layer = parsed.m_layers[i];
        const std::size_t expectedWeights = layer.inputSize * layer.outputSize;
        if (layer.quantizedWeights.size() != expectedWeights) {
            return Result::error(ErrorCode::InvalidArgument,
                                 "Layer '" + layer.name + "' has "
                                     + std::to_string(layer.quantizedWeights.size())
                                     + " weights, expected " + std::to_string(expectedWeights));
        }
        if (layer.biases.size() != layer.outputSize) {
            return Result::error(ErrorCode::InvalidArgument,
                                 "Layer '" + layer.name + "' has "
                                     + std::to_string(layer.biases.size()) + " biases, expected "
                                     + std::to_string(layer.outputSize));
        }
        if (i > 0 && layer.inputSize != parsed.m_layers[i - 1].outputSize) {
            return Result::error(ErrorCode::InvalidArgument,
                                 "Layer '" + layer.name + "' input size ("
                                     + std::to_string(layer.inputSize)
                                     + ") does not match previous layer output size ("
                                     + std::to_string(parsed.m_layers[i - 1].outputSize) + ")");
        }
    }

    out = std::move(parsed);
    return Result::ok();
}

Result TinyMlModel::loadFromFile(const std::string& path, TinyMlModel& out)
{
    std::ifstream file{path};
    if (!file.is_open()) {
        return Result::error(ErrorCode::FileNotFound,
                             "Could not open TinyML model file '" + path + "'");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str(), out);
}

} // namespace torquebus
