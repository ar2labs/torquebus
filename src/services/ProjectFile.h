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
// Each node in the JSON array carries its identifier, catalog type, parameter
// map, and canvas coordinates, followed by the directed wires connecting output
// ports to input ports.
//
// What a saved file contains today is the pipeline. Channels, databases,
// transmit lists and workspaces join it as those features arrive; the version
// field is what lets an older file still open when they do.

#pragma once

#include "core/Result.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/pipeline/GraphDescription.h"
#include "core/transmit/TransmitList.h"

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
    /// Bumped to 2 when the transmit list joined the file.
    ///
    /// An addition an older reader could ignore would not need this. A transmit
    /// list is not that: an older build would open the project, show no rows,
    /// and drop them on the next save - which is exactly what the version check
    /// below refuses to let happen. Older files still load; only *newer* ones
    /// are refused, and with a message that says why.
    /// Bumped to 3 when the dashboard joined it, for the same reason: an older
    /// build would open the project, draw no dashboard, and write the file back
    /// without one.
    static constexpr int kFormatVersion = 3;

    /// The extension, without the dot.
    [[nodiscard]] static QString extension() { return QStringLiteral("tbsproj"); }

    /// A file filter for QFileDialog.
    [[nodiscard]] static QString fileFilter();

    /// Writes `pipeline` to `path`.
    ///
    /// Atomic: written to a temporary file and renamed, so an interrupted save
    /// cannot leave a truncated project behind. Losing yesterday's work to a
    /// crash during today's save is not a trade anyone agreed to.
    /// Writes the project: the pipeline, and the transmit list beside it.
    ///
    /// Three out-parameters now, and this is the last one that goes in as a
    /// parameter: when the database list joins the file, these become a
    /// document object. Three is where a fourth would start being passed in the
    /// wrong order by somebody, and no compiler would notice.
    ///
    /// Not defaulted, deliberately. An overload that left the dashboard out
    /// would compile at every existing call site and silently erase a dashboard
    /// on the next save, which is the worst kind of convenience.
    [[nodiscard]] static Result save(const QString& path,
                                     const GraphDescription& pipeline,
                                     const TransmitList& transmit,
                                     const DashboardDescription& dashboard);

    /// Reads `path` into `pipeline`, replacing its contents.
    ///
    /// On failure `pipeline` is left untouched: a half-read project would leave
    /// the canvas showing something that was never saved.
    /// Reads a project. Every argument is left untouched when it fails.
    ///
    /// A file with no dashboard in it - every project written before this
    /// version - loads with an empty one rather than failing. That is what the
    /// version field is for, and it is the whole reason an older project still
    /// opens here.
    [[nodiscard]] static Result load(const QString& path,
                                     GraphDescription& pipeline,
                                     TransmitList& transmit,
                                     DashboardDescription& dashboard);
};

} // namespace torquebus::services
