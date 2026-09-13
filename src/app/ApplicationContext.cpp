// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "app/ApplicationContext.h"

#include "drivers/api/CanBackendRegistry.h"
#include "plugins/host/PluginApi.h"
#include "services/SettingsStore.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/ThemeManager.h"

#include <QCoreApplication>
#include <QtGlobal>

#include <filesystem>
#include <string>
#include <string_view>

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

    // After the built-ins, so that a plugin registering over one is doing it
    // deliberately and last, rather than winning a race.
    loadPlugins();

    loadNameTables();
}

void ApplicationContext::say(const QString& text, bool isError)
{
    m_startupMessages.push_back(StartupMessage{.text = text, .isError = isError});
}

void ApplicationContext::loadNameTables()
{
    const std::filesystem::path file =
        std::filesystem::path{QCoreApplication::applicationFilePath().toStdWString()}
            .parent_path()
        / "data" / "j1939-names.csv";

    if (const Result result = m_j1939Names.loadFile(file); result.failed()) {
        // A file that is there and wrong is worth a line. A file that is not
        // there is the ordinary case and says nothing.
        const std::string_view message = result.message();
        say(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())), true);
        return;
    }

    if (m_j1939Names.empty()) {
        return;
    }

    say(QStringLiteral("J1939 names: %1 functions, %2 manufacturers from %3")
            .arg(m_j1939Names.functionCount())
            .arg(m_j1939Names.manufacturerCount())
            .arg(QString::fromStdString(m_j1939Names.sourcePath())),
        false);
}

void ApplicationContext::loadPlugins()
{
    plugins::PluginHost host;
    host.backends = &CanBackendRegistry::instance();
    host.nodes = &m_catalog;

    // Kept rather than printed: there is no window yet. These are the lines
    // that explain a backend that will not be in the list, so losing them
    // would leave somebody looking for a hardware fault.
    host.log = [this](std::string_view text, bool isError) {
        say(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())), isError);
    };

    const std::filesystem::path executable{
        QCoreApplication::applicationFilePath().toStdWString()};

    m_plugins.loadFrom(plugins::PluginLoader::directoryFor(executable), host);
}

} // namespace torquebus::app
