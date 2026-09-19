// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/AccentColor.h"

#include "ui/theme/Contrast.h"

#include <QCoreApplication>

#include <algorithm>

namespace torquebus::ui {
namespace {

/// Hue in degrees, which is the whole of what a swatch stores.
[[nodiscard]] double hueFor(AccentColor accent)
{
    switch (accent) {
    case AccentColor::TorqueBus:
        return 189.0;
    case AccentColor::Red:
        return 4.0;
    case AccentColor::Orange:
        return 28.0;
    case AccentColor::Yellow:
        return 48.0;
    case AccentColor::Green:
        return 145.0;
    case AccentColor::Blue:
        return 210.0;
    case AccentColor::Purple:
        return 268.0;
    case AccentColor::Pink:
        return 322.0;
    }
    return 189.0;
}

/// Which way lightness has to move to gain contrast against this theme.
///
/// Away from the panel, in both cases: on the dark theme the panel is nearly
/// black and the accent has to climb, on the light theme it is white and the
/// accent has to fall.
[[nodiscard]] double lightnessDirection(ThemeVariant variant)
{
    return variant == ThemeVariant::Dark ? +1.0 : -1.0;
}

/// One step of the search, small enough that the result is not visibly
/// quantised and large enough that the loop is short.
constexpr double kLightnessStep = 0.01;

/// Guard rails. A hue that has not cleared the floor by here never will - the
/// loop would walk into white or black, which are not accents.
constexpr double kMinimumLightness = 0.05;
constexpr double kMaximumLightness = 0.95;

} // namespace

std::array<AccentColor, 8> accentColors()
{
    return {AccentColor::TorqueBus,
            AccentColor::Red,
            AccentColor::Orange,
            AccentColor::Yellow,
            AccentColor::Green,
            AccentColor::Blue,
            AccentColor::Purple,
            AccentColor::Pink};
}

QString accentDisplayName(AccentColor accent)
{
    switch (accent) {
    case AccentColor::TorqueBus:
        return QCoreApplication::translate("AccentColor", "TorqueBus");
    case AccentColor::Red:
        return QCoreApplication::translate("AccentColor", "Red");
    case AccentColor::Orange:
        return QCoreApplication::translate("AccentColor", "Orange");
    case AccentColor::Yellow:
        return QCoreApplication::translate("AccentColor", "Yellow");
    case AccentColor::Green:
        return QCoreApplication::translate("AccentColor", "Green");
    case AccentColor::Blue:
        return QCoreApplication::translate("AccentColor", "Blue");
    case AccentColor::Purple:
        return QCoreApplication::translate("AccentColor", "Purple");
    case AccentColor::Pink:
        return QCoreApplication::translate("AccentColor", "Pink");
    }
    return QCoreApplication::translate("AccentColor", "TorqueBus");
}

QString toString(AccentColor accent)
{
    switch (accent) {
    case AccentColor::TorqueBus:
        return QStringLiteral("torquebus");
    case AccentColor::Red:
        return QStringLiteral("red");
    case AccentColor::Orange:
        return QStringLiteral("orange");
    case AccentColor::Yellow:
        return QStringLiteral("yellow");
    case AccentColor::Green:
        return QStringLiteral("green");
    case AccentColor::Blue:
        return QStringLiteral("blue");
    case AccentColor::Purple:
        return QStringLiteral("purple");
    case AccentColor::Pink:
        return QStringLiteral("pink");
    }
    return QStringLiteral("torquebus");
}

AccentColor accentColorFromString(const QString& value, AccentColor fallback)
{
    const QString token = value.trimmed().toLower();

    for (const AccentColor accent : accentColors()) {
        if (toString(accent) == token) {
            return accent;
        }
    }

    return fallback;
}

AccentPair accentPairFor(AccentColor accent, ThemeVariant variant)
{
    const Theme reference = Theme::forVariant(variant);

    // The house accent is the one colour here that was chosen by a person. It
    // is returned as it was written rather than reconstructed.
    if (accent == AccentColor::TorqueBus) {
        return {reference.accent, reference.accentHover};
    }

    // Converted to HSL explicitly rather than reading the components off a
    // colour that is stored as RGB. Qt will convert on the way, but "which spec
    // is this QColor in" is the kind of question a reader should not have to
    // hold in their head to trust the next four lines.
    const QColor referenceAccent = reference.accent.toHsl();
    const QColor referenceHover = reference.accentHover.toHsl();

    // Saturation and lightness are borrowed from the accent this theme already
    // had, so a chosen hue arrives at the same weight as the one it replaces
    // rather than at whatever weight the swatch happened to be drawn in.
    const double saturation = referenceAccent.hslSaturationF();
    const double startLightness = referenceAccent.lightnessF();

    // The hover is a fixed distance from the accent, and that distance is taken
    // from the theme too: the dark theme lifts its hover further than the light
    // theme does, and that difference is part of how each one feels.
    const double hoverDelta = referenceHover.lightnessF() - startLightness;

    const double hue = hueFor(accent) / 360.0;
    const double direction = lightnessDirection(variant);

    double lightness = startLightness;
    QColor color = QColor::fromHslF(hue, saturation, lightness);

    // Walk away from the panel until the colour is both visible on it and
    // readable under the label that will be drawn on top of it. A hue that
    // starts out compliant - most of them do - never enters the loop.
    while (lightness > kMinimumLightness && lightness < kMaximumLightness) {
        const bool visible = contrastRatio(color, reference.panel) >= kMinimumUiContrast;
        const bool readable = contrastRatio(color, reference.textInverted) >= kMinimumTextContrast;

        if (visible && readable) {
            break;
        }

        lightness += direction * kLightnessStep;
        color = QColor::fromHslF(hue, saturation, std::clamp(lightness, 0.0, 1.0));
    }

    const QColor hover =
        QColor::fromHslF(hue, saturation, std::clamp(lightness + hoverDelta, 0.0, 1.0));

    return {color, hover};
}

void applyAccent(Theme& theme, AccentColor accent)
{
    const AccentPair pair = accentPairFor(accent, theme.variant);

    theme.accent = pair.accent;
    theme.accentHover = pair.hover;
}

} // namespace torquebus::ui
