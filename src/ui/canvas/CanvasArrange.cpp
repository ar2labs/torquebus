// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/CanvasArrange.h"

#include "ui/canvas/CanvasLayout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace torquebus::ui {
namespace {

namespace lay = layout;

struct Size final {
    double width{0.0};
    double height{0.0};
};

[[nodiscard]] Size sizeOf(const NodeDescription& block, const ArrangeOptions& options)
{
    if (block.typeName == kCanBusType) {
        return {lay::kRailMinWidth, lay::kRailHeight};
    }
    if (options.expanded.contains(block.id)) {
        const double network =
            block.typeName == "tinyml.ecu" ? lay::kTinyMlCardGap + lay::kTinyMlCardHeight : 0.0;
        return {lay::kCardWidth, lay::kCardHeight + network};
    }
    return {lay::kChipWidth, lay::kChipHeight};
}

/// What the layout knows about the graph, read once.
struct Wiring final {
    std::set<std::string> buses;

    /// The first bus each module is wired to, in the order the graph lists them.
    std::map<std::string, std::string> home;

    /// Modules with a wire that puts frames on a bus.
    std::set<std::string> talkers;

    /// Wires that are not a drop to a bus.
    std::map<std::string, std::vector<std::string>> dataOut;
    std::map<std::string, std::vector<std::string>> dataIn;
};

[[nodiscard]] Wiring readWiring(const GraphDescription& graph)
{
    Wiring wiring;
    std::map<std::string, std::size_t> order;

    for (std::size_t index = 0; index < graph.nodes().size(); ++index) {
        const NodeDescription& block = graph.nodes()[index];
        order.emplace(block.id, index);

        if (block.typeName == kCanBusType) {
            wiring.buses.insert(block.id);
        }
    }

    const auto setHome = [&](const std::string& module, const std::string& bus) {
        const auto existing = wiring.home.find(module);
        if (existing == wiring.home.end() || order[bus] < order[existing->second]) {
            wiring.home[module] = bus;
        }
    };

    for (const EdgeDescription& edge : graph.edges()) {
        const bool fromBus = wiring.buses.contains(edge.fromNode);
        const bool toBus = wiring.buses.contains(edge.toNode);

        if (fromBus && toBus) {
            continue; // A gateway has no place in a layout of one bus.
        }
        if (fromBus) {
            setHome(edge.toNode, edge.fromNode);
        } else if (toBus) {
            setHome(edge.fromNode, edge.toNode);
            wiring.talkers.insert(edge.fromNode);
        } else {
            wiring.dataOut[edge.fromNode].push_back(edge.toNode);
            wiring.dataIn[edge.toNode].push_back(edge.fromNode);
        }
    }

    return wiring;
}

/// A row of blocks, and where each wants to be.
struct Row final {
    std::vector<const NodeDescription*> blocks;
    std::vector<double> wantedX;
};

class Layout final {
public:
    Layout(const GraphDescription& graph, const ArrangeOptions& options)
        : m_graph{graph}
        , m_options{options}
        , m_wiring{readWiring(graph)}
    { }

    [[nodiscard]] std::vector<Placement> run()
    {
        double top = 0.0;

        // Buses by channel, then by where the graph lists them: CAN 1 above CAN 2.
        std::vector<const NodeDescription*> buses;
        for (const NodeDescription& block : m_graph.nodes()) {
            if (block.typeName == kCanBusType) {
                buses.push_back(&block);
            }
        }
        std::ranges::stable_sort(buses, [](const NodeDescription* a, const NodeDescription* b) {
            return a->parameters.integer("channel", 0) < b->parameters.integer("channel", 0);
        });

        for (const NodeDescription* bus : buses) {
            top = layoutBus(*bus, top);
        }

        layoutFree(top);

        // In the order the graph holds the blocks, which is what makes the result
        // the same for the same graph.
        std::vector<Placement> result;
        result.reserve(m_graph.nodes().size());

        for (const NodeDescription& block : m_graph.nodes()) {
            if (const auto placed = m_placed.find(block.id); placed != m_placed.end()) {
                result.push_back(Placement{block.id, placed->second.first, placed->second.second});
            }
        }

        return result;
    }

private:
    [[nodiscard]] double pitchOf(const NodeDescription& block) const
    {
        return sizeOf(block, m_options).width + lay::kChipGap;
    }

    /// Members of a bus in the order they are laid out: where they are now, left
    /// to right, then as the graph lists them.
    [[nodiscard]] std::vector<const NodeDescription*> membersOf(const std::string& bus) const
    {
        std::vector<const NodeDescription*> members;
        for (const NodeDescription& block : m_graph.nodes()) {
            const auto home = m_wiring.home.find(block.id);
            if (home != m_wiring.home.end() && home->second == bus
                && !m_wiring.buses.contains(block.id)) {
                members.push_back(&block);
            }
        }

        // Row by row, nearest the bus first, and left to right in a row. Not by x alone: a row that
        // wrapped was set half a pitch in, and sorting the two rows together by x would interleave
        // them - a different diagram each time it was arranged.
        const NodeDescription* railBlock = m_graph.find(bus);
        const double railY = railBlock != nullptr ? railBlock->y : 0.0;
        const double step = lay::kChipHeight + lay::kRowGap;
        const auto rowOf = [railY, step](const NodeDescription* block) {
            return static_cast<long>(std::floor(std::abs(block->y - railY) / step));
        };

        std::ranges::stable_sort(members,
                                 [&rowOf](const NodeDescription* a, const NodeDescription* b) {
                                     const long rowA = rowOf(a);
                                     const long rowB = rowOf(b);
                                     return rowA != rowB ? rowA < rowB : a->x < b->x;
                                 });
        return members;
    }

    /// Cuts a list into rows of at most `maxPerRow`.
    [[nodiscard]] std::vector<std::vector<const NodeDescription*>>
    wrapped(const std::vector<const NodeDescription*>& blocks) const
    {
        std::vector<std::vector<const NodeDescription*>> rows;
        const auto perRow = static_cast<std::size_t>(std::max(1, m_options.maxPerRow));

        for (std::size_t first = 0; first < blocks.size(); first += perRow) {
            const std::size_t last = std::min(blocks.size(), first + perRow);
            rows.emplace_back(blocks.begin() + static_cast<std::ptrdiff_t>(first),
                              blocks.begin() + static_cast<std::ptrdiff_t>(last));
        }

        return rows;
    }

    [[nodiscard]] double rowHeight(const std::vector<const NodeDescription*>& blocks) const
    {
        double height = 0.0;
        for (const NodeDescription* block : blocks) {
            height = std::max(height, sizeOf(*block, m_options).height);
        }
        return height;
    }

    /// Puts a row at `y`, each block at the first free place at or after the one it
    /// wants, so that nothing in a row overlaps and a block that wants to be under
    /// the one that feeds it is, when there is room.
    ///
    /// `alignBottom` sets the blocks of a row so that their bottoms line up rather than their tops
    /// - for the rows over a bus, where it is the bottom that the drop leaves from and a taller
    /// block must not make the others' drops longer.
    void placeRow(const Row& row, double y, double startX, bool alignBottom = false)
    {
        const double height = rowHeight(row.blocks);

        double next = startX;
        for (std::size_t index = 0; index < row.blocks.size(); ++index) {
            const NodeDescription& block = *row.blocks[index];
            const double x = std::max(next, row.wantedX[index]);
            const double top = alignBottom ? y + (height - sizeOf(block, m_options).height) : y;

            m_placed[block.id] = {x, top};
            next = x + pitchOf(block);
        }
    }

    /// Where a row of `blocks` sit when nothing says where they want to be: in a
    /// line from the left, `stagger` of a pitch in.
    [[nodiscard]] Row evenRow(const std::vector<const NodeDescription*>& blocks,
                              double stagger) const
    {
        Row row;
        row.blocks = blocks;

        double x = lay::kRailPlateWidth + lay::kRailInset + stagger;
        for (const NodeDescription* block : blocks) {
            row.wantedX.push_back(x);
            x += pitchOf(*block);
        }
        return row;
    }

    /// Everything downstream of `starts` that is not wired to a bus, by how many
    /// steps from them, deepest last.
    [[nodiscard]] std::vector<std::vector<const NodeDescription*>>
    downstreamOf(const std::vector<const NodeDescription*>& starts)
    {
        std::map<std::string, int> depth;
        for (const NodeDescription* start : starts) {
            depth[start->id] = 0;
        }

        // Longest path, by relaxing until nothing moves. Bounded by the number of
        // blocks, which is also the most steps a path can have: a cycle - which a
        // pipeline cannot have, and a hand-edited file might - would otherwise
        // never settle.
        for (std::size_t pass = 0; pass < m_graph.nodes().size(); ++pass) {
            bool moved = false;

            for (const auto& [from, targets] : m_wiring.dataOut) {
                const auto at = depth.find(from);
                if (at == depth.end()) {
                    continue;
                }
                for (const std::string& target : targets) {
                    if (m_wiring.home.contains(target) || m_claimed.contains(target)) {
                        continue;
                    }
                    int& d = depth[target];
                    if (d < at->second + 1) {
                        d = at->second + 1;
                        moved = true;
                    }
                }
            }

            if (!moved) {
                break;
            }
        }

        std::vector<std::vector<const NodeDescription*>> layers;
        for (const NodeDescription& block : m_graph.nodes()) {
            const auto at = depth.find(block.id);
            if (at == depth.end() || at->second == 0) {
                continue;
            }

            const auto layer = static_cast<std::size_t>(at->second);
            if (layers.size() < layer) {
                layers.resize(layer);
            }
            layers[layer - 1].push_back(&block);
            m_claimed.insert(block.id);
        }

        return layers;
    }

    /// Where a block wants to be: under the first block that feeds it.
    [[nodiscard]] double wantedUnder(const NodeDescription& block) const
    {
        const auto feeders = m_wiring.dataIn.find(block.id);
        if (feeders != m_wiring.dataIn.end()) {
            for (const std::string& feeder : feeders->second) {
                if (const auto placed = m_placed.find(feeder); placed != m_placed.end()) {
                    return placed->second.first;
                }
            }
        }
        return lay::kRailPlateWidth + lay::kRailInset;
    }

    /// Lays out the layers under `y`. Returns the bottom of the last.
    double layDownstream(const std::vector<std::vector<const NodeDescription*>>& layers, double y)
    {
        for (const auto& layer : layers) {
            std::vector<const NodeDescription*> sorted = layer;
            std::ranges::stable_sort(sorted,
                                     [this](const NodeDescription* a, const NodeDescription* b) {
                                         return wantedUnder(*a) < wantedUnder(*b);
                                     });

            for (const auto& chunk : wrapped(sorted)) {
                Row row;
                row.blocks = chunk;
                for (const NodeDescription* block : chunk) {
                    row.wantedX.push_back(wantedUnder(*block));
                }

                placeRow(row, y, lay::kRailPlateWidth + lay::kRailInset);
                y += rowHeight(chunk) + lay::kRowGap;
            }
        }
        return y;
    }

    /// One bus and everything on it. Returns where the next band starts.
    double layoutBus(const NodeDescription& bus, double top)
    {
        const std::vector<const NodeDescription*> members = membersOf(bus.id);

        std::vector<const NodeDescription*> speakers; // Above the rail.
        std::vector<const NodeDescription*> listeners; // Below it.

        for (const NodeDescription* member : members) {
            const bool speaks = m_wiring.talkers.contains(member->id);
            const bool feedsSomething = m_wiring.dataOut.contains(member->id);

            // What speaks and feeds nothing hangs above the rail. What something
            // is wired out of is under it, where what it feeds can be too - a drop
            // from a module over the rail to a block under it would cross the
            // wires of the bus.
            (speaks && !feedsSomething ? speakers : listeners).push_back(member);
            m_claimed.insert(member->id);
        }

        const auto above = wrapped(speakers);
        const auto below = wrapped(listeners);

        // Rows over the rail, nearest first, and their height.
        double aboveHeight = 0.0;
        for (const auto& row : above) {
            aboveHeight += rowHeight(row) + lay::kRowGap;
        }
        if (!above.empty()) {
            aboveHeight += lay::kDropLength - lay::kRowGap;
        }

        const double railY = top + aboveHeight;

        // The bus first, so what hangs from it can find it.
        m_placed[bus.id] = {0.0, railY};

        // Rows above, from the rail outwards. A row further out is half a pitch in,
        // so that its drops go between the blocks of the row nearer the rail.
        double y = railY - lay::kDropLength;
        for (std::size_t index = 0; index < above.size(); ++index) {
            const double height = rowHeight(above[index]);
            y -= height;

            const double stagger = (index % 2 == 1) ? lay::kChipPitch * 0.5 : 0.0;
            placeRow(evenRow(above[index], stagger),
                     y,
                     lay::kRailPlateWidth + lay::kRailInset,
                     /*alignBottom=*/true);
            y -= lay::kRowGap;
        }

        // Rows below.
        double belowY = railY + lay::kRailHeight + lay::kDropLength;
        for (std::size_t index = 0; index < below.size(); ++index) {
            const double stagger = (index % 2 == 1) ? lay::kChipPitch * 0.5 : 0.0;
            placeRow(
                evenRow(below[index], stagger), belowY, lay::kRailPlateWidth + lay::kRailInset);
            belowY += rowHeight(below[index]) + lay::kRowGap;
        }

        // Whatever the listeners feed, under them.
        belowY = layDownstream(downstreamOf(listeners), belowY);

        const double bottom = std::max(belowY, railY + lay::kRailHeight + lay::kRowGap);
        return bottom + lay::kRowGap;
    }

    /// The blocks that are on no bus: sources at the top, what they feed under.
    void layoutFree(double top)
    {
        std::vector<const NodeDescription*> roots;
        for (const NodeDescription& block : m_graph.nodes()) {
            if (m_claimed.contains(block.id) || m_wiring.buses.contains(block.id)) {
                continue;
            }
            if (!m_wiring.dataIn.contains(block.id)) {
                roots.push_back(&block);
            }
        }

        std::ranges::stable_sort(
            roots, [](const NodeDescription* a, const NodeDescription* b) { return a->x < b->x; });

        for (const NodeDescription* root : roots) {
            m_claimed.insert(root->id);
        }

        double y = top;
        for (const auto& chunk : wrapped(roots)) {
            placeRow(evenRow(chunk, 0.0), y, lay::kRailPlateWidth + lay::kRailInset);
            y += rowHeight(chunk) + lay::kRowGap;
        }

        y = layDownstream(downstreamOfFree(roots), y);

        // Anything left is on a cycle, or fed only by something that is: a hand
        // edited graph. It still has a place.
        std::vector<const NodeDescription*> rest;
        for (const NodeDescription& block : m_graph.nodes()) {
            if (!m_claimed.contains(block.id) && !m_wiring.buses.contains(block.id)) {
                rest.push_back(&block);
                m_claimed.insert(block.id);
            }
        }
        for (const auto& chunk : wrapped(rest)) {
            placeRow(evenRow(chunk, 0.0), y, lay::kRailPlateWidth + lay::kRailInset);
            y += rowHeight(chunk) + lay::kRowGap;
        }
    }

    [[nodiscard]] std::vector<std::vector<const NodeDescription*>>
    downstreamOfFree(const std::vector<const NodeDescription*>& roots)
    {
        return downstreamOf(roots);
    }

    const GraphDescription& m_graph;
    const ArrangeOptions& m_options;
    Wiring m_wiring;

    std::set<std::string> m_claimed;
    std::map<std::string, std::pair<double, double>> m_placed;
};

} // namespace

std::vector<Placement> layOutBlocks(const GraphDescription& graph, const ArrangeOptions& options)
{
    return Layout{graph, options}.run();
}

} // namespace torquebus::ui
