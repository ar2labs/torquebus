// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/BusGrouping.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace torquebus {
namespace {

constexpr std::string_view kSourceType = "can.source";
constexpr std::string_view kTransmitType = "can.transmit";

[[nodiscard]] bool isSource(const NodeDescription* node)
{
    return node != nullptr && node->typeName == kSourceType;
}

[[nodiscard]] bool isTransmit(const NodeDescription* node)
{
    return node != nullptr && node->typeName == kTransmitType;
}

/// The channel a block names, 0 - CAN 1 - when it names none, which is what the
/// catalog builds it as.
[[nodiscard]] std::int64_t channelOf(const NodeDescription& node)
{
    return std::clamp<std::int64_t>(node.parameters.integer("channel", 0), 0, 254);
}

/// The CAN Channel and CAN Transmit blocks that are only a way onto a bus.
///
/// Wired, and wired the way their kind is wired: a block with a wire the wrong
/// way round, or with one to another of its own kind, is a gateway or a graph
/// somebody edited by hand, and is not this function's to rewrite.
[[nodiscard]] std::set<std::string> replaceableBlocks(const GraphDescription& graph)
{
    std::set<std::string> wired;
    std::set<std::string> excluded;

    for (const EdgeDescription& edge : graph.edges()) {
        const NodeDescription* from = graph.find(edge.fromNode);
        const NodeDescription* to = graph.find(edge.toNode);

        // The two ways a wire is the way round its block is wired: out of a CAN
        // Channel, into a CAN Transmit.
        const bool outOfSource = isSource(from);
        const bool intoTransmit = isTransmit(to);

        if (outOfSource) {
            wired.insert(from->id);
        }
        if (intoTransmit) {
            wired.insert(to->id);
        }

        // Out of a CAN Transmit or into a CAN Channel is a graph the catalog
        // would not have let anybody draw.
        if (isTransmit(from)) {
            excluded.insert(from->id);
        }
        if (isSource(to)) {
            excluded.insert(to->id);
        }

        // Channel to transmit: a gateway.
        if (outOfSource && intoTransmit) {
            excluded.insert(from->id);
            excluded.insert(to->id);
        }
    }

    std::set<std::string> result;
    for (const NodeDescription& node : graph.nodes()) {
        if (node.enabled && wired.contains(node.id) && !excluded.contains(node.id)) {
            result.insert(node.id);
        }
    }
    return result;
}

} // namespace

bool canGroupOnBuses(const GraphDescription& graph)
{
    return !replaceableBlocks(graph).empty();
}

BusGrouping groupOnBuses(GraphDescription& graph)
{
    const std::set<std::string> replaced = replaceableBlocks(graph);
    BusGrouping result;

    if (replaced.empty()) {
        return result;
    }

    // Which bus each replaced block becomes, and what a new one is made from.
    struct Plan final {
        std::string busId;
        bool createBus{false};
        const NodeDescription* firstBlock{nullptr}; // First in the graph's order.
        const NodeDescription* firstSource{nullptr};
    };

    std::map<std::int64_t, Plan> plans;

    for (const NodeDescription& node : graph.nodes()) {
        if (!replaced.contains(node.id)) {
            continue;
        }

        Plan& plan = plans[channelOf(node)];

        if (plan.firstBlock == nullptr) {
            plan.firstBlock = &node;
        }
        if (isSource(&node) && plan.firstSource == nullptr) {
            plan.firstSource = &node;
        }
    }

    for (auto& [channel, plan] : plans) {
        // A bus that is already on the channel is the one to use - unless it is
        // switched off, which would switch off everything that is moved onto it.
        const auto existing =
            std::ranges::find_if(graph.nodes(), [channel](const NodeDescription& n) {
                return n.typeName == kCanBusType && n.enabled && channelOf(n) == channel;
            });

        if (existing != graph.nodes().end()) {
            plan.busId = existing->id;
            continue;
        }

        plan.createBus = true;

        // The id of the first CAN Channel when there is one - it is leaving, so
        // the id is free, and the wires that named it keep naming the thing they
        // were wired to.
        plan.busId = plan.firstSource != nullptr
                         ? plan.firstSource->id
                         : graph.uniqueId(std::format("can_{}", channel + 1));
    }

    // Where each replaced block's wires go.
    std::map<std::string, std::string> busOf;
    for (const NodeDescription& node : graph.nodes()) {
        if (replaced.contains(node.id)) {
            busOf.emplace(node.id, plans.at(channelOf(node)).busId);
        }
    }

    // The new list of blocks: the old, without the replaced ones, and each new
    // bus in the place of the first block it replaces.
    std::vector<NodeDescription> blocks;
    blocks.reserve(graph.nodes().size());

    for (const NodeDescription& node : graph.nodes()) {
        if (!replaced.contains(node.id)) {
            blocks.push_back(node);
            continue;
        }

        ++result.blocksRemoved;

        const Plan& plan = plans.at(channelOf(node));
        if (!plan.createBus || plan.firstBlock != &node) {
            continue;
        }

        const NodeDescription& anchor = plan.firstSource != nullptr ? *plan.firstSource : node;

        NodeDescription bus;
        bus.id = plan.busId;
        bus.typeName = std::string{kCanBusType};
        bus.parameters.set("channel", ParameterValue::fromInteger(channelOf(node)));
        bus.x = anchor.x;
        bus.y = anchor.y;

        blocks.push_back(std::move(bus));
        ++result.busesAdded;
    }

    std::vector<EdgeDescription> wires;
    wires.reserve(graph.edges().size());

    for (const EdgeDescription& edge : graph.edges()) {
        EdgeDescription moved = edge;
        bool changed = false;

        if (const auto from = busOf.find(edge.fromNode); from != busOf.end()) {
            moved.fromNode = from->second;
            moved.fromPort = 0; // The bus's rx.
            changed = true;
        }
        if (const auto to = busOf.find(edge.toNode); to != busOf.end()) {
            moved.toNode = to->second;
            moved.toPort = 0; // The bus's tx.
            changed = true;
        }

        if (changed) {
            ++result.wiresMoved;
        }

        // Two transmit blocks fed from one output become one wire to the bus.
        if (std::ranges::find(wires, moved) == wires.end()) {
            wires.push_back(std::move(moved));
        }
    }

    graph.nodes() = std::move(blocks);
    graph.edges() = std::move(wires);

    return result;
}

} // namespace torquebus
