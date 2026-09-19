// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Applies a Theme to the running application: QPalette, style sheet and icon
// tinting, all at once, with no restart. Widgets that need to repaint
// themselves connect to themeChanged().

#pragma once

#include "ui/theme/AccentColor.h"
#include "ui/theme/Theme.h"

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QString>

namespace torquebus::ui {

class ThemeManager final : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override;

    /// The manager owned by the application. Set once at startup by
    /// ApplicationContext; never null while the UI is alive.
    [[nodiscard]] static ThemeManager* instance();

    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }
    [[nodiscard]] ThemeVariant variant() const noexcept { return m_theme.variant; }

    /// Applies `variant` to QApplication, as an explicit choice by the user.
    ///
    /// Explicit is the operative word: this stops the application following the
    /// desktop's light/dark setting. Somebody who picks Dark while their
    /// desktop is Light has said something, and having the window flip back the
    /// next time Windows changes its mind would be the application overruling
    /// them. Use setFollowSystemTheme() to hand the decision back.
    void applyVariant(ThemeVariant variant);

    /// Switches Dark <-> Light. Also an explicit choice.
    void toggleVariant();

    // --- Accent -----------------------------------------------------------

    [[nodiscard]] AccentColor accent() const noexcept { return m_accent; }

    /// Re-derives the palette around a different accent and repaints.
    ///
    /// The variant is untouched: an accent is a hue, and which theme it is
    /// rendered against is a separate question (see AccentColor.h).
    void setAccent(AccentColor accent);

    // --- Density ----------------------------------------------------------

    [[nodiscard]] Density density() const noexcept { return m_density; }

    /// How much air a table row gets. Reapplies the style sheet.
    void setDensity(Density density);

    // --- Following the desktop --------------------------------------------

    [[nodiscard]] bool followsSystemTheme() const noexcept { return m_followSystemTheme; }

    /// Hands the light/dark decision to the desktop, or takes it back.
    ///
    /// Turning it on applies the desktop's current setting immediately, so the
    /// checkbox and the window agree without waiting for Windows to change.
    void setFollowSystemTheme(bool follow);

    /// Applies accent, follow-the-desktop and variant in one repaint.
    ///
    /// Startup's single entry point. Calling the three setters in a row would
    /// work and would parse and apply the style sheet up to three times before
    /// the first window is shown, for a result the user can only see once.
    ///
    /// `variant` is used only when `followSystem` is false.
    void
    applyPreferences(AccentColor accent, Density density, bool followSystem, ThemeVariant variant);

    /// What the desktop is asking for right now.
    ///
    /// A desktop that will not say - Qt reports Unknown on platforms with no
    /// such setting - is read as Dark, which is the house default rather than a
    /// guess about the user.
    [[nodiscard]] static ThemeVariant systemVariant();

    /// Loads an SVG icon from the resource system and recolours it for the
    /// current theme, so a single monochrome icon set serves both themes.
    ///
    /// `name` is the resource stem, e.g. "start" for ":/icons/start.svg".
    /// Results are cached and the cache is cleared on every theme change.
    [[nodiscard]] QIcon icon(const QString& name) const;

    /// Same, but forces a specific colour - used for semantic toolbar buttons
    /// such as the red Record dot.
    [[nodiscard]] QIcon icon(const QString& name, const QColor& color) const;

    /// Size in bytes of the style sheet last loaded from the resource system,
    /// or -1 if it could not be loaded at all.
    ///
    /// Exists because "the theme did not change" and "the theme changed but the
    /// binary still has the old resource compiled into it" look identical on
    /// screen. This number tells the two apart, so a styling problem can be
    /// diagnosed from the Output panel instead of by guesswork.
    [[nodiscard]] int styleSheetBytes() const noexcept { return m_styleSheetBytes; }

    /// True when a named icon is present in the resource system. Used at
    /// startup to report an icon set that did not make it into the build.
    [[nodiscard]] static bool hasIconResource(const QString& name);

Q_SIGNALS:
    void themeChanged(const torquebus::ui::Theme& theme);

private:
    /// Rebuilds the palette for `variant` with the current accent and repaints.
    ///
    /// Separate from applyVariant() because that one also means "the user has
    /// chosen": this is the half that only changes colours, and it is what the
    /// desktop's own notifications come through.
    void rebuild(ThemeVariant variant);

    void applyFont() const;
    void applyPalette() const;
    void applyStyleSheet() const;
    [[nodiscard]] QString buildStyleSheet() const;

    /// Warns about any '@token' the substitution list missed.
    ///
    /// Qt discards a declaration it cannot parse without saying so, so an
    /// unregistered token is a rule that silently does nothing.
    static void reportUnsubstitutedTokens(const QString& sheet);

    Theme m_theme;
    AccentColor m_accent{AccentColor::TorqueBus};
    Density m_density{Density::Comfortable};
    bool m_followSystemTheme{false};

    mutable QHash<QString, QIcon> m_iconCache;
    mutable int m_styleSheetBytes{-1};
};

} // namespace torquebus::ui
