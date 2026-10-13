// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How the application shows a kind of block: its icon, its colour, the short
// word on its badge and the line under its name.
//
// The Pipeline canvas draws blocks, the palette lists them and the Properties
// panel titles them, and a block that has one icon in the palette and another in
// the panel that edits it reads as two different blocks. So the mapping is here,
// once, keyed by the catalog's type name - the one thing that never changes.

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "ui/theme/Theme.h"

#include <QColor>
#include <QString>

#include <string_view>

namespace torquebus::ui {

/// The icon a palette group is shown with.
[[nodiscard]] QString iconNameForCategory(const QString& category);

/// The icon a kind of block is shown with, wherever it is shown.
[[nodiscard]] QString iconNameForNodeType(const QString& typeName);

/// "LUA ECU", "J1939", "CAN BUS": what a block is, in the least space that says it.
[[nodiscard]] QString shortTypeBadge(std::string_view typeName);

/// The colour of a kind of block's accent: the stripe on a chip, the tint of a
/// card's header. Blocks of one family share one, so the canvas reads by colour
/// before it reads by text.
[[nodiscard]] QColor accentForNodeType(std::string_view typeName, const Theme& theme);

/// An accent as text or as an icon on the surface of a theme: lightened on the dark theme, where a
/// block's accent is the dark colour a block is edged in, and darkened on the light one, where the
/// same lightening would be a pastel on white that nobody can read.
[[nodiscard]] QColor accentOnSurface(const QColor& accent, const Theme& theme, int amount = 135);

/// Blocks whose behaviour is a script, and so get an Edit button.
[[nodiscard]] bool isScriptNodeType(std::string_view typeName) noexcept;

/// The line under a block's name: which channel, which address, which file -
/// whatever tells two blocks of the same kind apart. Empty when there is nothing
/// to add.
[[nodiscard]] QString nodeSubtitle(const NodeDescription& node);

} // namespace torquebus::ui
