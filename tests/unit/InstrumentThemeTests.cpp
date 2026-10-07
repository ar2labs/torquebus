// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The instrument cluster's telltales mean something by their colour, and ISO
// 2575 fixes which. Both themes have to keep the meaning while changing the
// brightness, and nobody looks at the light theme's amber on a light face unless
// something makes them; these tests are that something.

#include "ui/theme/Contrast.h"
#include "ui/theme/Theme.h"

#include <gtest/gtest.h>

#include <QColor>

#include <algorithm>
#include <array>
#include <cstdlib>

using torquebus::ui::contrastRatio;
using torquebus::ui::kMinimumUiContrast;
using torquebus::ui::Theme;
using torquebus::ui::ThemeVariant;

namespace {

struct Lamp {
    const char* name;
    QColor Theme::* role;
    int hue; ///< The ISO 2575 hue, in degrees.
    int tolerance;
};

constexpr std::array<Lamp, 4> kLamps{{
    {"red", &Theme::lampRed, 0, 15},
    {"amber", &Theme::lampAmber, 40, 12},
    {"green", &Theme::lampGreen, 145, 20},
    {"blue", &Theme::lampBlue, 215, 15},
}};

// Hue is a circle: 359 and 1 are two degrees apart, not 358.
int hueDistance(int a, int b)
{
    const int diff = std::abs(a - b) % 360;
    return std::min(diff, 360 - diff);
}

} // namespace

TEST(InstrumentThemeTests, EveryLampIsVisibleOnTheGlass)
{
    // WCAG 1.4.11. A telltale is seen, not read, so it takes the non-text floor.
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        const Theme theme = Theme::forVariant(variant);

        for (const Lamp& lamp : kLamps) {
            SCOPED_TRACE(::testing::Message()
                         << lamp.name << " lamp on " << theme.name.toStdString());

            ASSERT_TRUE(contrastRatio(theme.*lamp.role, theme.instrumentScreen)
                        >= kMinimumUiContrast);
        }
    }
}

TEST(InstrumentThemeTests, EveryLampKeepsItsHueOnBothThemes)
{
    // Darkening a colour until it is visible on white is how a green becomes a
    // teal and an amber becomes a brown. The hue is the part ISO 2575 fixes, so
    // it is the part that must survive the trip.
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        const Theme theme = Theme::forVariant(variant);

        for (const Lamp& lamp : kLamps) {
            SCOPED_TRACE(::testing::Message()
                         << lamp.name << " lamp on " << theme.name.toStdString());

            ASSERT_TRUE(hueDistance((theme.*lamp.role).hslHue(), lamp.hue) <= lamp.tolerance);
        }
    }
}

TEST(InstrumentThemeTests, TheGlassIsTheOppositeEndFromTheWindow)
{
    // A lit display against its bezel: the glass is the deepest surface on the
    // dark theme and the brightest on the light one, never the same tone as the
    // chrome around it.
    const Theme dark = Theme::dark();
    const Theme light = Theme::light();

    ASSERT_TRUE(dark.instrumentScreen.lightness() < dark.instrumentBezel.lightness());
    ASSERT_TRUE(light.instrumentScreen.lightness() > light.instrumentBezel.lightness());
}
