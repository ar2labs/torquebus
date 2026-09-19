// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The terminal node of a Signals edge: what a DBC Decoder can be wired to.
//
// TraceSinkNode's counterpart, one port along. A decoder produces signals and
// until now the only thing that could consume them was a callback the trace
// panel owned; a block on the canvas had nowhere to end. This is that end, and
// it is what makes "CAN Channel -> DBC Decoder -> Signal Plot" a graph somebody
// can draw rather than a sentence in a design document.
//
// It does no plotting. It writes into a SignalSeriesStore, which the Graph
// panel reads on its own timer - the same arrangement as the trace, and for the
// same reason (rule #5): the executor thread must never wait on a repaint.

#pragma once

#include "core/database/DecodedSignal.h"
#include "core/pipeline/PipelineNode.h"
#include "core/plot/SignalSeries.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace torquebus {

class SignalPlotNode final : public IPipelineNode {
public:
    /// The store must outlive the node, which the engine guarantees by
    /// recompiling the graph whenever the project changes - the same contract
    /// TraceSinkNode has with its TraceStore.
    explicit SignalPlotNode(SignalSeriesStore& store, std::string label = "Signal Plot")
        : m_store{store}
        , m_label{std::move(label)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "signal.plot"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        const std::span<const DecodedSignal> incoming = context.in<DecodedSignal>(0);
        if (incoming.empty()) {
            return;
        }

        m_recorded += incoming.size();
        m_store.append(incoming);
    }

    [[nodiscard]] std::uint64_t recordedSamples() const noexcept { return m_recorded; }

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Samples recorded", m_recorded},
            // The two numbers that explain a plot with fewer points than
            // expected, which is otherwise a mystery with two possible causes.
            {"Samples dropped as too short", m_store.truncated()},
            {"Samples aged out of the plot", m_store.discarded()},
        };
    }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"signals", PortType::Signals},
    };

    SignalSeriesStore& m_store;
    std::string m_label;
    std::uint64_t m_recorded{0};
};

} // namespace torquebus
