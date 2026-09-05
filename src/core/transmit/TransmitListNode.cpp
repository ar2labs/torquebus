// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/transmit/TransmitListNode.h"

namespace torquebus {
namespace {

/// Headroom for one pass.
///
/// A pass collects one frame per due periodic entry plus whatever one-shots are
/// waiting, so the realistic worst case is the whole list coming due at once
/// plus a full one-shot queue. Reserved rather than capped: dropping a frame
/// the user explicitly asked to send would be the wrong failure, and this is a
/// reservation, not a limit - the vector will grow if a list is enormous, once,
/// and then never again.
constexpr std::size_t kReservedFramesPerPass = 512;

} // namespace

Result TransmitListNode::prepare(std::size_t)
{
    m_outgoing.clear();
    m_outgoing.reserve(kReservedFramesPerPass);

    m_started = std::chrono::steady_clock::now();
    m_sent = 0;

    // Every periodic row starts its period now. Without this a list that ran in
    // an earlier measurement would consider all of its rows overdue and fire
    // them together in the first pass.
    m_list.restartSchedule();

    return Result::ok();
}

void TransmitListNode::process(NodeContext& context)
{
    // Cleared at the start of the pass because the span published last pass had
    // to stay alive until now - the buffer contract from PortType.h.
    m_outgoing.clear();

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - m_started);

    m_list.collectDue(static_cast<std::uint64_t>(elapsed.count()), m_outgoing);

    if (m_outgoing.empty()) {
        return;
    }

    // The channel and direction are stamped here rather than stored on the
    // entry: which channel a list transmits on is a property of how the block
    // is wired, not of the frame somebody typed. The same list dropped onto a
    // second channel should work without editing every row.
    //
    // The timestamp is deliberately left alone. Whatever transmits it gives it
    // a bus time; inventing one here would put a number in the trace that never
    // happened.
    for (CanFrame& frame : m_outgoing) {
        frame.direction = CanDirection::Tx;
    }

    m_sent += m_outgoing.size();

    context.publish<CanFrame>(0, std::span<const CanFrame>{m_outgoing});
}

} // namespace torquebus
