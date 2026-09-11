// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Remaining bus simulation: PLAN.md v0.15, and the reason a bench works.
//
// An ECU on a bench is surrounded by silence. It is waiting for the engine
// message, the ignition status, the vehicle speed - and without them it sits in
// a fault state, refuses to leave its default session, or simply does nothing
// while somebody wonders whether the wiring is wrong. The whole point of the
// feature CANoe calls *Restbussimulation* is to make that silence stop: every
// message the rest of the network would have sent, sent.
//
// The shape of it here is the one the workflow asks for:
//
//     exclude = "BodyController"
//
// - one block, one database, and a list of the nodes **not** to simulate. That
// is the question an engineer actually has ("everything except the one on my
// desk"), and building it the other way round - listing the forty nodes to
// simulate - is the same answer typed out forty times and re-typed whenever the
// database grows a node.
//
// ---------------------------------------------------------------------------
// What the signals carry
//
// Every signal holds its default - raw zero, which is `offset` in physical
// terms - unless it is named in `signals`, in which case it follows a **system
// variable**:
//
//     signals = "Engine.EngineSpeed, Engine.Throttle=throttle_pedal"
//
// That is the whole interactive story, and it is deliberately not a second one:
// a dashboard slider bound to `throttle_pedal` drives the rest bus, and so does
// a Lua script calling `var_set("throttle_pedal", 40)`. One mechanism, already
// built, already on screen - rather than a generator editor in this block that
// would do what the `tb` prelude and a dashboard already do between them.
//
// Only the named signals become variables. A 200-message database has more
// signals than SystemVariables has room for, and a table full of names nobody
// asked for would be a table nobody can find anything in.
//
// ---------------------------------------------------------------------------
// What it will not do
//
// **A message with no cycle time is not sent.** A database that declares no
// GenMsgCycleTime for a message is not saying "every 100 ms"; plenty of
// messages are event-triggered, and inventing a period for them would put
// traffic on the bus that the real network never carries - which is worse than
// missing traffic, because it looks right. Set `defaultCycleMs` to opt in, and
// the count of what was skipped is in the statistics either way.

#pragma once

#include "core/can/CanFrame.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

class RestBusNode final : public IPipelineNode {
public:
    /// One signal that follows a variable rather than holding its default.
    struct DrivenSignal final {
        std::string message;
        std::string signal;
        std::string variable;
    };

    RestBusNode() = default;

    [[nodiscard]] std::string_view typeName() const noexcept override
    {
        return "sim.restbus";
    }

    [[nodiscard]] std::string displayName() const override { return "Rest Bus"; }

    /// No input at all. It is a source: it sends what the missing half of the
    /// network would have sent, and what arrives on the bus does not change
    /// that. A rest bus that reacted to traffic would be an ECU, and there is a
    /// block for those.
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return {};
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// The database to simulate. Shared, not owned - the same one the decoder
    /// and the scripts read.
    void setDatabase(std::shared_ptr<const CanDatabase> database)
    {
        m_database = std::move(database);
    }

    /// Nodes whose messages are simulated. Empty means every node in the
    /// database that sends anything.
    void setSimulatedNodes(std::vector<std::string> nodes)
    {
        m_included = std::move(nodes);
    }

    /// Nodes whose messages are **not** simulated - the ones on the bench.
    /// Applied after the include list, so a node in both is excluded: the
    /// question "is this ECU real?" has one answer, and the safe one is yes.
    void setExcludedNodes(std::vector<std::string> nodes)
    {
        m_excluded = std::move(nodes);
    }

    /// Cycle time for messages whose database declares none. Zero - the
    /// default - skips them; see the header comment for why that is not timid.
    void setDefaultCycleMs(std::uint32_t milliseconds)
    {
        m_defaultCycleMs = milliseconds;
    }

    void setTransmitChannel(std::uint8_t channel) { m_channel = channel; }

    /// Signals that follow a variable. Names that are not in the database are
    /// reported by prepare() rather than ignored: a typo in a signal name is a
    /// control that does nothing, and finding that out on a bench is expensive.
    void setDrivenSignals(std::vector<DrivenSignal> signals)
    {
        m_driven = std::move(signals);
    }

    void setSystemVariables(SystemVariables* variables) { m_variables = variables; }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;
    void process(NodeContext& context) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Messages simulated", m_simulated},
            // Worth its own number: a database whose messages carry no cycle
            // time produces a rest bus that sends nothing, and the block would
            // otherwise look broken rather than under-specified.
            {"Messages without a cycle time", m_skipped},
            {"Signals following a variable", m_drivenCount},
            {"Frames sent", m_sent},
        };
    }

    /// How many messages this block will send. Zero after a prepare that found
    /// nothing to simulate, which the canvas reports rather than leaving to be
    /// discovered at the first quiet measurement.
    [[nodiscard]] std::size_t simulatedMessages() const noexcept { return m_jobs.size(); }

private:
    /// One message on its cycle, with the signals that are not at their default.
    struct Job final {
        const CanMessage* message{nullptr};

        std::chrono::nanoseconds interval{};
        std::chrono::steady_clock::time_point next{};

        /// The frame as it stands: built once at prepare with every signal at
        /// its default, then overwritten per cycle only where a variable drives
        /// one. A message with no driven signals is therefore memcpy-cheap.
        CanFrame frame{};

        struct Driven final {
            const CanSignal* signal{nullptr};
            SystemVariables::Handle handle{SystemVariables::kUnknown};
        };

        std::vector<Driven> driven;
    };

    /// True when `transmitter` is one this block stands in for.
    [[nodiscard]] bool simulates(const std::string& transmitter) const;

    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::shared_ptr<const CanDatabase> m_database;
    SystemVariables* m_variables{nullptr};

    std::vector<std::string> m_included;
    std::vector<std::string> m_excluded;
    std::vector<DrivenSignal> m_driven;

    std::uint32_t m_defaultCycleMs{0};
    std::uint8_t m_channel{0};

    std::vector<Job> m_jobs;
    std::vector<CanFrame> m_outgoing;

    std::uint64_t m_simulated{0};
    std::uint64_t m_skipped{0};
    std::uint64_t m_drivenCount{0};
    std::uint64_t m_sent{0};
};

} // namespace torquebus
