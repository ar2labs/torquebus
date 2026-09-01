// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Software filtering, applied in the engine before frames reach any consumer.
//
// Two things this is deliberately NOT:
//
//   - It is not the hardware acceptance filter. Some adapters have one
//     (CanCapabilities::hardwareFilters); when they do, the engine may push a
//     compatible subset down to the driver as an optimisation. The software
//     filter stays authoritative, because otherwise the same project would
//     behave differently on a PCAN-USB and on a Kvaser.
//
//   - It is not the Trace panel's view filter. That one is applied at display
//     time and can be changed without losing data. This one decides what
//     enters the pipeline at all, which means a frame rejected here is never
//     logged either. Both exist on purpose.
//
// Evaluation allocates nothing and touches no shared state, so it runs on the
// engine thread for every single frame.

#pragma once

#include "core/can/CanFrame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace torquebus {

/// What a matching frame should cause.
enum class CanFilterAction : std::uint8_t {
    Accept, ///< Let the frame through (allow-list entry).
    Reject  ///< Drop the frame (block-list entry).
};

/// Which identifier formats a rule applies to.
enum class CanFormatMatch : std::uint8_t {
    Any,
    StandardOnly,
    ExtendedOnly
};

/// Which directions a rule applies to.
enum class CanDirectionMatch : std::uint8_t {
    Any,
    RxOnly,
    TxOnly
};

/// One filter rule.
///
/// A rule matches when *every* criterion it constrains matches. Criteria left
/// at their defaults constrain nothing, so a default-constructed CanFilter
/// matches every frame.
struct CanFilter final {
    /// Free-text label shown in the UI ("J1939 engine PGNs", "ignore 0x7DF").
    std::string name;

    CanFilterAction action{CanFilterAction::Accept};

    /// Inclusive identifier range. The default spans every 29-bit identifier.
    std::uint32_t identifierFrom{0};
    std::uint32_t identifierTo{kMaxExtendedIdentifier};

    /// Optional mask/value test, applied as (identifier & mask) == (value & mask).
    /// A zero mask disables it. This is what makes J1939 PGN filtering
    /// expressible without enumerating thousands of identifiers.
    std::uint32_t mask{0};
    std::uint32_t value{0};

    /// Application channel this rule applies to. Unset means every channel.
    bool channelConstrained{false};
    std::uint8_t channel{0};

    CanFormatMatch format{CanFormatMatch::Any};
    CanDirectionMatch direction{CanDirectionMatch::Any};

    /// When false, error frames never match this rule.
    bool matchErrorFrames{true};

    /// When false, remote-transmission-request frames never match.
    bool matchRemoteFrames{true};

    /// A disabled rule is kept in the project but ignored during evaluation,
    /// so the user can toggle it without losing its configuration.
    bool enabled{true};

    [[nodiscard]] constexpr bool matches(const CanFrame& frame) const noexcept
    {
        if (!enabled) {
            return false;
        }

        if (frame.error && !matchErrorFrames) {
            return false;
        }

        if (frame.rtr && !matchRemoteFrames) {
            return false;
        }

        if (channelConstrained && frame.channel != channel) {
            return false;
        }

        switch (format) {
        case CanFormatMatch::StandardOnly:
            if (frame.isExtended()) { return false; }
            break;
        case CanFormatMatch::ExtendedOnly:
            if (!frame.isExtended()) { return false; }
            break;
        case CanFormatMatch::Any:
            break;
        }

        switch (direction) {
        case CanDirectionMatch::RxOnly:
            if (!frame.isRx()) { return false; }
            break;
        case CanDirectionMatch::TxOnly:
            if (frame.isRx()) { return false; }
            break;
        case CanDirectionMatch::Any:
            break;
        }

        if (frame.identifier < identifierFrom || frame.identifier > identifierTo) {
            return false;
        }

        if (mask != 0 && (frame.identifier & mask) != (value & mask)) {
            return false;
        }

        return true;
    }

    // --- Convenience constructors for the common cases --------------------

    [[nodiscard]] static CanFilter acceptIdentifier(std::uint32_t identifier)
    {
        CanFilter filter;
        filter.identifierFrom = identifier;
        filter.identifierTo = identifier;
        return filter;
    }

    [[nodiscard]] static CanFilter acceptRange(std::uint32_t from, std::uint32_t to)
    {
        CanFilter filter;
        filter.identifierFrom = from;
        filter.identifierTo = to;
        return filter;
    }

    [[nodiscard]] static CanFilter rejectIdentifier(std::uint32_t identifier)
    {
        CanFilter filter = acceptIdentifier(identifier);
        filter.action = CanFilterAction::Reject;
        return filter;
    }

    [[nodiscard]] static CanFilter acceptMask(std::uint32_t maskBits, std::uint32_t valueBits)
    {
        CanFilter filter;
        filter.mask = maskBits;
        filter.value = valueBits;
        return filter;
    }

    [[nodiscard]] static CanFilter acceptChannel(std::uint8_t applicationChannel)
    {
        CanFilter filter;
        filter.channelConstrained = true;
        filter.channel = applicationChannel;
        return filter;
    }
};

/// An ordered set of rules, evaluated per frame.
///
/// Semantics, chosen to be the least surprising thing an engineer can be
/// handed:
///
///   - An empty set passes everything. A filter you have not configured is
///     not a filter that silently blocks your bus.
///   - Reject rules are evaluated first and win outright. "Everything except
///     0x7DF" is one rule, not a hand-built allow-list.
///   - If any Accept rule exists, the set becomes an allow-list: a frame must
///     match at least one of them.
///   - If only Reject rules exist, everything not rejected passes.
class CanFilterSet final {
public:
    CanFilterSet() = default;

    void add(CanFilter filter) { m_filters.push_back(std::move(filter)); }

    void clear() noexcept { m_filters.clear(); }

    [[nodiscard]] bool empty() const noexcept { return m_filters.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_filters.size(); }

    [[nodiscard]] const std::vector<CanFilter>& filters() const noexcept { return m_filters; }
    [[nodiscard]] std::vector<CanFilter>& filters() noexcept { return m_filters; }

    /// True when the frame should reach the pipeline.
    [[nodiscard]] bool accepts(const CanFrame& frame) const noexcept
    {
        if (m_filters.empty()) {
            return true;
        }

        bool hasAcceptRule = false;
        bool matchedAccept = false;

        for (const CanFilter& filter : m_filters) {
            if (!filter.enabled) {
                continue;
            }

            if (filter.action == CanFilterAction::Reject) {
                if (filter.matches(frame)) {
                    return false; // a reject wins outright
                }
                continue;
            }

            hasAcceptRule = true;
            if (!matchedAccept && filter.matches(frame)) {
                matchedAccept = true;
                // No early return: a later Reject rule must still be able to
                // veto this frame.
            }
        }

        return hasAcceptRule ? matchedAccept : true;
    }

    /// Compacts `frames` in place, keeping only accepted frames, and returns
    /// how many survived. Used by the engine to filter a whole batch without
    /// a second buffer.
    [[nodiscard]] std::size_t retainAccepted(std::span<CanFrame> frames) const noexcept
    {
        if (m_filters.empty()) {
            return frames.size();
        }

        std::size_t kept = 0;
        for (std::size_t index = 0; index < frames.size(); ++index) {
            if (accepts(frames[index])) {
                if (kept != index) {
                    frames[kept] = frames[index];
                }
                ++kept;
            }
        }
        return kept;
    }

private:
    std::vector<CanFilter> m_filters;
};

} // namespace torquebus
