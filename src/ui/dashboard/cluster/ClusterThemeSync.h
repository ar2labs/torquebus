// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Keeps the instrument cluster's QML colours and fonts in step with the application's Theme.
//
// The cluster has no palette of its own. cluster/common/ClusterTheme.qml declares one property
// per role of Theme that the cluster uses, with the same name, and derives the rest from them;
// this fills those properties. Whoever hosts the cluster calls it once for the theme in force
// and again whenever the theme changes - the user's accent choice included, since that is part
// of the Theme it is given.
//
// QML cannot see Theme, and a colour copied by hand into a .qml file is a colour that stops
// following the application the day somebody changes the theme. This is the one place that
// knows which roles the cluster mirrors, and ClusterThemeSyncTests keeps it and the QML file
// from drifting apart.

#pragma once

#include <QObject>
#include <QStringList>

namespace torquebus::ui {

struct Theme;

/// Writes the roles of `theme` that ClusterTheme.qml mirrors into `qmlTheme`, an instance of
/// that type, and the application's fonts: the interface font for the cluster's text, the
/// monospaced font of the trace and the editors for its readouts.
///
/// Returns false, and says which property is missing, if `qmlTheme` does not have every one
/// of them - the QML file and this function have drifted. What is present is still written.
bool syncClusterTheme(QObject& qmlTheme, const Theme& theme);

/// The names of the colour properties syncClusterTheme writes. For the test that every
/// writable colour property of ClusterTheme.qml has a source.
[[nodiscard]] QStringList clusterThemeColorProperties();

} // namespace torquebus::ui
