// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/simulation/RestBusNode.h"

#include <algorithm>
#include <format>

namespace torquebus {
namespace {

/// What a .dbc writes when it does not say who sends a message.
constexpr std::string_view kAnyNode = "Vector__XXX";

/// Upper bound on frames one pass may produce.
///
/// A rest bus of two hundred 10 ms messages produces two per pass at the
/// executor's 5 ms cadence. Reaching this means the graph stalled for a second
/// and every message came due at once - in which case sending the backlog in
/// one burst would be worse than dropping it, because a receiver would see two
/// hundred frames with the same timestamp and conclude the bus had gone mad.
constexpr std::size_t kMaximumFramesPerPass = 1024;

} // namespace

bool RestBusNode::simulates(const std::string& transmitter) const
{
    // Excluded first, and it wins: the question "is this ECU real?" has one
    // answer, and the safe one is yes. A node named in both lists is the one on
    // the bench, and simulating it would put two of it on the wire.
    if (std::find(m_excluded.begin(), m_excluded.end(), transmitter) != m_excluded.end()) {
        return false;
    }

    if (m_included.empty()) {
        return true;
    }

    return std::find(m_included.begin(), m_included.end(), transmitter) != m_included.end();
}

Result RestBusNode::prepare(std::size_t maximumBatchSize)
{
    (void)maximumBatchSize;

    m_jobs.clear();
    m_outgoing.clear();

    m_simulated = 0;
    m_skipped = 0;
    m_drivenCount = 0;
    m_sent = 0;

    if (m_database == nullptr) {
        return Result::error(ErrorCode::InvalidState,
                             "Rest Bus: no database. A rest bus without one has "
                             "nothing to say.");
    }

    const auto now = std::chrono::steady_clock::now();

    for (const CanMessage& message : m_database->messages()) {
        // A message nobody is declared to send is simulated when no include
        // list narrows things, and not otherwise: "Vector__XXX" means the
        // database did not say, and guessing that it belongs to a named node
        // would put it on the wire twice.
        const std::string& transmitter = message.transmitter;

        if (!simulates(transmitter.empty() ? std::string{kAnyNode} : transmitter)) {
            continue;
        }

        const std::uint32_t cycleMs =
            message.cycleTimeMs > 0 ? message.cycleTimeMs : m_defaultCycleMs;

        if (cycleMs == 0) {
            ++m_skipped;
            continue;
        }

        Job job;
        job.message = &message;
        job.interval = std::chrono::milliseconds{cycleMs};

        // Spread rather than aligned: two hundred messages all due at t=0 would
        // put the whole rest bus in one pass, every cycle, and a real network
        // does not start in lockstep. Staggered by index over the first cycle,
        // which is arbitrary but stable - a run repeats exactly.
        job.next = now + (job.interval * static_cast<int>(m_jobs.size() % 16)) / 16;

        job.frame = message.makeFrame();
        job.frame.channel = m_channel;
        job.frame.direction = CanDirection::Tx;

        m_jobs.push_back(std::move(job));
        ++m_simulated;
    }

    // --- The signals somebody drives ---------------------------------------
    for (const DrivenSignal& driven : m_driven) {
        const auto job =
            std::find_if(m_jobs.begin(), m_jobs.end(), [&driven](const Job& candidate) {
                return candidate.message->name == driven.message;
            });

        if (job == m_jobs.end()) {
            // Named rather than ignored: a signal on a message this block is
            // not sending is a control that does nothing, and finding that out
            // on a bench is expensive.
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Rest Bus: '{}' is not a message this block sends. Check the "
                            "name, the excluded nodes, and whether it has a cycle time.",
                            driven.message));
        }

        const CanSignal* signal = job->message->findSignal(driven.signal);

        if (signal == nullptr) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Rest Bus: '{}' has no signal '{}'", driven.message, driven.signal));
        }

        if (m_variables == nullptr) {
            return Result::error(ErrorCode::InvalidState,
                                 "Rest Bus: a signal is bound to a variable, and this "
                                 "graph has nowhere to keep one.");
        }

        const std::string name =
            driven.variable.empty() ? driven.message + "." + driven.signal : driven.variable;

        const SystemVariables::Handle handle = m_variables->resolve(name);

        if (handle == SystemVariables::kUnknown) {
            return Result::error(
                ErrorCode::InvalidState,
                std::format("Rest Bus: no room for the variable '{}' - the limit is {} "
                            "names.",
                            name,
                            SystemVariables::kMaximumVariables));
        }

        job->driven.push_back(Job::Driven{signal, handle});
        ++m_drivenCount;
    }

    m_outgoing.reserve(std::min(m_jobs.size(), kMaximumFramesPerPass));

    return Result::ok();
}

void RestBusNode::process(NodeContext& context)
{
    m_outgoing.clear();

    if (m_jobs.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    for (Job& job : m_jobs) {
        if (now < job.next) {
            continue;
        }

        // Advanced from the deadline rather than from now, so a cycle time does
        // not drift by however long the pass took; and caught up rather than
        // repeated when the executor was late, which is the rule every other
        // periodic thing here follows.
        job.next += job.interval;
        if (job.next < now) {
            job.next = now + job.interval;
        }

        if (m_outgoing.size() >= kMaximumFramesPerPass) {
            continue;
        }

        for (const Job::Driven& driven : job.driven) {
            // Saturation is ignored on purpose: encode() says when a value did
            // not fit, and a slider pushed past a signal's range is a fact
            // about the slider. Refusing to send the frame over it would take
            // the whole rest bus down because one control was mis-configured.
            static_cast<void>(driven.signal->encode(
                m_variables->value(driven.handle), job.frame.data.data(), job.frame.length));
        }

        m_outgoing.push_back(job.frame);
        ++m_sent;
    }

    if (!m_outgoing.empty()) {
        context.publish<CanFrame>(0, m_outgoing);
    }
}

} // namespace torquebus
