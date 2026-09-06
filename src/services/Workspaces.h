// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Named panel arrangements: PLAN.md section 7's "CAN Development",
// "Diagnostics", "Vehicle Testing", "Logging", "Simulation".
//
// The window already remembers one layout - the one you left it in. That is
// enough until the same person uses the tool for two different jobs, at which
// point it is actively unhelpful: reading a trace wants the trace across the
// whole window, and building a simulation wants the canvas and the script
// output side by side, and neither arrangement survives an afternoon of the
// other one.
//
// A workspace is a name and a layout blob. Two details make it more than a map:
//
//   * **Each one remembers the dock-layout version it was saved by.** The blob
//     is KDDockWidgets' own serialisation, and it names every panel that
//     existed when it was written. A build with a panel that did not exist then
//     cannot restore it faithfully, and half-restoring a layout is worse than
//     saying so: the missing panel is simply gone, with no way to know it ever
//     existed. A workspace from another version is refused, by name, with the
//     reason.
//
//   * **The name is the key, so the name is constrained.** Stored as JSON
//     object keys, which means a name containing a slash would read back as
//     something else. Names are trimmed and slashes refused at the door rather
//     than mangled later.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace torquebus::services {

class SettingsStore;

class Workspaces final {
public:
    /// Reads from `settings`. Borrowed; must outlive this object.
    explicit Workspaces(SettingsStore& settings);

    /// Saved names, in the order they were created.
    [[nodiscard]] QStringList names() const;

    [[nodiscard]] bool contains(const QString& name) const;

    /// True when `name` can be used: not empty after trimming, and with no
    /// character that the settings file would read back differently.
    [[nodiscard]] static bool isValidName(const QString& name);

    /// Stores `layout` under `name`, replacing any workspace of that name, and
    /// writes the settings file. Returns false for a name isValidName rejects.
    ///
    /// `layoutVersion` is the application's current dock-layout version; it is
    /// stored alongside so that a later build can tell whether the blob
    /// describes the panels it has.
    bool save(const QString& name, const QByteArray& layout, int layoutVersion);

    /// The layout stored under `name`, or an empty array when there is none or
    /// when it was written by a build with a different set of panels.
    ///
    /// `layoutVersion` is what the caller can restore. The version check is
    /// here rather than at the call site because forgetting it produces a
    /// window with panels silently missing, which nobody reports as a bug -
    /// they just rearrange it again.
    [[nodiscard]] QByteArray layoutFor(const QString& name, int layoutVersion) const;

    /// True when `name` exists but was saved by a build with a different panel
    /// set - so the window can say that rather than "no such workspace".
    [[nodiscard]] bool isStale(const QString& name, int layoutVersion) const;

    void remove(const QString& name);

private:
    [[nodiscard]] static QString layoutKey(const QString& name);
    [[nodiscard]] static QString versionKey(const QString& name);

    void storeNames(const QStringList& names) const;

    SettingsStore& m_settings;
};

} // namespace torquebus::services
