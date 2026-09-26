// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The accent is the only colour in the application a user can choose, so it is
// the only one nobody looked at before it went on screen. Eight hues times two
// themes is sixteen combinations; these tests are what stands in for sixteen
// pairs of eyes.

#include "ui/theme/AccentColor.h"
#include "ui/theme/Contrast.h"
#include "ui/theme/Theme.h"

#include <gtest/gtest.h>

using torquebus::ui::AccentColor;
using torquebus::ui::accentColorFromString;
using torquebus::ui::accentColors;
using torquebus::ui::AccentPair;
using torquebus::ui::accentPairFor;
using torquebus::ui::applyAccent;
using torquebus::ui::contrastRatio;
using torquebus::ui::kMinimumTextContrast;
using torquebus::ui::kMinimumUiContrast;
using torquebus::ui::relativeLuminance;
using torquebus::ui::Theme;
using torquebus::ui::ThemeVariant;
using torquebus::ui::toString;

TEST(AccentColorTests, BlackAgainstWhiteIsTheWidestContrastThereIs)
{
    // The two ends of the WCAG scale, so a wrong transfer function or a wrong
    // set of luminance weights shows up here before it shows up as a palette
    // that passes its own broken check.
    ASSERT_TRUE(contrastRatio(QColor{Qt::black}, QColor{Qt::white}) > 20.9);
    ASSERT_TRUE(contrastRatio(QColor{Qt::black}, QColor{Qt::white}) < 21.1);

    ASSERT_TRUE(contrastRatio(QColor{Qt::black}, QColor{Qt::black}) == 1.0);

    // Symmetric: the ratio is a property of the pair, not of the order.
    const QColor teal{0x2F, 0xB6, 0xCC};
    ASSERT_TRUE(contrastRatio(teal, QColor{Qt::white}) == contrastRatio(QColor{Qt::white}, teal));
}

TEST(AccentColorTests, LuminanceIsWeightedNotAnAverage)
{
    // Green carries most of the perceived brightness and blue almost none. A
    // naive (r+g+b)/3 would rank these three the same; getting this wrong would
    // make every yellow accent pass a check it should fail.
    ASSERT_TRUE(relativeLuminance(QColor{Qt::green}) > relativeLuminance(QColor{Qt::red}));
    ASSERT_TRUE(relativeLuminance(QColor{Qt::red}) > relativeLuminance(QColor{Qt::blue}));
}

TEST(AccentColorTests, TheHouseAccentIsReturnedExactlyAsItWasWritten)
{
    // Not "close enough". Deriving this one would round-trip it through HSL and
    // land a unit or two away, which would repaint every existing installation
    // to reproduce a colour we already have. Everyone who never opens
    // Preferences must see the window they saw yesterday.
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        const Theme theme = Theme::forVariant(variant);
        const AccentPair pair = accentPairFor(AccentColor::TorqueBus, variant);

        ASSERT_TRUE(pair.accent == theme.accent);
        ASSERT_TRUE(pair.hover == theme.accentHover);
    }
}

TEST(AccentColorTests, EveryAccentIsVisibleOnItsPanelAndReadableUnderItsLabel)
{
    // The reason this module exists. The accent is the active tab's background
    // and the focus ring's colour: it has to be seen against the panel (WCAG
    // 1.4.11) and the label drawn on top of it has to be readable (1.4.3 AA).
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        const Theme theme = Theme::forVariant(variant);

        for (const AccentColor accent : accentColors()) {
            const AccentPair pair = accentPairFor(accent, variant);

            const double visible = contrastRatio(pair.accent, theme.panel);
            const double readable = contrastRatio(pair.accent, theme.textInverted);

            SCOPED_TRACE(::testing::Message()
                         << "accent " << toString(accent).toStdString() << " on "
                         << theme.name.toStdString() << " is " << pair.accent.name().toStdString());

            ASSERT_TRUE(visible >= kMinimumUiContrast);
            ASSERT_TRUE(readable >= kMinimumTextContrast);
        }
    }
}

TEST(AccentColorTests, TheSameChoiceIsADifferentColourOnEachTheme)
{
    // The whole point of storing a hue rather than an RGB triple. A blue picked
    // on the dark theme must not follow the user to the light theme as the same
    // luminous blue - there it would be a pale ring on a white panel.
    for (const AccentColor accent : accentColors()) {
        const AccentPair dark = accentPairFor(accent, ThemeVariant::Dark);
        const AccentPair light = accentPairFor(accent, ThemeVariant::Light);

        SCOPED_TRACE(::testing::Message() << "accent " << toString(accent).toStdString());
        ASSERT_TRUE(dark.accent != light.accent);

        // And in the expected direction: brighter where the panel is dark.
        ASSERT_TRUE(relativeLuminance(dark.accent) > relativeLuminance(light.accent));
    }
}

TEST(AccentColorTests, HoverIsALiftAwayFromTheAccentNeverAJump)
{
    // Both themes lift their hover; the dark one lifts it further. A hover that
    // came out darker than the thing it highlights would read as a press.
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        for (const AccentColor accent : accentColors()) {
            const AccentPair pair = accentPairFor(accent, variant);

            SCOPED_TRACE(::testing::Message() << "accent " << toString(accent).toStdString());
            ASSERT_TRUE(relativeLuminance(pair.hover) > relativeLuminance(pair.accent));
        }
    }
}

TEST(AccentColorTests, AnAccentSurvivesATripThroughTheSettingsFile)
{
    for (const AccentColor accent : accentColors()) {
        ASSERT_TRUE(accentColorFromString(toString(accent)) == accent);
    }

    // Case and whitespace are the shapes a hand-edited settings file arrives
    // in. This store is JSON precisely so people can edit it.
    ASSERT_TRUE(accentColorFromString(QStringLiteral("  Blue  ")) == AccentColor::Blue);

    // A word we do not know is not an error worth stopping for. The window gets
    // its usual colour and the user gets to keep working.
    ASSERT_TRUE(accentColorFromString(QStringLiteral("chartreuse")) == AccentColor::TorqueBus);
    ASSERT_TRUE(accentColorFromString(QStringLiteral(""), AccentColor::Pink) == AccentColor::Pink);
}

TEST(AccentColorTests, TokensAreWordsSoTheRowCanBeReordered)
{
    // Storing the enumerator's number would mean that inserting a swatch in the
    // middle of the row silently repaints everybody's window.
    ASSERT_TRUE(toString(AccentColor::TorqueBus) == QStringLiteral("torquebus"));
    ASSERT_TRUE(toString(AccentColor::Purple) == QStringLiteral("purple"));

    for (const AccentColor accent : accentColors()) {
        ASSERT_FALSE(toString(accent).isEmpty());
    }
}

TEST(AccentColorTests, ApplyingAnAccentTouchesTheAccentAndNothingElse)
{
    Theme theme = Theme::dark();
    const Theme original = Theme::dark();

    applyAccent(theme, AccentColor::Purple);

    ASSERT_TRUE(theme.accent != original.accent);
    ASSERT_TRUE(theme.accentHover != original.accentHover);

    // A user choosing a purple accent is not asking for a different trace, a
    // different canvas or a different idea of what an error looks like.
    ASSERT_TRUE(theme.panel == original.panel);
    ASSERT_TRUE(theme.canvas == original.canvas);
    ASSERT_TRUE(theme.text == original.text);
    ASSERT_TRUE(theme.rx == original.rx);
    ASSERT_TRUE(theme.tx == original.tx);
    ASSERT_TRUE(theme.error == original.error);
    ASSERT_TRUE(theme.variant == original.variant);
}
