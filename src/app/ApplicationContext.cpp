// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "app/ApplicationContext.h"

#include "drivers/api/CanBackendRegistry.h"
#include "services/SettingsStore.h"
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

    m_themes->applyVariant(variant);

    // Backends announce themselves once, here, so that any window - including
    // a test harness window - sees the same set of interfaces.
    CanBackendRegistry::instance().registerBuiltins();
}

} // namespace torquebus::app
