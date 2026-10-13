// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where a tidy diagram puts every block.
//
// A pipeline that has grown by dropping blocks wherever there was room is a
// pipeline nobody can follow, and a project that has been through a few
// hands is nearly always one. This is the one-button answer: every bus laid
// across the page as a line, the modules that speak on it hung above it, the
// ones that only listen hung below, and whatever those feed in rows under them
// - the way a simulation setup is drawn, and the way a reader looks for it.
//
// A pure function over the description, so that what it does is a thing that can
// be tested: given this graph, these blocks are here. It moves nothing itself;
// the canvas applies what it returns, which is what makes "arrange" one step a
// person can look at and put back.

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace torquebus::ui {

struct ArrangeOptions final {
    /// The most blocks in a row before it wraps. What the window can show at a
    /// readable size, which is for the caller to say: a row wider than the window
    /// is a row nobody can read without scrolling it.
    int maxPerRow{8};

    /// Blocks the canvas shows as cards. They are wider and taller than chips, and
    /// a row that did not know would put the next block under one.
    std::unordered_set<std::string> expanded;
};

/// Where one block's top left goes.
struct Placement final {
    std::string id;
    double x{0.0};
    double y{0.0};
};

/// A position for every block in the graph, in the order the graph holds them.
///
/// Per bus, from the top: rows of modules that speak on it and have nothing
/// downstream of them; the bus itself; a row of the modules that only listen, or
/// that something hangs off; and then each step further downstream, under the
/// block that feeds it. A block not wired to any bus is laid out the same way
/// without one: what feeds nothing at the top, what it feeds beneath. Within a
/// row the order the blocks are in on the canvas now is kept, which is what makes
/// arranging a diagram somebody has already half-arranged a refinement and not a
/// shuffle.
[[nodiscard]] std::vector<Placement> layOutBlocks(const GraphDescription& graph,
                                                  const ArrangeOptions& options = {});

} // namespace torquebus::ui
