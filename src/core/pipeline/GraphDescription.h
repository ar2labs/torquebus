// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The graph as data: what the canvas edits, what the project file stores, and
// what the engine builds from.
//
// PipelineGraph is the *running* graph - compiled, with node objects and an
// execution plan, alive only between start and stop. GraphDescription is the
// user's graph: names, types, settings, wires and positions, which outlive any
// measurement and survive being written to a file and read back.
//
// Keeping them apart is what lets the engine rebuild from scratch on every
// start (a stale edge to a removed channel is a bug that survives months)
// without the user losing anything. The description is the source of truth; the
// running graph is derived from it, every time.
//
// Nodes are addressed by a string id rather than by index, because a project
// file that renumbers when a node is deleted is a project file whose diffs are
// unreadable and whose edges silently point somewhere else.

#pragma once

#include "core/Result.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/NodeParameters.h"
#include "core/pipeline/PipelineGraph.h"

#include <string>
#include <vector>

namespace torquebus {

struct NodeDescription final {
    /// Unique within the graph, stable across saves: "ecu_motor", "trace_1".
    std::string id;

    /// A type name from the catalog: "can.source", "lua.ecu".
    std::string typeName;

    NodeParameters parameters;

    /// Where the node sits on the canvas.
    ///
    /// Carried by the description and ignored by the runtime. It belongs here
    /// because it is part of what the user made - reopening a project with the
    /// blocks rearranged into a default layout would be its own small betrayal
    /// - and it costs the executor nothing, since building never reads it.
    double x{0.0};
    double y{0.0};

    /// A node the user has switched off.
    ///
    /// Kept in the project, skipped when building - along with every wire that
    /// touches it. Deleting a node to try the measurement without it, then
    /// drawing it again afterwards, loses its settings and its position; this
    /// is the same experiment without the loss.
    ///
    /// Anything downstream of a disabled node simply receives nothing. That is
    /// the honest reading of "off" and it is what cansim's `enabled` flag
    /// means, so the two behave alike.
    bool enabled{true};

    [[nodiscard]] friend bool operator==(const NodeDescription&, const NodeDescription&) = default;
};

struct EdgeDescription final {
    std::string fromNode;
    std::size_t fromPort{0};
    std::string toNode;
    std::size_t toPort{0};

    [[nodiscard]] friend bool operator==(const EdgeDescription&, const EdgeDescription&) = default;
};

/// A whole pipeline, as the user composed it.
class GraphDescription final {
public:
    void addNode(NodeDescription node) { m_nodes.push_back(std::move(node)); }
    void addEdge(EdgeDescription edge) { m_edges.push_back(std::move(edge)); }

    /// Removes a node and every edge touching it.
    ///
    /// Both, always: an edge to a node that no longer exists is the one
    /// inconsistency a description must never be able to hold, because it fails
    /// at build time, long after the deletion that caused it.
    void removeNode(const std::string& id);

    [[nodiscard]] const std::vector<NodeDescription>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const std::vector<EdgeDescription>& edges() const noexcept { return m_edges; }

    [[nodiscard]] std::vector<NodeDescription>& nodes() noexcept { return m_nodes; }
    [[nodiscard]] std::vector<EdgeDescription>& edges() noexcept { return m_edges; }

    /// Nullptr when absent.
    [[nodiscard]] const NodeDescription* find(const std::string& id) const;

    [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
    void clear();

    /// An id nothing else uses, derived from `base` ("ecu", "ecu_2", "ecu_3").
    [[nodiscard]] std::string uniqueId(std::string_view base) const;

    /// Checks what can be checked without building anything: duplicate ids,
    /// unknown types, edges naming a node that is not there, ports that do not
    /// exist on the type, and inputs taking more than one edge.
    ///
    /// Separate from build() because the canvas wants to say "that wire will
    /// not work" while the user is drawing it, with no engine running and no
    /// nodes instantiated.
    [[nodiscard]] Result validate(const NodeCatalog& catalog) const;

    /// Instantiates every node and wires them into `graph`.
    ///
    /// Does not compile: the caller adds whatever else it needs - the engine
    /// adds its own default trace path - and compiles once, at the end.
    [[nodiscard]] Result build(const NodeCatalog& catalog,
                               const NodeBuildContext& context,
                               PipelineGraph& graph) const;

    [[nodiscard]] friend bool operator==(const GraphDescription&,
                                         const GraphDescription&) = default;

private:
    std::vector<NodeDescription> m_nodes;
    std::vector<EdgeDescription> m_edges;
};

} // namespace torquebus
