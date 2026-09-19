// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The graph itself: nodes, typed edges, and the compile step that turns them
// into something an executor can walk without thinking.
//
// Two properties are worth stating up front, because everything else follows
// from them:
//
//   1. Connections are typechecked when they are made, not when data flows.
//      Wiring a signal output into a frame input fails at connect() with a
//      message naming both ports - on the canvas, that is a wire the editor
//      refuses to draw.
//
//   2. The graph is compiled, not interpreted. compile() sorts the nodes
//      topologically once and allocates the edge slots; after that a pass is a
//      walk down a flat vector with no lookups, no virtual dispatch beyond the
//      node itself, and no allocation. That is what makes a visual pipeline
//      affordable at 150k frames/s (rule #12).
//
// A graph cannot be edited while it is running. Editing stops the measurement,
// recompiles and restarts. That is honest, cheap, and removes an entire class
// of concurrency bugs that would otherwise be permanent.

#pragma once

#include "core/Result.h"
#include "core/pipeline/PipelineNode.h"
#include "core/pipeline/PortType.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace torquebus {

/// One end of an edge: a node and one of its ports.
struct PortRef final {
    NodeId node{NodeId::Invalid};
    std::size_t port{0};

    [[nodiscard]] friend bool operator==(const PortRef&, const PortRef&) = default;
};

struct Edge final {
    PortRef from;
    PortRef to;
};

class PipelineGraph final {
public:
    /// Frames handed to a node in one pass. Nodes size their buffers to this at
    /// prepare() time, which is why it is fixed for the life of a compile.
    static constexpr std::size_t kDefaultMaximumBatchSize = 4096;

    PipelineGraph();
    ~PipelineGraph();

    PipelineGraph(const PipelineGraph&) = delete;
    PipelineGraph& operator=(const PipelineGraph&) = delete;
    PipelineGraph(PipelineGraph&&) = delete;
    PipelineGraph& operator=(PipelineGraph&&) = delete;

    // --- Building ---------------------------------------------------------

    /// Takes ownership. Returns the id used to connect it.
    NodeId addNode(std::unique_ptr<IPipelineNode> node);

    /// Removes a node and every edge touching it.
    void removeNode(NodeId id);

    /// Connects an output to an input.
    ///
    /// Fails, with a message naming both ends, when: either node is unknown,
    /// either port index is out of range, the types do not match, or the input
    /// is already connected. An input takes exactly one edge - fan-*in* would
    /// need a merge policy (interleave? concatenate? by timestamp?) and
    /// choosing one silently is worse than making the user place an explicit
    /// merge node. Fan-*out* is unrestricted.
    [[nodiscard]] Result connect(PortRef from, PortRef to);

    void disconnect(PortRef to);

    void clear();

    // --- Inspection -------------------------------------------------------

    [[nodiscard]] std::size_t nodeCount() const noexcept { return m_nodes.size(); }
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return m_edges; }

    /// Nullptr when the id is unknown.
    [[nodiscard]] IPipelineNode* node(NodeId id) noexcept;
    [[nodiscard]] const IPipelineNode* node(NodeId id) const noexcept;

    /// Typed access, for a caller that put the node there and knows what it is.
    ///
    /// The graph owns its nodes, so a caller who needs to read a node's own
    /// state afterwards - how many frames a sink collected, whether an ECU
    /// faulted - otherwise has to keep a raw pointer alongside the id and hope
    /// the two stay in step. Returns nullptr if the id is unknown or the node
    /// is not a T, so a wrong guess is a null check rather than a bad cast.
    template<typename T>
    [[nodiscard]] T* nodeAs(NodeId id) noexcept
    {
        return dynamic_cast<T*>(node(id));
    }

    template<typename T>
    [[nodiscard]] const T* nodeAs(NodeId id) const noexcept
    {
        return dynamic_cast<const T*>(node(id));
    }

    /// Every node id, in insertion order.
    [[nodiscard]] std::vector<NodeId> nodeIds() const;

    // --- Compiling and running --------------------------------------------

    /// Validates the graph, sorts it topologically and prepares every node.
    ///
    /// Fails on a cycle, naming a node on it. A dataflow graph with a cycle has
    /// no valid evaluation order, and detecting it here means the canvas can
    /// refuse the wire that would close the loop.
    [[nodiscard]] Result compile(std::size_t maximumBatchSize = kDefaultMaximumBatchSize);

    [[nodiscard]] bool isCompiled() const noexcept { return m_compiled; }

    /// The evaluation order produced by the last successful compile.
    /// Empty before compiling. Exposed for tests and for the canvas, which
    /// shows execution order when asked.
    [[nodiscard]] const std::vector<NodeId>& executionOrder() const noexcept
    {
        return m_executionOrder;
    }

    /// Runs one pass over every node, in compiled order.
    ///
    /// Source nodes are expected to have been given their data before this is
    /// called - they are the ones with no inputs, and they read from wherever
    /// they were pointed at construction (a channel queue, a file, a timer).
    void execute();

    /// Calls finish() on every node, in reverse execution order, so a consumer
    /// is always torn down before the thing feeding it.
    void finish();

private:
    struct NodeEntry final {
        NodeId id{NodeId::Invalid};

        /// Always valid. What every operation goes through.
        IPipelineNode* node{nullptr};

        /// Non-null only for nodes the graph owns; borrowed nodes leave this
        /// empty and are kept alive by whoever lent them.
        std::unique_ptr<IPipelineNode> owned;

        /// Slots to publish into, per output port.
        ///
        /// A list per port, not one slot per port: fan-out means one output can
        /// feed several inputs, and an earlier version stored a single index
        /// here. Each new edge silently overwrote the previous one, so only the
        /// last consumer connected to a port ever received anything - a graph
        /// that looked right on the canvas and starved every branch but one.
        std::vector<std::vector<std::size_t>> outputSlots;

        /// Index into m_slots for each input port, or kUnconnected.
        std::vector<std::size_t> inputSlots;

        /// Scratch views handed to the node during a pass, sized at compile
        /// time so that execute() allocates nothing.
        std::vector<PortBatch> inputViews;
        std::vector<PortBatch> outputViews;
    };

    static constexpr std::size_t kUnconnected = static_cast<std::size_t>(-1);

    [[nodiscard]] NodeEntry* find(NodeId id) noexcept;
    [[nodiscard]] const NodeEntry* find(NodeId id) const noexcept;

    [[nodiscard]] Result topologicalSort(std::vector<NodeId>& order) const;

    void invalidate() noexcept;

    std::vector<NodeEntry> m_nodes;
    std::vector<Edge> m_edges;

    std::uint32_t m_nextNodeId{1};

    // --- Compiled state ---------------------------------------------------

    bool m_compiled{false};

    /// Evaluation order as ids, for inspection and for the canvas.
    std::vector<NodeId> m_executionOrder;

    /// The same order as indices into m_nodes. This is what execute() walks:
    /// looking a node up by id on every pass would make the executor a lookup
    /// loop, which is precisely the interpretation cost compiling exists to
    /// remove. Valid only while compiled, and any edit invalidates it.
    std::vector<std::size_t> m_executionPlan;

    /// One slot per edge: where a producer writes and a consumer reads. Sized
    /// at compile time and never reallocated during a measurement, so the
    /// spans handed to nodes stay valid.
    std::vector<PortBatch> m_slots;
};

} // namespace torquebus
