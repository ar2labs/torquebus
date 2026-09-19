// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/PipelineGraph.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

namespace torquebus {
namespace {

[[nodiscard]] std::string describe(const IPipelineNode& node, std::size_t port, bool isOutput)
{
    const std::span<const PortDescriptor> ports = isOutput ? node.outputs() : node.inputs();
    const std::string_view portName = port < ports.size() ? ports[port].name : "?";

    return std::format("{}.{}", node.typeName(), portName);
}

} // namespace

PipelineGraph::PipelineGraph() = default;
PipelineGraph::~PipelineGraph() = default;

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

NodeId PipelineGraph::addNode(std::unique_ptr<IPipelineNode> node)
{
    if (!node) {
        return NodeId::Invalid;
    }

    invalidate();

    NodeEntry entry;
    entry.id = static_cast<NodeId>(m_nextNodeId++);
    entry.node = node.get();
    entry.owned = std::move(node);

    const NodeId id = entry.id;
    m_nodes.push_back(std::move(entry));
    return id;
}

void PipelineGraph::removeNode(NodeId id)
{
    invalidate();

    std::erase_if(m_edges,
                  [id](const Edge& edge) { return edge.from.node == id || edge.to.node == id; });

    std::erase_if(m_nodes, [id](const NodeEntry& entry) { return entry.id == id; });
}

Result PipelineGraph::connect(PortRef from, PortRef to)
{
    NodeEntry* source = find(from.node);
    NodeEntry* target = find(to.node);

    if (source == nullptr || target == nullptr) {
        return Result::error(ErrorCode::InvalidArgument,
                             "Cannot connect: one of the nodes is not in this graph");
    }

    const std::span<const PortDescriptor> outputs = source->node->outputs();
    const std::span<const PortDescriptor> inputs = target->node->inputs();

    if (from.port >= outputs.size()) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("{} has no output port {}", source->node->typeName(), from.port));
    }

    if (to.port >= inputs.size()) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("{} has no input port {}", target->node->typeName(), to.port));
    }

    // The typecheck. This is the whole reason ports carry a type: the mistake
    // is caught while drawing the wire, with both ends named, instead of
    // becoming an empty panel three layers downstream.
    if (outputs[from.port].type != inputs[to.port].type) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Cannot connect {} ({}) to {} ({}): incompatible port types",
                        describe(*source->node, from.port, true),
                        toString(outputs[from.port].type),
                        describe(*target->node, to.port, false),
                        toString(inputs[to.port].type)));
    }

    const bool alreadyConnected =
        std::ranges::any_of(m_edges, [&to](const Edge& edge) { return edge.to == to; });

    if (alreadyConnected) {
        return Result::error(
            ErrorCode::InvalidState,
            std::format("{} already has an incoming connection. An input takes exactly one "
                        "edge - to combine several sources, insert a merge node rather than "
                        "letting the graph pick an order for you.",
                        describe(*target->node, to.port, false)));
    }

    invalidate();
    m_edges.push_back(Edge{from, to});
    return Result::ok();
}

void PipelineGraph::disconnect(PortRef to)
{
    invalidate();
    std::erase_if(m_edges, [&to](const Edge& edge) { return edge.to == to; });
}

void PipelineGraph::clear()
{
    invalidate();
    m_edges.clear();
    m_nodes.clear();
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

PipelineGraph::NodeEntry* PipelineGraph::find(NodeId id) noexcept
{
    const auto entry = std::ranges::find_if(
        m_nodes, [id](const NodeEntry& candidate) { return candidate.id == id; });
    return entry != m_nodes.end() ? &*entry : nullptr;
}

const PipelineGraph::NodeEntry* PipelineGraph::find(NodeId id) const noexcept
{
    const auto entry = std::ranges::find_if(
        m_nodes, [id](const NodeEntry& candidate) { return candidate.id == id; });
    return entry != m_nodes.end() ? &*entry : nullptr;
}

IPipelineNode* PipelineGraph::node(NodeId id) noexcept
{
    NodeEntry* entry = find(id);
    return entry != nullptr ? entry->node : nullptr;
}

const IPipelineNode* PipelineGraph::node(NodeId id) const noexcept
{
    const NodeEntry* entry = find(id);
    return entry != nullptr ? entry->node : nullptr;
}

std::vector<NodeId> PipelineGraph::nodeIds() const
{
    std::vector<NodeId> ids;
    ids.reserve(m_nodes.size());
    for (const NodeEntry& entry : m_nodes) {
        ids.push_back(entry.id);
    }
    return ids;
}

void PipelineGraph::invalidate() noexcept
{
    m_compiled = false;
    m_executionOrder.clear();
    m_executionPlan.clear();
    m_slots.clear();
}

// ---------------------------------------------------------------------------
// Compiling
// ---------------------------------------------------------------------------

Result PipelineGraph::topologicalSort(std::vector<NodeId>& order) const
{
    // Kahn's algorithm. Chosen over a depth-first sort because what is left
    // over when it stalls is exactly the set of nodes on a cycle, which is what
    // the error message needs to be useful.
    std::unordered_map<NodeId, std::size_t> incoming;
    incoming.reserve(m_nodes.size());

    for (const NodeEntry& entry : m_nodes) {
        incoming.emplace(entry.id, 0);
    }

    for (const Edge& edge : m_edges) {
        ++incoming[edge.to.node];
    }

    std::vector<NodeId> ready;
    ready.reserve(m_nodes.size());

    // Seeded in insertion order rather than in map order, so a graph with
    // several valid orders compiles to the same one every time. A trace whose
    // node order changed between runs would be a nightmare to debug.
    for (const NodeEntry& entry : m_nodes) {
        if (incoming[entry.id] == 0) {
            ready.push_back(entry.id);
        }
    }

    order.clear();
    order.reserve(m_nodes.size());

    while (!ready.empty()) {
        const NodeId current = ready.front();
        ready.erase(ready.begin());
        order.push_back(current);

        for (const Edge& edge : m_edges) {
            if (edge.from.node != current) {
                continue;
            }
            if (--incoming[edge.to.node] == 0) {
                ready.push_back(edge.to.node);
            }
        }
    }

    if (order.size() != m_nodes.size()) {
        // Whatever still has incoming edges is on, or downstream of, a cycle.
        for (const NodeEntry& entry : m_nodes) {
            if (incoming[entry.id] > 0) {
                return Result::error(
                    ErrorCode::InvalidState,
                    std::format("The graph contains a cycle, through '{}'. A dataflow graph "
                                "has no valid evaluation order once it loops.",
                                entry.node->typeName()));
            }
        }
        return Result::error(ErrorCode::InvalidState, "The graph contains a cycle");
    }

    return Result::ok();
}

Result PipelineGraph::compile(std::size_t maximumBatchSize)
{
    invalidate();

    if (m_nodes.empty()) {
        return Result::error(ErrorCode::InvalidState, "The graph has no nodes");
    }

    if (Result result = topologicalSort(m_executionOrder); result.failed()) {
        m_executionOrder.clear();
        return result;
    }

    // One slot per edge. Sized once and never reallocated during a measurement,
    // because the spans handed to nodes point into it.
    m_slots.assign(m_edges.size(), PortBatch{});

    for (NodeEntry& entry : m_nodes) {
        const std::size_t inputCount = entry.node->inputs().size();
        const std::size_t outputCount = entry.node->outputs().size();

        entry.inputSlots.assign(inputCount, kUnconnected);
        entry.outputSlots.assign(outputCount, {});
        entry.inputViews.assign(inputCount, PortBatch{});
        entry.outputViews.assign(outputCount, PortBatch{});
    }

    for (std::size_t index = 0; index < m_edges.size(); ++index) {
        const Edge& edge = m_edges[index];

        NodeEntry* source = find(edge.from.node);
        NodeEntry* target = find(edge.to.node);
        if (source == nullptr || target == nullptr) {
            return Result::error(ErrorCode::InvalidState, "Dangling edge in the graph");
        }

        // push_back, not assignment: an output port can feed many inputs.
        source->outputSlots[edge.from.port].push_back(index);
        target->inputSlots[edge.to.port] = index;
    }

    for (NodeEntry& entry : m_nodes) {
        if (Result result = entry.node->prepare(maximumBatchSize); result.failed()) {
            invalidate();
            return Result::error(result.code(),
                                 std::format("{} failed to prepare: {}",
                                             entry.node->typeName(),
                                             std::string{result.message()}));
        }
    }

    // Freeze the order as indices, so a pass is a walk down a flat vector.
    m_executionPlan.clear();
    m_executionPlan.reserve(m_executionOrder.size());
    for (const NodeId id : m_executionOrder) {
        const auto position =
            std::ranges::find_if(m_nodes, [id](const NodeEntry& entry) { return entry.id == id; });
        m_executionPlan.push_back(
            static_cast<std::size_t>(std::distance(m_nodes.begin(), position)));
    }

    m_compiled = true;
    return Result::ok();
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void PipelineGraph::execute()
{
    if (!m_compiled) {
        return;
    }

    for (const std::size_t index : m_executionPlan) {
        NodeEntry* entry = &m_nodes[index];

        // Gather this node's inputs from the slots its upstream neighbours
        // wrote earlier in this same pass - which is exactly what the
        // topological order guarantees has already happened.
        for (std::size_t port = 0; port < entry->inputSlots.size(); ++port) {
            const std::size_t slot = entry->inputSlots[port];
            entry->inputViews[port] = slot == kUnconnected ? PortBatch{} : m_slots[slot];
        }

        // Outputs start empty each pass, so a node that emits nothing this time
        // does not leave the previous batch visible to its consumers.
        std::ranges::fill(entry->outputViews, PortBatch{});

        NodeContext context{entry->inputViews, entry->outputViews};
        entry->node->process(context);

        // Publish to every consumer of each output port. They all receive the
        // same non-owning view of the producer's buffer - fan-out costs one
        // pointer copy per edge, not one frame copy per edge.
        for (std::size_t port = 0; port < entry->outputSlots.size(); ++port) {
            for (const std::size_t slot : entry->outputSlots[port]) {
                m_slots[slot] = entry->outputViews[port];
            }
        }
    }
}

void PipelineGraph::finish()
{
    // Reverse order: a consumer is torn down before the thing feeding it, so a
    // logger flushes before the source that was still producing into it goes
    // away.
    for (auto index = m_executionPlan.rbegin(); index != m_executionPlan.rend(); ++index) {
        m_nodes[*index].node->finish();
    }
}

} // namespace torquebus
