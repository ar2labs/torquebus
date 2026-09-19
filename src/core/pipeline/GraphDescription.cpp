// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/GraphDescription.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace torquebus {

void GraphDescription::removeNode(const std::string& id)
{
    std::erase_if(m_nodes, [&id](const NodeDescription& node) { return node.id == id; });

    std::erase_if(m_edges, [&id](const EdgeDescription& edge) {
        return edge.fromNode == id || edge.toNode == id;
    });
}

const NodeDescription* GraphDescription::find(const std::string& id) const
{
    const auto it = std::find_if(m_nodes.begin(),
                                 m_nodes.end(),
                                 [&id](const NodeDescription& node) { return node.id == id; });

    return it == m_nodes.end() ? nullptr : &*it;
}

void GraphDescription::clear()
{
    m_nodes.clear();
    m_edges.clear();
}

std::string GraphDescription::uniqueId(std::string_view base) const
{
    std::string candidate{base};

    // Plain `base` first, then base_2, base_3. Users number from one and the
    // first one has no number - "ecu, ecu_2, ecu_3" reads the way a person
    // would write it, where "ecu_1, ecu_2" invites the question of what ecu_0 was.
    for (int suffix = 2; find(candidate) != nullptr; ++suffix) {
        candidate = std::format("{}_{}", base, suffix);
    }

    return candidate;
}

Result GraphDescription::validate(const NodeCatalog& catalog) const
{
    std::set<std::string> seen;

    for (const NodeDescription& node : m_nodes) {
        if (node.id.empty()) {
            return Result::error(ErrorCode::InvalidArgument, "A node has no id");
        }

        if (!seen.insert(node.id).second) {
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("Two nodes share the id '{}'. Ids address edges, so a "
                                             "duplicate makes a wire ambiguous.",
                                             node.id));
        }

        // A disabled node is skipped by build(), wires and all - so validate()
        // must not refuse a graph over one. It did: a half-configured block
        // could not be switched off, because the parameter check below still
        // ran on it and Start was refused over a block that would never have
        // been instantiated.
        //
        // The rule this establishes, and the reason the check sits here rather
        // than being spelled into each test below: **whatever validate()
        // rejects, build() must also reject.** Anything else makes "off" a
        // setting that does not mean off.
        if (!node.enabled) {
            continue;
        }

        if (!catalog.contains(node.typeName)) {
            return Result::error(
                ErrorCode::NotImplemented,
                std::format("Node '{}' has unknown type '{}'", node.id, node.typeName));
        }

        // What the node's own settings say, as far as that can be known
        // without a build context. A block dropped on the canvas and not yet
        // filled in is the ordinary case, not a strange one - it should be
        // reported here, where the user is looking at it, rather than at the
        // next Start when they are looking at something else.
        if (Result result = catalog.validateParameters(node.typeName, node.parameters, node.id);
            result.failed()) {
            return result;
        }
    }

    // How many edges arrive at each input. An input takes exactly one, because
    // merging two streams needs a policy and picking one silently is the wrong
    // answer (PipelineGraph enforces the same rule; catching it here means the
    // canvas can refuse the wire as it is drawn).
    std::map<std::pair<std::string, std::size_t>, int> arrivals;

    for (const EdgeDescription& edge : m_edges) {
        const NodeDescription* from = find(edge.fromNode);
        const NodeDescription* to = find(edge.toNode);

        if (from == nullptr) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("An edge starts at '{}', which is not in this graph", edge.fromNode));
        }

        if (to == nullptr) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("An edge ends at '{}', which is not in this graph", edge.toNode));
        }

        const NodeTypeInfo* fromType = catalog.find(from->typeName);
        const NodeTypeInfo* toType = catalog.find(to->typeName);

        if (fromType == nullptr || toType == nullptr) {
            continue; // Already reported above.
        }

        if (edge.fromPort >= fromType->outputs.size()) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("'{}' has {} output port(s), so port {} does not exist",
                            from->id,
                            fromType->outputs.size(),
                            edge.fromPort));
        }

        if (edge.toPort >= toType->inputs.size()) {
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("'{}' has {} input port(s), so port {} does not exist",
                                             to->id,
                                             toType->inputs.size(),
                                             edge.toPort));
        }

        const PortType producing = fromType->outputs[edge.fromPort].type;
        const PortType consuming = toType->inputs[edge.toPort].type;

        if (producing != consuming) {
            // Both ends named, and both types: "cannot connect" alone leaves
            // the user to work out which of the two is wrong.
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("'{}' produces {} but '{}' expects {}",
                                             from->id,
                                             toString(producing),
                                             to->id,
                                             toString(consuming)));
        }

        if (!from->enabled || !to->enabled) {
            // Not counted towards the one-wire-per-input rule: the wire will
            // not exist at build time, so refusing the graph over it would
            // refuse a graph that runs perfectly well.
            continue;
        }

        if (++arrivals[{to->id, edge.toPort}] > 1) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Two wires arrive at '{}' port {}. An input takes one, because "
                            "merging two streams needs a policy - put a merge node between "
                            "them when there is one.",
                            to->id,
                            edge.toPort));
        }
    }

    return Result::ok();
}

Result GraphDescription::build(const NodeCatalog& catalog,
                               const NodeBuildContext& context,
                               PipelineGraph& graph) const
{
    // Validate first, so a bad graph fails on the description - where the
    // message can name ids the user recognises - rather than halfway through
    // instantiating, with some nodes already built and a channel already open.
    if (Result result = validate(catalog); result.failed()) {
        return result;
    }

    std::map<std::string, NodeId> built;

    for (const NodeDescription& node : m_nodes) {
        if (!node.enabled) {
            continue;
        }

        std::unique_ptr<IPipelineNode> instance;

        if (Result result =
                catalog.create(node.typeName, node.parameters, context, node.id, instance);
            result.failed()) {
            return result;
        }

        if (instance == nullptr) {
            return Result::error(
                ErrorCode::Unknown,
                std::format("Node '{}' of type '{}' built nothing", node.id, node.typeName));
        }

        built.emplace(node.id, graph.addNode(std::move(instance)));
    }

    for (const EdgeDescription& edge : m_edges) {
        // A wire touching a disabled node is skipped rather than being an
        // error: switching a node off must not require rewiring around it.
        const auto from = built.find(edge.fromNode);
        const auto to = built.find(edge.toNode);

        if (from == built.end() || to == built.end()) {
            continue;
        }

        if (Result result = graph.connect(PortRef{from->second, edge.fromPort},
                                          PortRef{to->second, edge.toPort});
            result.failed()) {
            return result;
        }
    }

    return Result::ok();
}

} // namespace torquebus
