// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The project on disk: a `.tbsproj` holding the pipeline the user drew.
//
// JSON, and deliberately readable JSON. A project file is something an engineer
// diffs in a review, copies between machines and checks into a lab's
// configuration repository, so it is written with stable key order, indented,
// and with the graph laid out the way the canvas shows it - nodes then wires,
// each node naming its type and settings.
//
// The format follows cansim's nodes.json closely, and not by accident: that
// file arrived at the same shape from the same problem, and twenty working
// scripts have been configured through it. Where the two differ, this one is
// the superset - ports on the wires, and canvas positions.
//
// What a saved file contains today is the pipeline. Channels, databases,
// transmit lists and workspaces join it as those features arrive; the version
// field is what lets an older file still open when they do.

#pragma once

#include "core/Result.h"
#include "core/pipeline/GraphDescription.h"

#include <QString>

namespace torquebus::services {

/// One project, as a file.
class ProjectFile final {
public:
    /// The format this build writes.
    ///
    /// Bumped when the layout changes in a way an older reader could not
    /// handle. A file from a newer version is refused with a message that says
    /// so, rather than being read half-correctly - a project that loads with
    /// its wires silently missing is worse than one that refuses to load.
    static constexpr int kFormatVersion = 1;

    /// The extension, without the dot.
    [[nodiscard]] static QString extension() { return QStringLiteral("tbsproj"); }

    /// A file filter for QFileDialog.
    [[nodiscard]] static QString fileFilter();

    /// Writes `pipeline` to `path`.
    ///
    /// Atomic: written to a temporary file and renamed, so an interrupted save
    /// cannot leave a truncated project behind. Losing yesterday's work to a
    /// crash during today's save is not a trade anyone agreed to.
    [[nodiscard]] static Result save(const QString& path, const GraphDescription& pipeline);

    /// Reads `path` into `pipeline`, replacing its contents.
    ///
    /// On failure `pipeline` is left untouched: a half-read project would leave
    /// the canvas showing something that was never saved.
    [[nodiscard]] static Result load(const QString& path, GraphDescription& pipeline);
};

} // namespace torquebus::services
