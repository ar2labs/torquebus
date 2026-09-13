// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Owns the long-lived services and hands them to whoever needs them.
//
// Explicit composition, not singletons: the main window receives references to
// the settings store and the theme manager rather than reaching for globals.
// That is what makes the window constructible inside a test with a temporary
// settings file and no side effects on the developer's own configuration.

#pragma once

#include "core/j1939/J1939NameTables.h"
#include "core/pipeline/NodeCatalog.h"
#include "plugins/host/PluginLoader.h"

#include <QString>

#include <memory>
#include <span>
#include <vector>

namespace torquebus::services {
class SettingsStore;
}

namespace torquebus::ui {
class ThemeManager;
}

namespace torquebus::app {

class ApplicationContext final {
public:
    ApplicationContext();
    ~ApplicationContext();

    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;

    /// Something said while the program was starting, before there was a
    /// window to say it in.
    ///
    /// Kept rather than printed, and these are exactly the lines that must not
    /// be lost: each one explains a backend, a block or a name that is not
    /// going to be where somebody looks for it.
    struct StartupMessage final {
        QString text;
        bool isError{false};
    };

    /// Loads the settings file, applies the persisted theme, and loads plugins.
    /// Call once, after QApplication exists and before creating any window.
    void initialize();

    [[nodiscard]] services::SettingsStore& settings() noexcept { return *m_settings; }
    [[nodiscard]] ui::ThemeManager& themes() noexcept { return *m_themes; }

    /// The node types this run knows: the built-in ones and whatever plugins
    /// added. Owned here rather than by the window so that plugins are loaded
    /// once, and so a second window sees the same catalogue as the first.
    [[nodiscard]] NodeCatalog& catalog() noexcept { return m_catalog; }

    [[nodiscard]] const plugins::PluginLoader& pluginLoader() const noexcept
    {
        return m_plugins;
    }

    /// The words behind the numbers in a J1939 NAME, when this machine has a
    /// file of them. Empty is the ordinary case and not a fault - see
    /// J1939NameTables.h for why the data is not shipped.
    [[nodiscard]] const J1939NameTables& j1939Names() const noexcept
    {
        return m_j1939Names;
    }

    /// What happened while starting, for a window to print once it has
    /// somewhere to print it.
    [[nodiscard]] std::span<const StartupMessage> startupMessages() const noexcept
    {
        return m_startupMessages;
    }

private:
    void loadPlugins();
    void loadNameTables();

    void say(const QString& text, bool isError);

    std::unique_ptr<services::SettingsStore> m_settings;
    std::unique_ptr<ui::ThemeManager> m_themes;

    NodeCatalog m_catalog{NodeCatalog::withBuiltinTypes()};

    plugins::PluginLoader m_plugins;
    J1939NameTables m_j1939Names;

    std::vector<StartupMessage> m_startupMessages;
};

} // namespace torquebus::app
