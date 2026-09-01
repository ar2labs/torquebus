// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/Theme.h"

namespace torquebus::ui {

Theme Theme::dark()
{
    // Neutral engineering greys, arranged as a deliberate elevation ladder:
    //
    //   background (deepest)  <  panel  <  panelAlternate  <  toolbar (highest)
    //
    // Two corrections, in order, got this to where it is:
    //
    //   1. The first version kept every step to four points of luminance,
    //      betting that low contrast is restful over a long session. It read
    //      as one flat sheet: nothing separated chrome from content, so the eye
    //      had no structure to land on, and "restful" became "hard to parse".
    //   2. Widening the range fixed the structure but the greys were carrying
    //      a visible blue cast - the blue channel sat four to nine points above
    //      the red one, which at these luminances tints the whole window.
    //
    // What is here now keeps the wide range and drops the cast: blue leads red
    // by two or three points, enough to keep the greys from going muddy under
    // a warm monitor, not enough to read as blue. That neutrality is most of
    // what makes a dark editor look calm rather than tinted.
    //
    // Text is deliberately not white. #D4D7DA against #202325 is about 11:1 -
    // far past the accessibility floor - while pure white would be 16:1 and
    // buzz against the background on any decent panel.

    Theme theme;
    theme.variant = ThemeVariant::Dark;
    theme.name = QStringLiteral("TorqueBus Dark");

    theme.background     = QColor(0x1A, 0x1C, 0x1E);
    theme.panel          = QColor(0x20, 0x23, 0x25);
    theme.panelAlternate = QColor(0x25, 0x28, 0x2A);
    theme.toolbar        = QColor(0x2A, 0x2D, 0x30);
    theme.border         = QColor(0x34, 0x38, 0x3B);
    theme.hover          = QColor(0x2F, 0x33, 0x36);
    theme.selection      = QColor(0x1B, 0x42, 0x56);

    theme.text           = QColor(0xD4, 0xD7, 0xDA);
    theme.textMuted      = QColor(0x8A, 0x8F, 0x94);
    theme.textInverted   = QColor(0x0F, 0x11, 0x12);

    theme.accent         = QColor(0x2F, 0xB6, 0xCC);
    theme.accentHover    = QColor(0x4F, 0xD0, 0xE4);

    // Rx and Tx are the two colours a user reads thousands of times an hour in
    // the trace. They are picked to differ in hue *and* in lightness, so they
    // stay distinguishable for the ~8% of men with a colour vision deficiency,
    // and they are desaturated enough not to vibrate against the dark grey.
    theme.rx             = QColor(0x7C, 0xC2, 0x94);
    theme.tx             = QColor(0x6E, 0xAD, 0xE8);
    theme.warning        = QColor(0xDC, 0xB4, 0x5C);
    theme.error          = QColor(0xDC, 0x6F, 0x6F);
    theme.success        = QColor(0x7C, 0xC2, 0x94);

    return theme;
}

Theme Theme::light()
{
    Theme theme;
    theme.variant = ThemeVariant::Light;
    theme.name = QStringLiteral("TorqueBus Light");

    // The same ladder, inverted: the panel is the brightest surface because
    // that is where the data lives, and the chrome recedes behind it. Widened
    // to match the dark theme, so the two variants have the same structural
    // legibility rather than one being an afterthought.
    theme.background     = QColor(0xE8, 0xEB, 0xEF);
    theme.panel          = QColor(0xFF, 0xFF, 0xFF);
    theme.panelAlternate = QColor(0xF4, 0xF6, 0xF8);
    theme.toolbar        = QColor(0xDD, 0xE2, 0xE8);
    theme.border         = QColor(0xBF, 0xC5, 0xCE);
    theme.hover          = QColor(0xD2, 0xD8, 0xE0);
    theme.selection      = QColor(0xC4, 0xE3, 0xF5);

    theme.text           = QColor(0x17, 0x19, 0x1D);
    theme.textMuted      = QColor(0x62, 0x6A, 0x75);
    theme.textInverted   = QColor(0xFF, 0xFF, 0xFF);

    theme.accent         = QColor(0x0B, 0x74, 0x90);
    theme.accentHover    = QColor(0x0F, 0x8F, 0xB2);

    // Darker than their dark-theme counterparts: these are read against white.
    //
    // Amber is the one that does not survive the trip. The dark theme's #DCB45C
    // is comfortable on grey and lands at 4.0:1 on white - under the 4.5:1 floor
    // for body text, which for a warning colour is the wrong side of the line to
    // be on. Darkened until it clears; it stops looking like amber somewhere
    // around here, which is why the dark theme keeps its own value.
    theme.rx             = QColor(0x1F, 0x7A, 0x43);
    theme.tx             = QColor(0x1B, 0x5F, 0xA8);
    theme.warning        = QColor(0x8A, 0x60, 0x00);
    theme.error          = QColor(0xB3, 0x2B, 0x2B);
    theme.success        = QColor(0x1F, 0x7A, 0x43);

    return theme;
}

Theme Theme::forVariant(ThemeVariant variant)
{
    return variant == ThemeVariant::Light ? light() : dark();
}

QString toString(ThemeVariant variant)
{
    return variant == ThemeVariant::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

ThemeVariant themeVariantFromString(const QString& value, ThemeVariant fallback)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("dark")) {
        return ThemeVariant::Dark;
    }
    if (normalized == QLatin1String("light")) {
        return ThemeVariant::Light;
    }
    return fallback;
}

} // namespace torquebus::ui
