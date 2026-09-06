// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What travels along an edge of the pipeline graph.
//
// Edges are typed, and the graph refuses a connection that does not typecheck.
// The alternative - one universal "CAN data" type that every node accepts and
// interprets by convention - moves the error from connection time to runtime,
// which for a visual tool is the difference between a diagram you can trust and
// a diagram you have to debug.
//
// A DBC decoder does not emit frames. It emits signals. The type system is what
// makes that statement enforceable instead of aspirational.

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/DecodedSignal.h"
#include "core/diagnostics/DiagnosticEvent.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace torquebus {

enum class PortType : std::uint8_t {
    /// Raw CAN / CAN FD frames. Produced by channels, replay and script nodes.
    Frames,

    /// Decoded signal values with timestamps. Produced by DBC / ARXML decoders.
    Signals,

    /// Reassembled J1939 / ISOBUS parameter groups.
    Pgns,

    /// Diagnostic events: UDS responses, DTCs, session and state changes.
    Events
};

[[nodiscard]] constexpr std::string_view toString(PortType type) noexcept
{
    switch (type) {
    case PortType::Frames:  return "Frames";
    case PortType::Signals: return "Signals";
    case PortType::Pgns:    return "Pgns";
    case PortType::Events:  return "Events";
    }
    return "Unknown";
}

/// Maps a C++ payload type onto its port type, so a node can ask for the right
/// span without repeating the enum at every call site.
template <typename T>
struct PortTraits;

template <>
struct PortTraits<CanFrame> {
    static constexpr PortType kType = PortType::Frames;
};

template <>
struct PortTraits<DecodedSignal> {
    static constexpr PortType kType = PortType::Signals;
};

template <>
struct PortTraits<DiagnosticEvent> {
    static constexpr PortType kType = PortType::Events;
};

// Pgns gets its specialisation when that payload lands. Declaring the enum
// values before the payloads existed - which is what v0.7 did - is what let
// Events be wired up here without changing anything the graph had already
// type-checked.

/// One batch travelling along one edge.
///
/// Non-owning by design: at bus speed, copying a batch per edge would cost more
/// than the work the nodes do. The producing node owns the storage and must
/// keep it alive until its next process() call - which is the reason nodes keep
/// reusable buffers as members rather than returning fresh containers.
class PortBatch final {
public:
    PortBatch() = default;

    template <typename T>
    explicit PortBatch(std::span<const T> items) noexcept
        : m_type{PortTraits<T>::kType}
        , m_data{items.data()}
        , m_count{items.size()}
    {
    }

    [[nodiscard]] PortType type() const noexcept { return m_type; }
    [[nodiscard]] std::size_t size() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

    /// Typed access. Returns an empty span when the batch holds something else,
    /// so a mis-wired graph starves a node rather than reinterpreting bytes -
    /// the graph should have rejected the connection long before this, and this
    /// is the second line of defence.
    template <typename T>
    [[nodiscard]] std::span<const T> as() const noexcept
    {
        if (m_type != PortTraits<T>::kType || m_data == nullptr) {
            return {};
        }
        return std::span<const T>{static_cast<const T*>(m_data), m_count};
    }

private:
    PortType m_type{PortType::Frames};
    const void* m_data{nullptr};
    std::size_t m_count{0};
};

/// Declares one input or output of a node: what it is called, and what flows
/// through it. Ports are fixed for the lifetime of a node - a node that wants a
/// variable number of inputs declares the maximum and ignores the empty ones.
struct PortDescriptor final {
    std::string_view name;
    PortType type{PortType::Frames};
};

} // namespace torquebus
