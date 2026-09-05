// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "app/ApplicationContext.h"

#include "drivers/api/CanBackendRegistry.h"
#include "services/SettingsStore.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/ThemeManager.h"

#include <QtGlobal>

namespace torquebus::app {

ApplicationContext::ApplicationContext()
    : m_settings{std::make_unique<services::SettingsStore>()}
    , m_themes{std::make_unique<ui::ThemeManager>()}
{
}

ApplicationContext::~ApplicationContext() = default;

void ApplicationContext::initialize()
{
    if (!m_settings->load()) {
        // Not fatal: load() falls back to defaults for a missing or corrupt
        // file, and the application must still start. Worth saying out loud
        // though - a settings file that silently stops being read is how a
        // user loses their layout twice before noticing.
        qWarning("TorqueBus: could not read %s; starting from defaults.",
                 qPrintable(m_settings->filePath()));
    }

    const ui::ThemeVariant variant = ui::themeVariantFromString(
        m_settings->value(QString::fromLatin1(services::keys::kTheme)),
        ui::ThemeVariant::Dark);

    const ui::AccentColor accent = ui::accentColorFromString(
        m_settings->value(QString::fromLatin1(services::keys::kAccent)));

    const ui::Density density = ui::densityFromString(
        m_settings->value(QString::fromLatin1(services::keys::kDensity)));

    // A first run follows the desktop; an installation that already has a theme
    // written down does not.
    //
    // The default is "nobody has ever chosen", not simply "true". Turning this
    // on for everybody would repaint the window of every existing user who had
    // deliberately picked the dark theme on a light desktop - which is the one
    // group whose preference we can actually see, and the one we would be
    // overruling.
    const bool followSystem = m_settings->boolValue(
        QString::fromLatin1(services::keys::kFollowSystemTheme),
        !m_settings->contains(QString::fromLatin1(services::keys::kTheme)));

    m_themes->applyPreferences(accent, density, followSystem, variant);

    // Backends announce themselves once, here, so that any window - including
    // a test harness window - sees the same set of interfaces.
    CanBackendRegistry::instance().registerBuiltins();
}

} // namespace torquebus::app
