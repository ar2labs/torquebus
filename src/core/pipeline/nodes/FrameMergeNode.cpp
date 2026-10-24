// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/nodes/FrameMergeNode.h"

#include <algorithm>

namespace torquebus {

Result FrameMergeNode::prepare(std::size_t maximumBatchSize)
{
    const std::size_t batch = std::max<std::size_t>(maximumBatchSize, 1U);

    // What can wait without anybody being dropped: what one pass of every wire at its fullest
    // leaves once a batch has gone. Sized here and never again - process() must not allocate.
    const std::size_t room = (kInputCount - 1U) * batch;

    m_out.assign(batch, CanFrame{});
    m_held.assign(room, CanFrame{});
    m_next.assign(room, CanFrame{});

    m_waiting = 0U;
    m_passed = 0U;
    m_heldOver = 0U;
    m_dropped = 0U;

    return Result::ok();
}

void FrameMergeNode::process(NodeContext& context)
{
    std::array<std::span<const CanFrame>, kInputCount> wires;
    std::size_t talking = 0U;
    std::size_t lastTalking = 0U;

    for (std::size_t wire = 0U; wire < kInputCount; ++wire) {
        wires[wire] = context.in<CanFrame>(wire);

        if (!wires[wire].empty()) {
            ++talking;
            lastTalking = wire;
        }
    }

    // Nothing waiting and at most one wire talking: there is nothing to put in order, and the
    // frames go on as they are - the same view, not a copy of it.
    if (m_waiting == 0U && talking <= 1U) {
        if (talking == 1U) {
            m_passed += wires[lastTalking].size();
            context.publish<CanFrame>(0, wires[lastTalking]);
        }

        return;
    }

    // The frames that waited are a lane of their own, ahead of the wires: they are older than
    // anything that arrived in this pass, and a tie goes to the lower lane.
    constexpr std::size_t kLanes = kInputCount + 1U;

    std::array<std::span<const CanFrame>, kLanes> lanes;
    lanes[0] = std::span<const CanFrame>{m_held.data(), m_waiting};
    std::copy(wires.begin(), wires.end(), lanes.begin() + 1);

    std::array<std::size_t, kLanes> heads{};
    std::size_t passed = 0U;
    std::size_t waiting = 0U;

    for (;;) {
        // The lane whose next frame is the earliest. Only the head of a lane is ever taken, so the
        // order within a lane is whatever it came in.
        std::size_t best = kLanes;

        for (std::size_t lane = 0U; lane < kLanes; ++lane) {
            if (heads[lane] == lanes[lane].size()) {
                continue;
            }

            if (best == kLanes
                || lanes[lane][heads[lane]].timestampNs < lanes[best][heads[best]].timestampNs) {
                best = lane;
            }
        }

        if (best == kLanes) {
            break;
        }

        const CanFrame& frame = lanes[best][heads[best]++];

        if (passed < m_out.size()) {
            m_out[passed++] = frame;
        } else if (waiting < m_next.size()) {
            m_next[waiting++] = frame;

            // Counted when it first has to wait, and not again for every pass it waits.
            if (best != 0U) {
                ++m_heldOver;
            }
        } else {
            ++m_dropped;
        }
    }

    m_held.swap(m_next);
    m_waiting = waiting;
    m_passed += passed;

    if (passed > 0U) {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_out.data(), passed});
    }
}

std::vector<NodeStatistic> FrameMergeNode::statistics() const
{
    return {{"Frames passed", m_passed},
            {"Frames held over", m_heldOver},
            {"Frames dropped", m_dropped}};
}

} // namespace torquebus
