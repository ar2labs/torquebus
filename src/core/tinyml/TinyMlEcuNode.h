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
//
// On a J1939 bus (`j1939Mode`) it is one ECU, and behaves like one: the first
// time it hears the bus in a measurement it claims the address it speaks from
// (Address Claimed, J1939-81), under a NAME that says what it is - an on-highway
// System Monitor - and only then sends its telemetry and its predictive DM1 from
// that address. That is what lets a panel name it, rather than show a number.
// It answers a Request for Address Claimed with its claim, and keeps the rules
// of a contest: another ECU claiming the same address with a lower NAME takes
// it, and one with a higher NAME is told again that the address is taken. What
// the loser does depends on what it is. By default this block cannot move: it
// says it cannot claim an address and is silent on the bus for the rest of the
// measurement. Told it is arbitrary-address capable, it moves instead - to the
// lowest address in 128 to 247 that nobody has been heard to claim - and goes on
// from there, with the same NAME.
//
// What it claims to be is its NAME, and the three things in it that are a choice
// are parameters: the function, the serial number and whether it can move. The
// manufacturer is not one - a simulated ECU is nobody's product.
//
// One thing of J1939-81 it leaves out, on purpose: the pseudo-random delay of up
// to 153 ms that an ECU waits before it answers a request or says it cannot
// claim. The delay is there to spread the answers of ECUs that power up at the
// same instant on a shared wire, and here the answers are queued by the engine
// and arbitrated by the bus model, in an order that does not vary. A delay would
// only make a measurement unrepeatable.

#pragma once

#include "core/can/CanFrame.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"
#include "core/database/DecodedSignal.h"
#include "core/j1939/J1939Name.h"
#include "core/pipeline/PipelineNode.h"
#include "core/tinyml/TinyMlModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    bool j1939Mode{false};

    /// What this ECU says it is, in the NAME it claims its address under (J1939-81, on-highway):
    /// 33 is a System Monitor, which is what a model that watches the vehicle is. The serial number
    /// is the address it speaks from unless told otherwise, so that two of these at two addresses
    /// are two NAMEs and not a contest.
    std::uint8_t j1939Function{33U};
    std::optional<std::uint32_t> j1939Identity;

    /// Whether it can take another address when it loses the one it speaks from. It is the top bit
    /// of the NAME, which makes an ECU that can move lose to every ECU that cannot - whoever can
    /// move, moves.
    bool j1939ArbitraryAddress{false};
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

    /// Reads one frame of a J1939 bus into the feature vector. True when it is one the model runs
    /// on: a speed, a coolant temperature or an engine load, valid or reporting that it is not.
    [[nodiscard]] bool observeJ1939(const CanFrame& frame) noexcept;

    /// One inference on the features as they are, and everything that comes of it: the telemetry
    /// frame, the decoded signals and the system variables. `cause` is the frame that made it due.
    void infer(const CanFrame& cause);

    /// The features the model is shown: the ones read off the bus, and a fault somebody injected.
    [[nodiscard]] TinyMlFeatureVector activeFeatures() const;

    /// Reads one frame of the 11-bit example vehicle (speedCanId, tempCanId) the same way.
    [[nodiscard]] bool observeLegacy(const CanFrame& frame) noexcept;

    /// A new speed, from either kind of bus: the acceleration, the timing of the frames it came
    /// in and what is normal for the coolant at that speed all follow from it.
    void observeSpeed(float speedKmh, std::uint64_t timestampNs) noexcept;

    /// Adds to the bus stress feature, which is bounded at both ends: it is a measure of how far
    /// out of profile the bus is, and a negative amount of that is not a thing.
    void addStress(float amount) noexcept;

    /// Recomputes how far the coolant is above what the vehicle's speed and the weather make
    /// normal. Called whenever any of the three moves.
    void updateThermalDelta() noexcept;

    /// The Address Claimed of a J1939 bus, appended to the output: who this ECU is, before it says
    /// anything else. `timestampNs` is the time of the frame that made it due - an ECU with no
    /// clock of its own takes the bus's.
    void appendAddressClaim(std::uint64_t timestampNs);

    /// An Address Claimed carrying this ECU's NAME from `source`: its own address, or the null
    /// address when it is saying that it cannot claim one. False when there was no room for it.
    bool appendClaimFrom(std::uint8_t source, std::uint64_t timestampNs);

    /// What this ECU is: on-highway, a function, a serial number - the address it was first meant
    /// to speak from, unless told otherwise, so that moving to another does not change who it is.
    [[nodiscard]] J1939Name ownName() const noexcept;

    /// Speaks from `address` from now on: the telemetry, the trouble code and the claim.
    void useAddress(std::uint8_t address) noexcept;

    /// The lowest address in 128 to 247 that no other ECU has been heard to claim in this
    /// measurement, other than the one it speaks from now. Nothing when they are all taken.
    [[nodiscard]] std::optional<std::uint8_t> freeAddress() const noexcept;

    /// A claim or a request seen on a J1939 bus, and what this ECU does about it: answers a request
    /// for the claims of the bus, defends its address against a higher NAME, and gives it up to a
    /// lower one - by moving, if it can, and by falling silent if it cannot. True when the frame
    /// was one of those, which the model does not read.
    bool onNetworkManagement(const CanFrame& frame);

    /// The telemetry frame of a J1939 bus (Proprietary B), and the predictive DM1 that follows it
    /// while the model thinks the engine is in trouble, appended to the output.
    void appendJ1939Telemetry(const CanFrame& cause,
                              const TinyMlInferenceResult& result,
                              std::uint16_t inferenceUs);

    /// The same, for the 11-bit example vehicle: RegimeClass, Confidence, AnomalyScore,
    /// ThermalHealth.
    void appendLegacyTelemetry(const CanFrame& cause,
                               const TinyMlInferenceResult& result,
                               std::uint16_t inferenceUs);

    /// Moves the predictive trouble code on - started, confirmed, taken back - and says so on the
    /// bus when there is something to say.
    void updatePredictiveDtc(const CanFrame& cause, const TinyMlInferenceResult& result);

    /// The state machine of it: whether the model has been sure for long enough to say so, and
    /// unsure for long enough to take it back.
    void advancePredictiveDtc(std::uint64_t now, const TinyMlInferenceResult& result);

    /// And what, if anything, goes on the bus because of it.
    void announcePredictiveDtc(const CanFrame& cause, bool wasActive, bool red);
    void appendDm1(const CanFrame& cause, bool active, bool red, std::uint8_t fmi);

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

    /// How much hotter than the vehicle's own curve the weather lets the coolant be. Zero until the
    /// bus reports an ambient temperature above 25 degC.
    float m_ambientAllowanceDegC{0.0F};

    /// J1939 telemetry and predictive DTC tracking.
    std::uint8_t m_j1939MessageCounter{0};
    std::uint32_t m_dm1Identifier{0};

    /// The address this ECU speaks from now, and the one it was meant to: the low byte of the
    /// telemetry identifier. They differ after it has moved.
    std::uint8_t m_address{0};
    std::uint8_t m_preferredAddress{0};

    /// The telemetry identifier from the address it speaks from now.
    std::uint32_t m_outputIdentifier{0};

    /// Its own Address Claimed, which comes back to it off the bus like its DM1 does, and whether
    /// it has been sent in this measurement. A measurement claims again: the ECU is switched on
    /// again.
    std::uint32_t m_claimIdentifier{0};
    bool m_addressClaimed{false};

    /// Another ECU took the address with a lower NAME, and this one could not move. Nothing more
    /// goes on the bus from it until the next measurement, but a request for the claims of the bus
    /// is answered with the news that it has no address.
    bool m_addressLost{false};

    /// The addresses other ECUs have been heard to claim in this measurement, which is what an ECU
    /// that has to move chooses from.
    std::array<bool, 256> m_claimedByOthers{};

    /// Bus time the engine has been (or has stopped being) in trouble since, and what was last said
    /// about it on the bus. Optionals because bus time zero is a time, and a sentinel that equals
    /// it would read a frame stamped zero as "not started".
    std::optional<std::uint64_t> m_anomalySinceNs;
    std::optional<std::uint64_t> m_recoverySinceNs;
    bool m_predictiveDtcActive{false};
    bool m_predictiveDtcRed{false};
    std::optional<std::uint64_t> m_lastDm1Ns;
};

} // namespace torquebus
