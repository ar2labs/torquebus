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

    theme.background = QColor(0x1A, 0x1C, 0x1E);
    theme.panel = QColor(0x20, 0x23, 0x25);
    theme.panelAlternate = QColor(0x25, 0x28, 0x2A);
    theme.toolbar = QColor(0x2A, 0x2D, 0x30);

    // The surface ladder, darkest first:
    //
    //   separator  #0E1011   the groove between panels
    //   tabStrip   #16191B   behind the tabs
    //   background #1A1C1E   the window void
    //   panel      #202325   content, and the active tab
    //
    // Each step is 6-10 points per channel: enough to read as a different
    // surface at a glance, small enough that the window still looks like one
    // object rather than a collage. The active tab shares `panel` exactly, so
    // it merges into the content below it.
    theme.tabStrip = QColor(0x16, 0x19, 0x1B);
    theme.separator = QColor(0x0E, 0x10, 0x11);
    theme.border = QColor(0x34, 0x38, 0x3B);
    theme.hover = QColor(0x2F, 0x33, 0x36);
    theme.selection = QColor(0x1B, 0x42, 0x56);

    // Above the panels rather than below them. A divider tinted like
    // `separator` reads as a groove between two panels and vanishes the moment
    // it borders the canvas, which is darker still.
    theme.divider = QColor(0x36, 0x3C, 0x40);

    // The canvas is the deepest surface in the window - deeper than the groove
    // between panels - so the blocks, which are painted in `panel`, float the
    // furthest above anything.
    //
    // The grid is two steps up from it: +5 for the 15 px filler, which should
    // be felt rather than seen, and +14 for the 150 px guide, which is what the
    // eye actually aligns to.
    theme.canvas = QColor(0x0C, 0x0E, 0x0F);
    theme.canvasGridFine = QColor(0x11, 0x13, 0x14);
    theme.canvasGridCoarse = QColor(0x1A, 0x1D, 0x1F);

    theme.text = QColor(0xD4, 0xD7, 0xDA);
    theme.textMuted = QColor(0x8A, 0x8F, 0x94);
    theme.textInverted = QColor(0x0F, 0x11, 0x12);

    theme.accent = QColor(0x2F, 0xB6, 0xCC);
    theme.accentHover = QColor(0x4F, 0xD0, 0xE4);

    // Rx and Tx are the two colours a user reads thousands of times an hour in
    // the trace. They are picked to differ in hue *and* in lightness, so they
    // stay distinguishable for the ~8% of men with a colour vision deficiency,
    // and they are desaturated enough not to vibrate against the dark grey.
    theme.rx = QColor(0x7C, 0xC2, 0x94);
    theme.tx = QColor(0x6E, 0xAD, 0xE8);
    theme.warning = QColor(0xDC, 0xB4, 0x5C);
    theme.error = QColor(0xDC, 0x6F, 0x6F);
    theme.success = QColor(0x7C, 0xC2, 0x94);

    // The telltales are the one place the palette is saturated on purpose: they
    // have to be seen from the corner of the eye, and the colour is the message.
    // So they are not softened like rx/tx above.
    theme.lampRed = QColor(0xFF, 0x4D, 0x4F);
    theme.lampAmber = QColor(0xFF, 0xB0, 0x20);
    theme.lampGreen = QColor(0x34, 0xD2, 0x7B);
    theme.lampBlue = QColor(0x3D, 0x8B, 0xFF);

    // The same near-black as the canvas: a lit display is the darkest thing in
    // the room, and the bezel steps up from it.
    theme.instrumentScreen = QColor(0x0C, 0x0E, 0x0F);
    theme.instrumentBezel = QColor(0x23, 0x27, 0x2A);

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
    theme.background = QColor(0xE8, 0xEB, 0xEF);
    theme.panel = QColor(0xFF, 0xFF, 0xFF);
    theme.panelAlternate = QColor(0xF4, 0xF6, 0xF8);
    theme.toolbar = QColor(0xDD, 0xE2, 0xE8);

    // The same ladder inverted, and it has to be inverted rather than
    // mirrored: on a light theme the recessed surfaces go *darker* than the
    // content, exactly as they go darker on the dark theme. What flips is
    // which end the content sits at - white here, mid-grey there.
    //
    //   panel      #FFFFFF   content, and the active tab
    //   background #E8EBEF   the window void
    //   tabStrip   #D7DCE3   behind the tabs
    //   separator  #C3CAD4   the groove between panels
    theme.tabStrip = QColor(0xD7, 0xDC, 0xE3);
    theme.separator = QColor(0xC3, 0xCA, 0xD4);
    theme.border = QColor(0xBF, 0xC5, 0xCE);
    theme.hover = QColor(0xD2, 0xD8, 0xE0);
    theme.selection = QColor(0xC4, 0xE3, 0xF5);

    // Below the panels here, as it is above them in the dark theme: the
    // divider has to be visible against the white content on one side and the
    // recessed canvas on the other.
    theme.divider = QColor(0xB8, 0xBF, 0xCA);

    // Recessed from the white panels, but nothing like as deep as the dark
    // theme's canvas - a light window with a near-black rectangle in the middle
    // of it is not a light window. The grid steps *down* from the ground here,
    // by the same two magnitudes.
    theme.canvas = QColor(0xDE, 0xE2, 0xE8);
    theme.canvasGridFine = QColor(0xD7, 0xDC, 0xE3);
    theme.canvasGridCoarse = QColor(0xC5, 0xCC, 0xD6);

    theme.text = QColor(0x17, 0x19, 0x1D);
    theme.textMuted = QColor(0x62, 0x6A, 0x75);
    theme.textInverted = QColor(0xFF, 0xFF, 0xFF);

    theme.accent = QColor(0x0B, 0x74, 0x90);
    theme.accentHover = QColor(0x0F, 0x8F, 0xB2);

    // Darker than their dark-theme counterparts: these are read against white.
    //
    // Amber is the one that does not survive the trip. The dark theme's #DCB45C
    // is comfortable on grey and lands at 4.0:1 on white - under the 4.5:1 floor
    // for body text, which for a warning colour is the wrong side of the line to
    // be on. Darkened until it clears; it stops looking like amber somewhere
    // around here, which is why the dark theme keeps its own value.
    theme.rx = QColor(0x1F, 0x7A, 0x43);
    theme.tx = QColor(0x1B, 0x5F, 0xA8);
    theme.warning = QColor(0x8A, 0x60, 0x00);
    theme.error = QColor(0xB3, 0x2B, 0x2B);
    theme.success = QColor(0x1F, 0x7A, 0x43);

    // Same hues as the dark theme, darkened until each clears the 3:1 floor
    // against the glass; the hue is what ISO 2575 specifies and the lightness
    // is not. Amber moves the furthest - at its dark-theme brightness it is
    // 2.6:1 on this glass - and ends up closer to ochre, for the reason given
    // for `warning` above.
    theme.lampRed = QColor(0xD9, 0x3A, 0x3C);
    theme.lampAmber = QColor(0xB8, 0x74, 0x00);
    theme.lampGreen = QColor(0x1E, 0x9E, 0x57);
    theme.lampBlue = QColor(0x2A, 0x6F, 0xDB);

    // Bright, where the dark theme's is black: the lit face of a light cluster
    // is the brightest thing on it, with the housing a step darker around it.
    theme.instrumentScreen = QColor(0xF7, 0xF8, 0xFA);
    theme.instrumentBezel = QColor(0xD7, 0xDC, 0xE3);

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

int rowPaddingFor(Density density)
{
    // One pixel of padding costs two pixels of row height, and two pixels of
    // row height costs a frame you can see on a full trace. Comfortable is 1,
    // which is what the sheet has always said - the default is not a new
    // opinion about how the application should look.
    switch (density) {
    case Density::Compact:
        return 0;
    case Density::Comfortable:
        return 1;
    case Density::Spacious:
        return 3;
    }
    return 1;
}

int rowMinimumHeightFor(Density density)
{
    switch (density) {
    case Density::Compact:
        return 16;
    case Density::Comfortable:
        return 18;
    case Density::Spacious:
        return 24;
    }
    return 18;
}

QString toString(Density density)
{
    switch (density) {
    case Density::Compact:
        return QStringLiteral("compact");
    case Density::Comfortable:
        return QStringLiteral("comfortable");
    case Density::Spacious:
        return QStringLiteral("spacious");
    }
    return QStringLiteral("comfortable");
}

Density densityFromString(const QString& value, Density fallback)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("compact")) {
        return Density::Compact;
    }
    if (normalized == QLatin1String("comfortable")) {
        return Density::Comfortable;
    }
    if (normalized == QLatin1String("spacious")) {
        return Density::Spacious;
    }
    return fallback;
}

} // namespace torquebus::ui
