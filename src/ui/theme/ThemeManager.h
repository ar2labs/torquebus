// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Applies a Theme to the running application: QPalette, style sheet and icon
// tinting, all at once, with no restart. Widgets that need to repaint
// themselves connect to themeChanged().

#pragma once

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

    /// Applies `variant` to QApplication. A no-op when it is already active.
    void applyVariant(ThemeVariant variant);

    /// Switches Dark <-> Light.
    void toggleVariant();

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
    mutable QHash<QString, QIcon> m_iconCache;
    mutable int m_styleSheetBytes{-1};
};

} // namespace torquebus::ui
