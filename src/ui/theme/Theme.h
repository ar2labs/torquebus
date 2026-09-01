// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The colour vocabulary of the application.
//
// Nothing in TorqueBus hard-codes a colour. Widgets ask the theme for a role -
// "the colour of a received frame", "the colour of a panel border" - and the
// theme decides. That is what makes a second theme a data change rather than a
// grep-and-replace across the UI.

#pragma once

#include <QColor>
#include <QString>

namespace torquebus::ui {

enum class ThemeVariant {
    Dark,
    Light
};

/// A complete palette. Values are filled by Theme::dark() / Theme::light().
struct Theme final {
    ThemeVariant variant{ThemeVariant::Dark};
    QString name;

    // --- Surfaces ---------------------------------------------------------
    QColor background;      ///< The window itself, behind every panel.
    QColor panel;           ///< Dock widget and view background.
    QColor panelAlternate;  ///< Alternating row background in tables.
    QColor toolbar;         ///< Toolbars and the menu bar.

    /// The strip a panel's tabs sit on, behind the tabs themselves.
    ///
    /// Deliberately *darker* than `panel`, which is the opposite of what a
    /// naive reading suggests. The active tab is painted in `panel`, so it
    /// merges into the content below it and reads as attached to it, while the
    /// inactive tabs stay recessed on the darker strip. This is how VS Code,
    /// Visual Studio and every editor with a tab strip do it, and it is what
    /// makes a tab look like a tab rather than a button in a row of buttons.
    QColor tabStrip;

    QColor border;          ///< Panel outlines and grid lines.

    /// The draggable gap between two panels.
    ///
    /// Darker than the window void, so the gap reads as a groove cut between
    /// panels rather than as more background. Without a distinct colour the
    /// splitters vanish: the panels sit flush and nothing says the edge can be
    /// dragged. Its hover state is the accent, which is the actual affordance.
    QColor separator;
    QColor hover;
    QColor selection;

    // --- Text -------------------------------------------------------------
    QColor text;
    QColor textMuted;       ///< Secondary labels, disabled items, units.
    QColor textInverted;    ///< Text drawn on top of `accent`.

    // --- Identity ---------------------------------------------------------
    QColor accent;          ///< TorqueBus blue/cyan. Focus, active tab, links.
    QColor accentHover;

    // --- Semantics --------------------------------------------------------
    QColor rx;              ///< Received frames.
    QColor tx;              ///< Transmitted frames.
    QColor warning;
    QColor error;
    QColor success;

    /// Colour of a frame row, by direction. Used by the trace and the console.
    [[nodiscard]] QColor directionColor(bool transmitted) const { return transmitted ? tx : rx; }

    [[nodiscard]] static Theme dark();
    [[nodiscard]] static Theme light();
    [[nodiscard]] static Theme forVariant(ThemeVariant variant);
};

[[nodiscard]] QString toString(ThemeVariant variant);
[[nodiscard]] ThemeVariant themeVariantFromString(const QString& value,
                                                  ThemeVariant fallback = ThemeVariant::Dark);

} // namespace torquebus::ui
