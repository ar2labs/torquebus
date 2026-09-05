// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The transmit list as a source in the pipeline graph.
//
// A source and not a special case inside the engine, which is the whole point
// of rule #11: everything that puts frames on a bus is a node, so a transmit
// list can be filtered, logged or fed to a script exactly like anything else.
// Wiring one to a `can.transmit` sink is what actually reaches the bus, and
// wiring it to a trace instead is a dry run - which is a useful thing to be
// able to draw rather than a mode somebody has to implement.
//
// The node does not own the list. The panel does, and edits it while this runs;
// TransmitList is the piece that makes that safe.

#pragma once

#include "core/pipeline/PipelineNode.h"
#include "core/transmit/TransmitList.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

class TransmitListNode final : public IPipelineNode {
public:
    /// Serves one application channel.
    ///
    /// The list must outlive the node, which the engine guarantees by
    /// recompiling the graph whenever the project changes.
    ///
    /// One node per channel, each collecting only the rows that name its own -
    /// which is what lets a single list feed every bus at once without a row
    /// going out twice or on the wrong one.
    explicit TransmitListNode(TransmitList& list,
                              std::uint8_t channel = 0,
                              std::string label = "Transmit list")
        : m_list{list}
        , m_channel{channel}
        , m_label{std::move(label)}
    {
    }

    [[nodiscard]] std::uint8_t channel() const noexcept { return m_channel; }

    [[nodiscard]] std::string_view typeName() const noexcept override
    {
        return "transmit.list";
    }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return {};
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// Starts the clock and clears the schedule.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    [[nodiscard]] std::uint64_t sentFrames() const noexcept { return m_sent; }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    TransmitList& m_list;
    std::uint8_t m_channel;
    std::string m_label;

    /// Reused every pass, so a pass allocates nothing (rule #12). The published
    /// span points into here, so it must survive until the next process().
    std::vector<CanFrame> m_outgoing;

    /// Measured from prepare(), like LuaEcuNode's clock, so a transmit time and
    /// a frame timestamp are two readings of the same thing.
    std::chrono::steady_clock::time_point m_started;

    std::uint64_t m_sent{0};
};

} // namespace torquebus
