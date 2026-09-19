// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/nodes/DbcDecoderNode.h"

#include <algorithm>
#include <format>

namespace torquebus {
namespace {

/// The most signals any one message in the database carries.
///
/// This, times the batch size, is the worst case for one pass: a batch made
/// entirely of that message.
[[nodiscard]] std::size_t widestMessage(const CanDatabase& database) noexcept
{
    std::size_t widest = 0;
    for (const CanMessage& message : database.messages()) {
        widest = std::max(widest, message.signalList.size());
    }
    return widest;
}

} // namespace

Result DbcDecoderNode::prepare(std::size_t maximumBatchSize)
{
    m_buffer.clear();

    if (!m_database || m_database->empty()) {
        // Not an error. A decoder with no database is what a freshly dropped
        // block on the canvas looks like, and refusing to compile the graph
        // would mean a project cannot be built up in any order but one.
        return Result::ok();
    }

    const std::size_t widest = widestMessage(*m_database);
    const std::size_t worstCase = maximumBatchSize * widest;

    // A database wide enough to make this allocation absurd is a database that
    // is wrong, not a bus that is fast. Say so at compile time, where there is
    // somebody to read it, rather than at the first pass.
    constexpr std::size_t kMaximumBufferedSignals = 8u * 1024u * 1024u;
    if (worstCase > kMaximumBufferedSignals) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("Decoder '{}' would need room for {} signals per pass "
                                         "({} frames times {} signals in the widest message), "
                                         "which is more than any real bus produces",
                                         m_label,
                                         worstCase,
                                         maximumBatchSize,
                                         widest));
    }

    m_buffer.resize(worstCase);

    // One message worth, which is all this ever holds at once.
    m_present.reserve(widest);

    return Result::ok();
}

void DbcDecoderNode::process(NodeContext& context)
{
    const std::span<const CanFrame> incoming = context.in<CanFrame>(0);
    if (incoming.empty() || !m_database || m_buffer.empty()) {
        return;
    }

    std::size_t count = 0;

    for (const CanFrame& frame : incoming) {
        // Error and remote frames carry no data to decode. A remote frame in
        // particular has a length but no payload, and decoding it would produce
        // a full set of plausible zeroes.
        if (frame.error || frame.rtr) {
            continue;
        }

        const CanMessage* message = m_database->find(frame);
        if (message == nullptr) {
            ++m_unknown;
            continue;
        }

        ++m_decoded;

        message->signalsIn(frame.data.data(), frame.length, m_present);

        for (const CanSignal* signal : m_present) {
            if (count >= m_buffer.size()) {
                // Unreachable: prepare() sized for the worst case. Kept because
                // "unreachable" is a claim about code somewhere else, and the
                // cost of being wrong here is writing past a vector.
                break;
            }

            const bool fits = signal->fitsIn(frame.length);

            DecodedSignal& decoded = m_buffer[count++];
            decoded.timestampNs = frame.timestampNs;
            decoded.message = message;
            decoded.signal = signal;
            decoded.raw = signal->rawValue(frame.data.data(), frame.length);
            decoded.value = static_cast<double>(decoded.raw) * signal->factor + signal->offset;
            decoded.identifier = frame.identifier;
            decoded.channel = frame.channel;
            decoded.truncated = !fits;

            if (!fits) {
                ++m_truncated;
            }
        }
    }

    if (count == 0) {
        return;
    }

    m_emitted += count;
    context.publish<DecodedSignal>(0, std::span<const DecodedSignal>{m_buffer.data(), count});
}

} // namespace torquebus
