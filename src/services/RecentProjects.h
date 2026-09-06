// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The projects this user opened, most recent first.
//
// Small, and almost entirely policy. The interesting decisions are about what
// *not* to do:
//
//   * **A missing file stays on the list.** A project on a network share, or on
//     the laptop's other drive, is not gone because it is unreachable this
//     morning - and a list that quietly shortens itself every time somebody
//     works offline is a list nobody can rely on. An entry is dropped when
//     opening it actually fails, which is the moment there is evidence.
//
//   * **The same project cannot appear twice.** Paths reaching this class come
//     from a file dialog, a command line and a settings file written by an
//     older build, so they differ in separators and in case long before they
//     differ in meaning. They are compared as the filesystem would compare
//     them.
//
//   * **Order is use, not time.** Reopening the fifth entry moves it to the
//     top. No timestamps are kept: they would be one more thing to get wrong
//     across a timezone, and nothing here needs them.

#pragma once

#include <QString>
#include <QStringList>

namespace torquebus::services {

class SettingsStore;

class RecentProjects final {
public:
    /// How many are remembered.
    ///
    /// Ten fits a menu without a scroll bar and covers the two or three
    /// projects somebody actually alternates between, plus the ones they
    /// glanced at last week.
    static constexpr int kMaximum = 10;

    /// Reads the list from `settings`. The store is borrowed and must outlive
    /// this object; nothing is written until add(), remove() or clear().
    explicit RecentProjects(SettingsStore& settings);

    /// The paths, most recent first. Absolute, in the platform's separators.
    [[nodiscard]] const QStringList& paths() const noexcept { return m_paths; }

    [[nodiscard]] bool isEmpty() const noexcept { return m_paths.isEmpty(); }

    /// Puts `path` at the top, removing any earlier mention of the same file
    /// and dropping the oldest entry when the list is full.
    ///
    /// An empty path is ignored: "save" on a project that has never been saved
    /// goes through Save As, and a moment of it being empty is not a project
    /// worth remembering.
    void add(const QString& path);

    /// Drops `path`. Called when opening it failed, which is the only evidence
    /// that an entry is worth forgetting.
    void remove(const QString& path);

    void clear();

private:
    void store() const;

    /// How two paths are compared: absolute, native separators, and on Windows
    /// case-folded - because `D:\work\a.tbsproj` and `d:\Work\A.TBSPROJ` are one
    /// file there and two strings everywhere.
    [[nodiscard]] static QString comparable(const QString& path);

    SettingsStore& m_settings;
    QStringList m_paths;
};

} // namespace torquebus::services
