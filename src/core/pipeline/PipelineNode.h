// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a block on the canvas is, underneath.
//
// A node is a batch-in / batch-out transform with declared, typed ports. That
// is the whole contract, and it is deliberately small: the canvas, the project
// file, the headless runner and the executor all deal with nodes through this
// interface and nothing else.
//
// The performance contract is part of the interface, not a footnote (rule #12):
//
//   * process() receives whole batches. A node never sees a single frame.
//   * process() must not allocate. Output buffers are node members, sized once
//     and reused - which is also why an emitted batch is a non-owning view: the
//     node's buffer is the storage, and it stays valid until the next call.
//   * process() must not block. It runs on the executor thread, and every other
//     node in the graph is waiting behind it.
//
// At 150k frames/s these are not style preferences. A single allocation per
// batch, at 200 batches/s, is survivable; a single allocation per frame is not.

#pragma once

#include "core/Result.h"
#include "core/pipeline/PortType.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// Identifies a node inside one graph. Stable across a compile, and persisted
/// in the project file, so it must not be a pointer or an index into a vector
/// that can be reordered.
enum class NodeId : std::uint32_t { Invalid = 0 };

/// What the executor hands a node for one pass.
///
/// Inputs are the batches its upstream neighbours emitted in this same pass;
/// outputs are slots the node writes into. Both are indexed by the position of
/// the port in the node's own declaration, which is why those declarations are
/// fixed for the node's lifetime.
class NodeContext final {
public:
    NodeContext(std::span<const PortBatch> inputs, std::span<PortBatch> outputs) noexcept
        : m_inputs{inputs}
        , m_outputs{outputs}
    {
    }

    [[nodiscard]] std::size_t inputCount() const noexcept { return m_inputs.size(); }
    [[nodiscard]] std::size_t outputCount() const noexcept { return m_outputs.size(); }

    /// The batch on one input port. Empty when nothing arrived this pass, which
    /// is the normal case for an idle bus and must not be treated as an error.
    [[nodiscard]] PortBatch input(std::size_t port) const noexcept
    {
        return port < m_inputs.size() ? m_inputs[port] : PortBatch{};
    }

    /// Convenience for the overwhelmingly common single-input node.
    template <typename T>
    [[nodiscard]] std::span<const T> in(std::size_t port = 0) const noexcept
    {
        return input(port).as<T>();
    }

    /// Publishes a batch on an output port. The span must remain valid until
    /// this node's next process() call - in practice, a member buffer.
    ///
    /// Named publish() and not emit(). `emit` is a Qt macro that expands to
    /// nothing, so `void emit(std::size_t port, ...)` becomes
    /// `void (std::size_t port, ...)` in any translation unit that has included
    /// a Qt header - and the error appears in this file while the cause is a
    /// macro defined somewhere else entirely.
    ///
    /// Rule #3 keeps Qt out of the core's *dependencies*; it cannot keep the
    /// core's headers from being included by Qt code, which is exactly what the
    /// UI does. So core headers avoid Qt's macro names: emit, signals, slots,
    /// foreach.
    template <typename T>
    void publish(std::size_t port, std::span<const T> items) noexcept
    {
        if (port < m_outputs.size()) {
            m_outputs[port] = PortBatch{items};
        }
    }

private:
    std::span<const PortBatch> m_inputs;
    std::span<PortBatch> m_outputs;
};

class IPipelineNode {
public:
    IPipelineNode() = default;
    virtual ~IPipelineNode() = default;

    IPipelineNode(const IPipelineNode&) = delete;
    IPipelineNode& operator=(const IPipelineNode&) = delete;
    IPipelineNode(IPipelineNode&&) = delete;
    IPipelineNode& operator=(IPipelineNode&&) = delete;

    /// Stable type identifier, e.g. "can.source", "dbc.decoder". Persisted in
    /// the project file and used by the canvas to build its palette, so it must
    /// not change once released.
    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;

    /// Name shown on the block. Editable by the user; not an identity.
    [[nodiscard]] virtual std::string displayName() const { return std::string{typeName()}; }

    [[nodiscard]] virtual std::span<const PortDescriptor> inputs() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PortDescriptor> outputs() const noexcept = 0;

    /// Called once when the graph compiles, before any measurement starts.
    ///
    /// This is where a node sizes its buffers, opens its file, compiles its Lua
    /// chunk or validates its configuration - everything that may fail, may
    /// allocate, or may take time. Returning a failure aborts the compile with
    /// that message, which is far better than discovering the problem on the
    /// first frame.
    [[nodiscard]] virtual Result prepare(std::size_t maximumBatchSize)
    {
        (void)maximumBatchSize;
        return Result::ok();
    }

    /// One pass. See the performance contract at the top of this file.
    virtual void process(NodeContext& context) = 0;

    /// Called once when the measurement stops, after the final pass. Where a
    /// logger closes its file and a script node runs its on_disable.
    virtual void finish() {}
};

} // namespace torquebus
