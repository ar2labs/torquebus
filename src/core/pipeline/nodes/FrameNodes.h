// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The first three node types: a source, a transform and a sink.
//
// Together they are enough to build the implicit default graph
// (`[CAN 1] -> [Trace]`) and to prove the executor end to end. Everything that
// follows - DBC decoder, J1939, Lua ECU, logger, plot - is the same shape with
// different contents.
//
// Each of them is also a worked example of the performance contract in
// PipelineNode.h: buffers are members, sized in prepare(), reused every pass,
// and emitted as non-owning views.

#pragma once

#include "core/can/CanChannel.h"
#include "core/can/CanFilter.h"
#include "core/can/CanFrame.h"
#include "core/pipeline/PipelineNode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace torquebus {

/// Reads one application channel and puts its frames into the graph.
///
/// A source has no inputs: it pulls from the channel's queue in process(),
/// which is where the boundary between "the driver's thread filled a queue" and
/// "the executor thread runs the graph" actually sits.
class ChannelSourceNode final : public IPipelineNode {
public:
    /// The channel is owned by the engine, not by the node. A node outliving
    /// its channel would be a lifetime bug; the engine guarantees it does not,
    /// by recompiling the graph whenever channels change.
    explicit ChannelSourceNode(CanChannel& channel)
        : m_channel{channel}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.source"; }

    [[nodiscard]] std::string displayName() const override { return m_channel.displayName(); }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override
    {
        m_maximumBatchSize = maximumBatchSize;
        m_buffer.reserve(maximumBatchSize);
        return Result::ok();
    }

    void process(NodeContext& context) override
    {
        // drain() applies the channel's filter and updates its statistics, so
        // the numbers on the status bar stay the numbers of what entered the
        // graph - not of what the driver happened to hand us.
        const std::size_t count = m_channel.drain(m_buffer, m_maximumBatchSize);
        if (count == 0) {
            return;
        }

        context.publish<CanFrame>(0, std::span<const CanFrame>{m_buffer.data(), count});
    }

    [[nodiscard]] CanChannel& channel() noexcept { return m_channel; }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    CanChannel& m_channel;
    std::vector<CanFrame> m_buffer;
    // Overwritten by prepare() before the first pass; this value only matters
    // if someone runs a node outside a compiled graph.
    std::size_t m_maximumBatchSize{4096};
};

/// Passes through the frames that match a filter set.
///
/// Distinct from the per-channel filter on purpose. The channel filter decides
/// what enters the pipeline at all - a frame it rejects is never logged either.
/// This one is a node in the middle of the graph: it shapes one branch while
/// the others still see everything, which is what makes "trace everything, but
/// only plot these PGNs" expressible.
class FrameFilterNode final : public IPipelineNode {
public:
    FrameFilterNode() = default;

    explicit FrameFilterNode(CanFilterSet filters)
        : m_filters{std::move(filters)}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.filter"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    [[nodiscard]] CanFilterSet& filters() noexcept { return m_filters; }
    [[nodiscard]] const CanFilterSet& filters() const noexcept { return m_filters; }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override
    {
        m_buffer.resize(maximumBatchSize);
        return Result::ok();
    }

    void process(NodeContext& context) override
    {
        const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
        if (incoming.empty()) {
            return;
        }

        // An unconfigured filter forwards the input span untouched rather than
        // copying it. At bus speed the difference between "forward a view" and
        // "copy every frame" is the difference between a filter node costing
        // nothing and costing as much as the source.
        if (m_filters.empty()) {
            context.publish<CanFrame>(0, incoming);
            return;
        }

        const std::size_t count = std::min(incoming.size(), m_buffer.size());
        std::copy_n(incoming.begin(), count, m_buffer.begin());

        const std::size_t kept =
            m_filters.retainAccepted(std::span<CanFrame>{m_buffer.data(), count});

        m_rejected += count - kept;

        if (kept > 0) {
            context.publish<CanFrame>(0, std::span<const CanFrame>{m_buffer.data(), kept});
        }
    }

    [[nodiscard]] std::uint64_t rejectedFrames() const noexcept { return m_rejected; }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    CanFilterSet m_filters;
    std::vector<CanFrame> m_buffer;
    std::uint64_t m_rejected{0};
};

/// Terminal node: transmits what it receives on a channel.
///
/// The other half of ChannelSourceNode, and what closes the loop for a
/// simulated ECU: `[Lua ECU] -> [CAN 1 transmit]` puts the script's frames on
/// the bus. Without it a script emits into nothing, which is useful for a test
/// and useless for a simulation.
///
/// Failures are counted rather than logged per frame. A channel that has gone
/// bus-off would otherwise produce one log line per frame at bus speed, which
/// buries the one line that mattered - the statistics panel is where a rising
/// failure count belongs.
class ChannelSinkNode final : public IPipelineNode {
public:
    explicit ChannelSinkNode(CanChannel& channel)
        : m_channel{channel}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.transmit"; }

    [[nodiscard]] std::string displayName() const override
    {
        return m_channel.displayName() + " transmit";
    }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kTransmitInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            if (m_channel.transmit(frame).succeeded()) {
                ++m_transmitted;
            } else {
                ++m_failed;
            }
        }
    }

    [[nodiscard]] std::uint64_t transmittedFrames() const noexcept { return m_transmitted; }
    [[nodiscard]] std::uint64_t failedFrames() const noexcept { return m_failed; }

private:
    static constexpr std::array<PortDescriptor, 1> kTransmitInputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    CanChannel& m_channel;
    std::uint64_t m_transmitted{0};
    std::uint64_t m_failed{0};
};

/// Terminal node: hands each batch to a callback.
///
/// This is what a panel, a logger or a test harness attaches to the graph. The
/// callback runs on the executor thread and is bound by the same contract as
/// process(): do not block, do not throw, do not call back into the graph. A
/// consumer that needs another thread posts to it and returns.
class FrameSinkNode final : public IPipelineNode {
public:
    using Callback = std::function<void(std::span<const CanFrame>)>;

    explicit FrameSinkNode(Callback callback, std::string label = "sink")
        : m_callback{std::move(callback)}
        , m_label{std::move(label)}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.sink"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
        if (incoming.empty() || !m_callback) {
            return;
        }

        m_delivered += incoming.size();
        m_callback(incoming);
    }

    [[nodiscard]] std::uint64_t deliveredFrames() const noexcept { return m_delivered; }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    Callback m_callback;
    std::string m_label;
    std::uint64_t m_delivered{0};
};

} // namespace torquebus
