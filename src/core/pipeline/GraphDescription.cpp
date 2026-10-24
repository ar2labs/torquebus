// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/GraphDescription.h"

#include "core/pipeline/nodes/FrameNodes.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
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

        if (edge.frameFormat != CanFormatMatch::Any && producing != PortType::Frames) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("The wire from '{}' to '{}' carries {}, and only a wire that carries "
                            "frames can be limited to 11-bit or 29-bit ones",
                            from->id,
                            to->id,
                            toString(producing)));
        }

        if (!from->enabled || !to->enabled) {
            // Not counted towards the one-wire-per-input rule: the wire will
            // not exist at build time, so refusing the graph over it would
            // refuse a graph that runs perfectly well.
            continue;
        }

        // A block wired to itself. For most blocks that is a cycle, which compile()
        // would refuse with a message about a loop; for a CAN Bus it is not a cycle in
        // the graph at all - the bus is built as two nodes, one that reads the channel
        // and one that writes it - and so the loop closes on the wire: everything the bus
        // hears is sent straight back onto it, heard again, and sent again. That is a
        // flood the hardware carries out, at the rate the hardware allows.
        if (edge.fromNode == edge.toNode) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("'{}' is wired to itself. {}",
                            from->id,
                            from->typeName == kCanBusType
                                ? "A bus that sends what it hears back onto itself would repeat "
                                  "every frame for as long as the measurement runs."
                                : "A block cannot take its own output as its input."));
        }

        // The same loop through two buses that are one channel.
        if (from->typeName == kCanBusType && to->typeName == kCanBusType
            && from->parameters.integer("channel", 0) == to->parameters.integer("channel", 0)) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("'{}' and '{}' are both CAN {}. Wiring one to the other would send "
                            "every frame back onto the bus it came from. A gateway joins buses "
                            "that are different channels.",
                            from->id,
                            to->id,
                            from->parameters.integer("channel", 0) + 1));
        }

        // A fan-in port is the exception: it is the one place a description
        // takes several wires, because build() gives each of them a node to end
        // in - see PortDescriptor::fanIn - and so no stream is ever merged by a
        // rule nobody chose.
        if (!toType->inputs[edge.toPort].fanIn && ++arrivals[{to->id, edge.toPort}] > 1) {
            // Frames have the policy written down, in a block of its own (FrameMergeNode.h); the
            // other kinds of stream have none yet, and the message does not pretend otherwise.
            const bool frames = toType->inputs[edge.toPort].type == PortType::Frames;

            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Two wires arrive at '{}' port {}. An input takes one, because "
                            "merging two streams needs a policy{}.",
                            to->id,
                            edge.toPort,
                            frames ? " - wire both into a Frame Merge block and its output here"
                                   : ""));
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

        PortRef source{from->second, edge.fromPort};
        PortRef destination{to->second, edge.toPort};

        // A wire limited to one identifier format passes through a filter. It
        // is the filter the Frame Filter block builds, not a second kind of
        // filtering: what a wire says it carries is then checked, counted and
        // reported exactly as if the user had drawn the block in.
        if (edge.frameFormat != CanFormatMatch::Any) {
            const bool standard = edge.frameFormat == CanFormatMatch::StandardOnly;

            CanFilter rule;
            rule.name = standard ? "11-bit" : "29-bit";
            rule.format = edge.frameFormat;

            CanFilterSet rules;
            rules.add(std::move(rule));

            auto filter = std::make_unique<FrameFilterNode>(std::move(rules));
            filter->setLabel(std::format(
                "{} to {} ({} only)", edge.fromNode, edge.toNode, standard ? "11-bit" : "29-bit"));

            const NodeId filterId = graph.addNode(std::move(filter));
            if (Result result = graph.connect(source, PortRef{filterId, 0}); result.failed()) {
                return result;
            }

            source = PortRef{filterId, 0};
        }

        // A fan-in port takes any number of wires, and a running node takes one
        // per input - so the wire ends in a node of the type's own making
        // (for the CAN Bus, a CAN Transmit on its channel) and not in the one
        // the description names, which the others do not share.
        const NodeDescription* target = find(edge.toNode);
        const NodeTypeInfo* targetType =
            target == nullptr ? nullptr : catalog.find(target->typeName);

        if (targetType != nullptr && targetType->inputs[edge.toPort].fanIn) {
            std::unique_ptr<IPipelineNode> tap;

            if (Result result = catalog.createTap(
                    target->typeName, target->parameters, context, target->id, edge.toPort, tap);
                result.failed()) {
                return result;
            }

            if (tap == nullptr) {
                return Result::error(ErrorCode::Unknown,
                                     std::format("Node '{}' of type '{}' built nothing for a wire",
                                                 target->id,
                                                 target->typeName));
            }

            destination = PortRef{graph.addNode(std::move(tap)), 0};
        }

        if (Result result = graph.connect(source, destination); result.failed()) {
            return result;
        }
    }

    return Result::ok();
}

} // namespace torquebus
