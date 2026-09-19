// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Terminal nodes for the Signals port.
//
// FrameSinkNode's counterpart. Same contract, same reason for the atomic: a
// panel that has gone away must stop receiving without the graph being
// recompiled underneath the thread that is walking it.

#pragma once

#include "core/database/DecodedSignal.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>

namespace torquebus {

/// Hands each batch of decoded signals to a callback.
///
/// The callback runs on the executor thread. The batch, and the definitions the
/// signals in it point at, are valid only for the duration of the call - a
/// consumer that wants to keep a value keeps the number, not the DecodedSignal.
class SignalSinkNode final : public IPipelineNode {
public:
    using Callback = std::function<void(std::span<const DecodedSignal>)>;

    explicit SignalSinkNode(Callback callback, std::string label = "signal sink")
        : m_callback{std::move(callback)}
        , m_label{std::move(label)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "signal.sink"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        if (!m_active.load(std::memory_order_relaxed)) {
            return;
        }

        const std::span<const DecodedSignal> incoming = context.in<DecodedSignal>(0);
        if (incoming.empty() || !m_callback) {
            return;
        }

        m_delivered += incoming.size();
        m_callback(incoming);
    }

    /// Stops delivery without touching the compiled graph. See
    /// FrameSinkNode::deactivate for why this is an atomic rather than a
    /// recompile.
    void deactivate() noexcept { m_active.store(false, std::memory_order_relaxed); }

    [[nodiscard]] bool isActive() const noexcept
    {
        return m_active.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t deliveredSignals() const noexcept { return m_delivered; }

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {{"Signals delivered", m_delivered}};
    }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"signals", PortType::Signals},
    };

    Callback m_callback;
    std::string m_label;
    std::uint64_t m_delivered{0};
    std::atomic<bool> m_active{true};
};

} // namespace torquebus
