// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The pictures a dashboard shows, and where a project finds them.
//
// A picture is a file, and a project is a file that names other files. The database list and the
// scripts of a project are written relative to the project's own folder when they are inside or
// near it, so that a project checked into a repository and cloned onto another machine still finds
// them; a path that cannot be written that way is written whole. A dashboard's pictures follow the
// same rule, for the same reason, and this file is the rule.
//
//   * **Stored**: relative to the project's folder when the picture is in it or a few folders away
//     from it, with forward slashes whatever the platform - a project is read on more than one -
//     and absolute when it is on another drive or far away.
//   * **Resolved**: a relative path is joined to the project's folder; an unsaved project has none,
//     and its relative paths mean the current directory, which is what a script path does.
//   * **Rebased**: Save As moves the folder. The pictures have not moved, so every relative path is
//     worked out again against the new one - or the dashboard of the project that was saved to
//     another folder would open with every picture missing.
//
// A picture that is not where the project says it is, is not an error that refuses the project. The
// widget says so on the dashboard and the Output panel names it, and the person who moved the file
// can put it back or choose again. That is the same trade validate() makes about a database that is
// on another machine.
//
// No Qt, so the rule is the same in the form that chooses a picture, the panel that draws it and
// the test that moves a project.

#pragma once

#include "core/dashboard/DashboardDescription.h"

#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// The extensions of the pictures the Dashboard draws, lowercase and without the dot.
[[nodiscard]] const std::vector<std::string_view>& imageExtensions();

/// Whether `path` names a picture, by its extension and without asking the disk.
[[nodiscard]] bool isImagePath(std::string_view path);

/// Where a widget's picture is on disk: an absolute path as it is, a relative one joined to
/// `baseDir` (the project's folder), and when there is no folder, to the current directory. Slashes
/// are forward and `.` and `..` are folded. Empty in, empty out.
[[nodiscard]] std::string resolveAsset(std::string_view image, std::string_view baseDir);

/// What to store for the file at `absolute`: relative to `baseDir` when the file is in it, below it
/// or up to four folders away, and `absolute` (with forward slashes) when it is on another drive,
/// farther off, or there is no folder. The inverse of resolveAsset().
[[nodiscard]] std::string storableAsset(std::string_view absolute, std::string_view baseDir);

/// Every picture a dashboard names - a widget's own and its states' - once each, in the order they
/// are first met.
[[nodiscard]] std::vector<std::string> assetsUsedBy(const DashboardDescription& dashboard);

/// Writes every picture path again for a project that has moved from `fromDir` to `toDir`, so that
/// each still names the same file. Paths that were absolute and are still not near are left as they
/// were.
void rebaseAssets(DashboardDescription& dashboard,
                  std::string_view fromDir,
                  std::string_view toDir);

/// A picture a widget names and the disk does not have.
struct MissingAsset final {
    std::string widgetId;

    /// As the widget writes it, not as it resolves: it is what the person recognises.
    std::string image;
};

/// The pictures that are not where `baseDir` says they should be.
[[nodiscard]] std::vector<MissingAsset> missingAssets(const DashboardDescription& dashboard,
                                                      std::string_view baseDir);

} // namespace torquebus
