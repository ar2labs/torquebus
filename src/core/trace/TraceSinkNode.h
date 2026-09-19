// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The node that puts frames into a TraceStore.
//
// It borrows the store rather than owning it, and that is the whole design
// decision here. Two consequences follow, and both are the point:
//
//   1. Several nodes can feed one store. A trace input takes one edge, like
//      every input, so a two-channel measurement needs two trace nodes - and
//      they write to the same store, in the executor's deterministic order.
//      The alternative was fan-in, which needs a merge policy the graph
//      deliberately refuses to invent.
//
//   2. The store outlives the graph. The graph is rebuilt on every start, and
//      a trace that emptied itself each time someone pressed Start would be
//      useless during a fault that takes several attempts to reproduce.
//
// Pausing is not here. "Stop adding to what I am reading" is a property of a
// view, not of the data path - freezing the model leaves the store filling, so
// resuming catches up instead of having lost the frames. Putting Pause on this
// node would have made it a data-loss button.

#pragma once

#include "core/pipeline/PipelineNode.h"
#include "core/trace/TraceStore.h"

#include <array>
#include <span>
#include <string>
#include <utility>

namespace torquebus {

class TraceSinkNode final : public IPipelineNode {
public:
    /// The store must outlive this node.
    explicit TraceSinkNode(TraceStore& store, std::string label = "CAN Trace")
        : m_store{store}
        , m_label{std::move(label)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.trace"; }
    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override { m_store.append(context.in<CanFrame>(0)); }

    [[nodiscard]] TraceStore& store() noexcept { return m_store; }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    TraceStore& m_store;
    std::string m_label;
};

} // namespace torquebus
