// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How readable one colour is against another, by the WCAG 2.1 definition.
//
// Here because the accent colour is *chosen by the user* and everything else in
// the palette is not. Every other colour in Theme was picked by hand and looked
// at; an accent the user selected from a row of swatches has to be checked by
// the program, because nobody looked at that particular combination before it
// went on screen.
//
// The two thresholds that matter, and what they are for:
//
//   - 3.0 : WCAG 1.4.11, "non-text contrast". A focus ring, the marker under
//           the active tab, the bar in a load column - things you have to be
//           able to *see*, not read.
//   - 4.5 : WCAG 1.4.3 at the AA level, for text. The active tab's label is
//           drawn in `textInverted` on top of the accent, so the accent has to
//           clear this too or the one tab you are looking at is the one you
//           cannot read.

#pragma once

#include <QColor>

namespace torquebus::ui {

/// WCAG relative luminance, in [0, 1].
///
/// Not the same as HSL lightness and not interchangeable with it: this is a
/// weighted, gamma-corrected sum that says how bright a colour actually looks,
/// which is why a yellow and a blue of equal "lightness" are nothing alike.
[[nodiscard]] double relativeLuminance(const QColor& color);

/// WCAG contrast ratio, from 1.0 (identical) to 21.0 (black against white).
///
/// Symmetric - the order of the arguments does not matter.
[[nodiscard]] double contrastRatio(const QColor& first, const QColor& second);

/// WCAG 1.4.11: the floor for something that has to be seen but not read.
inline constexpr double kMinimumUiContrast = 3.0;

/// WCAG 1.4.3 AA: the floor for text.
inline constexpr double kMinimumTextContrast = 4.5;

} // namespace torquebus::ui
