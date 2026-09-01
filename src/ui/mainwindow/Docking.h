// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The one and only file in TorqueBus that includes KDDockWidgets.
//
// Every panel, the main window and the workspace code speak the small vocabulary
// declared here. If the docking library is ever replaced - or if a KDDockWidgets
// upgrade moves its headers around - this file is the entire blast radius.

#pragma once

#include <kddockwidgets/Config.h>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>

#include <QByteArray>
#include <QIcon>
#include <QStringList>
#include <QSize>
#include <QString>

namespace torquebus::ui {

/// Base class of the application's main window.
using DockMainWindowBase = KDDockWidgets::QtWidgets::MainWindow;

/// A dockable panel host.
using DockWidget = KDDockWidgets::QtWidgets::DockWidget;

/// Where a panel is placed relative to the main window or to another panel.
enum class DockLocation {
    Left,
    Right,
    Top,
    Bottom
};

/// Configures the docking framework. Must be called once, after QApplication
/// is constructed and before the main window is created.
void configureDockingSystem();

/// Wraps `content` in a dock widget.
///
/// `uniqueName` is persisted inside saved layouts and must never change once
/// released - it is the key LayoutSaver uses to find the panel again. `title`
/// is what the user sees and may be translated freely.
[[nodiscard]] DockWidget* createDockWidget(const QString& uniqueName,
                                           const QString& title,
                                           QWidget* content,
                                           const QIcon& icon = {});

/// Replaces a dock's icon, in every place the framework shows one (tab, title
/// bar, side bar). Needed because icons are tinted per theme and a dock keeps
/// whichever one it was given at construction.
void setDockIcon(DockWidget* dock, const QIcon& icon);

/// Describes the actual widget tree inside a dock, one line per widget.
///
/// Exists because styling KDDockWidgets from the outside has been guesswork:
/// the style sheet addresses these widgets by class name, and a Qt style sheet
/// matches on metaObject()->className() - which, for a class that does not
/// declare Q_OBJECT, is the name of its nearest ancestor that does. A selector
/// written against the class you can see in the headers then silently matches
/// nothing, and the rule looks ignored rather than wrong.
///
/// Rather than guess a fourth time, this reports what is really there: the
/// class name Qt will match on, the object name, the geometry, and the palette
/// colours each widget is actually painting with.
[[nodiscard]] QStringList describeDockChrome(DockWidget* dock);

/// Places `dock` inside `window` at `location`.
///
/// `initialSize` requests a starting width (for Left/Right) or height (for
/// Top/Bottom); a zero component means "let the layout decide". The size is a
/// starting point, not a constraint - the user resizes freely afterwards, and
/// a restored layout overrides it entirely.
///
/// Location is relative to the window's layout as a whole: the first dock
/// added occupies all of it, and each later one splits a side off what is
/// already there. The central panel therefore has to be added first.
void addDockTo(DockMainWindowBase* window,
               DockWidget* dock,
               DockLocation location,
               QSize initialSize = {});

/// Serialises the current arrangement of every panel, including floating
/// windows and panels the user moved to a second monitor.
[[nodiscard]] QByteArray saveDockLayout();

/// Restores an arrangement previously produced by saveDockLayout().
/// Returns false when the data is empty, corrupt or from an incompatible
/// version - the caller then falls back to the built-in default layout.
[[nodiscard]] bool restoreDockLayout(const QByteArray& serialized);

/// Translates a TorqueBus location into the framework's own enumeration.
[[nodiscard]] KDDockWidgets::Location toKddwLocation(DockLocation location);

} // namespace torquebus::ui
