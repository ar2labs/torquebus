// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The one colour in the palette the user gets to choose.
//
// A swatch stores a *hue* and nothing else. The saturation and the lightness
// come from the theme that is active when the colour is asked for, so the same
// blue is a luminous one on the dark theme and a deep one on the light theme -
// two colours, one choice. Storing the literal RGB the user clicked would be
// simpler and would put a pale yellow focus ring on a white panel the first
// time somebody picked yellow and then switched themes.
//
// The derived tone is then checked against the surface it will sit on and
// darkened or lightened until it clears the WCAG floors (see Contrast.h). This
// is the part that cannot be done by eye: eight hues times two themes is
// sixteen combinations, and the palette has to work for all of them without
// anybody having looked at each one.
//
// `AccentColor::TorqueBus` is exempt from all of it and returns the theme's own
// accent unchanged. Deriving it would round-trip through HSL and land two units
// away from the hand-picked value - a change nobody asked for, applied to every
// existing installation, to reproduce a colour we already have.

#pragma once

#include "ui/theme/Theme.h"

#include <QColor>
#include <QString>

#include <array>

namespace torquebus::ui {

/// In the order the preferences row shows them.
enum class AccentColor : int { TorqueBus = 0, Red, Orange, Yellow, Green, Blue, Purple, Pink };

/// Every accent, in display order. The row in Preferences is built from this,
/// so adding one here is the only edit a new swatch needs.
[[nodiscard]] std::array<AccentColor, 8> accentColors();

/// The accent's name, translated, for the swatch's tooltip.
[[nodiscard]] QString accentDisplayName(AccentColor accent);

/// The token written to the settings file. Stable across releases: a word, not
/// the enumerator's number, so reordering the row cannot silently repaint
/// everybody's window.
[[nodiscard]] QString toString(AccentColor accent);

[[nodiscard]] AccentColor accentColorFromString(const QString& value,
                                                AccentColor fallback = AccentColor::TorqueBus);

/// The two tones an accent takes: the colour itself and its hover.
struct AccentPair final {
    QColor accent;
    QColor hover;
};

/// What `accent` looks like on `variant`.
[[nodiscard]] AccentPair accentPairFor(AccentColor accent, ThemeVariant variant);

/// Writes that pair into `theme`, which must already be filled for its variant.
void applyAccent(Theme& theme, AccentColor accent);

} // namespace torquebus::ui
