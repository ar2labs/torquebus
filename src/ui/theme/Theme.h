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

enum class ThemeVariant { Dark, Light };

/// A complete palette. Values are filled by Theme::dark() / Theme::light().
struct Theme final {
    ThemeVariant variant{ThemeVariant::Dark};
    QString name;

    // --- Surfaces ---------------------------------------------------------
    QColor background; ///< The window itself, behind every panel.
    QColor panel; ///< Dock widget and view background.
    QColor panelAlternate; ///< Alternating row background in tables.
    QColor toolbar; ///< Toolbars and the menu bar.

    /// The strip a panel's tabs sit on, behind the tabs themselves.
    ///
    /// Deliberately *darker* than `panel`, which is the opposite of what a
    /// naive reading suggests. The active tab is painted in `panel`, so it
    /// merges into the content below it and reads as attached to it, while the
    /// inactive tabs stay recessed on the darker strip. This is how VS Code,
    /// Visual Studio and every editor with a tab strip do it, and it is what
    /// makes a tab look like a tab rather than a button in a row of buttons.
    QColor tabStrip;

    QColor border; ///< Panel outlines and grid lines.

    /// The draggable gap between two panels.
    ///
    /// Darker than the window void, so the gap reads as a groove cut between
    /// panels rather than as more background. Without a distinct colour the
    /// splitters vanish: the panels sit flush and nothing says the edge can be
    /// dragged. Its hover state is the accent, which is the actual affordance.
    QColor separator;

    /// The bar a splitter or a dock separator is painted in.
    ///
    /// Its own role rather than `separator` or `border`, because it has to be
    /// visible against *every* surface it can land between - a panel on one
    /// side and the pipeline canvas on the other, which are the lightest and
    /// darkest things in the window. A groove tinted like the darkest surface
    /// disappears the moment it borders that surface, which is exactly what
    /// happened: the divider inside the Pipeline panel could not be seen at
    /// all.
    ///
    /// So this sits *between* the surfaces rather than below them: lighter
    /// than the panels in the dark theme, darker in the light one.
    QColor divider;

    /// The pipeline canvas, behind the blocks.
    ///
    /// Recessed from the panels, because the blocks are the panels' equivalent
    /// and they have to float above something.
    QColor canvas;

    /// Grid lines on the canvas, at 15 px and 150 px.
    ///
    /// Derived from `canvas` with small deltas, and **coarse is always the
    /// stronger of the two**. Borrowing surface roles here was the original
    /// mistake: `background` and `tabStrip` happen to differ from `separator`
    /// by enough to draw a hard mesh every 15 px, and in the dark theme
    /// `tabStrip` is darker than `background`, so the 150 px guide came out
    /// fainter than the 15 px filler it is supposed to organise.
    QColor canvasGridFine;
    QColor canvasGridCoarse;

    QColor hover;
    QColor selection;

    // --- Text -------------------------------------------------------------
    QColor text;
    QColor textMuted; ///< Secondary labels, disabled items, units.
    QColor textInverted; ///< Text drawn on top of `accent`.

    // --- Identity ---------------------------------------------------------
    QColor accent; ///< TorqueBus blue/cyan. Focus, active tab, links.
    QColor accentHover;

    // --- Semantics --------------------------------------------------------
    QColor rx; ///< Received frames.
    QColor tx; ///< Transmitted frames.
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

/// How much air a row of a table gets.
///
/// Not a colour, and here anyway: it is the other half of what "how the
/// application looks" means, and it is decided in the same place and stored in
/// the same file. TorqueBus is a dense tool by design - the QSS header says so
/// - and Comfortable is that design. Compact is for a laptop screen with a
/// thousand frames on it; Spacious is for reading across a desk, or for anyone
/// who finds 20-pixel rows hard to hit with a mouse.
enum class Density { Compact, Comfortable, Spacious };

/// Vertical padding, in pixels, applied above and below a table row's text.
///
/// Comfortable is 1 px, which is what the style sheet has always had - so the
/// default changes nothing for anybody who never opens Preferences.
[[nodiscard]] int rowPaddingFor(Density density);

/// Floor on a row's height, in pixels. Padding alone does not settle it: below
/// this the row is whatever the font makes it, and the mouse target shrinks
/// with the font rather than with the setting.
[[nodiscard]] int rowMinimumHeightFor(Density density);

[[nodiscard]] QString toString(Density density);
[[nodiscard]] Density densityFromString(const QString& value,
                                        Density fallback = Density::Comfortable);

} // namespace torquebus::ui
