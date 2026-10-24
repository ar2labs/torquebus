// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Several wires into one.
//
// An input of the graph takes exactly one wire, because merging two streams needs a policy and the
// graph does not pick one silently (GraphDescription.h). This is the policy, written down, for the
// case that comes first: two buses and one block that reads both. The J1939 block files every ECU
// under the bus its frames came from, so a block fed by a CAN Bus on each channel through this
// reports both buses - where before, two live buses reached one block only through the replay of a
// recording of both.
//
// What it does:
//
//   * Frames come out in the order of their timestamps. Those of one measurement share an epoch
//     (CanFrame.h), so the order is the order things happened on the buses. A tie goes to the lower
//     input, and a frame held over from an earlier pass goes before one that arrived in this.
//   * The order of one wire is never changed. The merge only ever takes the next frame of some
//     wire, so a wire whose timestamps are not in order - a recording written by something else -
//     comes out in the order it went in.
//   * A frame comes out as it went in: it keeps the channel it was received on, its direction and
//     its flags. That is what a block that reads several buses needs, and the reason there is no
//     "output channel" to set.
//   * A pass carries at most one batch, because nothing downstream is sized for more
//     (PipelineNode.h). What does not fit waits for the next pass, up to what one pass of every
//     wire at its fullest would leave after a batch has gone; beyond that the latest frames are
//     dropped. Both are counted, because a count is how a person finds out.
//
// What it does not do: a frame that arrives in a later pass than a frame it happened before is a
// pass late. The merge orders what it is given in one pass, and a pass is a few milliseconds of
// bus; it cannot order what has not arrived.
//
// What it trusts: that the timestamps of the channels are on one clock. The virtual backend's are.
// The Kvaser and PEAK backends stamp with their own - the adapter's ticks, the driver's hardware
// time when it has any, the machine's clock for what the tool transmits itself - and those have not
// been checked against each other, so across two different adapters the order *between* buses is
// only as good as their clocks. The order *within* a bus never depends on them.
//
// With frames on one wire and nothing waiting, the frames go on as they are - not copied - so a
// merge with one wire in use costs what a wire costs.

#pragma once

#include "core/Result.h"
#include "core/can/CanFrame.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace torquebus {

class FrameMergeNode final : public IPipelineNode {
public:
    /// The wires it takes. A node's ports are fixed for its lifetime, so this is the most; an input
    /// with no wire is empty every pass and costs nothing.
    static constexpr std::size_t kInputCount = 4U;

    /// The ports of the type, which the catalog describes with the same constants.
    static constexpr std::array<PortDescriptor, kInputCount> kInputPorts{
        PortDescriptor{"frames 1", PortType::Frames},
        PortDescriptor{"frames 2", PortType::Frames},
        PortDescriptor{"frames 3", PortType::Frames},
        PortDescriptor{"frames 4", PortType::Frames},
    };

    FrameMergeNode() = default;

    [[nodiscard]] std::string_view typeName() const noexcept override { return "can.merge"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputPorts;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputPorts;
    }

    /// Sizes what it holds, and forgets what an earlier measurement left: a second prepare is a
    /// measurement that starts again.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override;

    /// Frames that went on, in all.
    [[nodiscard]] std::uint64_t passedFrames() const noexcept { return m_passed; }

    /// Frames that did not fit in the pass they arrived in and went on in a later one.
    [[nodiscard]] std::uint64_t heldOverFrames() const noexcept { return m_heldOver; }

    /// Frames that fitted neither that pass nor the room to wait for the next: lost, and counted.
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept { return m_dropped; }

    /// How many frames wait now for the next pass.
    [[nodiscard]] std::size_t waitingFrames() const noexcept { return m_waiting; }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    /// What goes out this pass - at most a batch - and what waits, and the room it waits in. The
    /// two waiting buffers are swapped each pass, so that the frames that stay are written while
    /// the ones that were waiting are still being read.
    std::vector<CanFrame> m_out;
    std::vector<CanFrame> m_held;
    std::vector<CanFrame> m_next;
    std::size_t m_waiting{0};

    std::uint64_t m_passed{0};
    std::uint64_t m_heldOver{0};
    std::uint64_t m_dropped{0};
};

} // namespace torquebus
