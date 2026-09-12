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

    /// Something a plugin, or the loader, said while plugins were being loaded.
    ///
    /// Kept rather than printed, because loading happens before there is a
    /// window to print into - and these are exactly the lines that must not be
    /// lost: every one of them explains a backend or a block that is not going
    /// to be in the list.
    struct PluginMessage final {
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

    /// What happened while plugins were loading, for a window to print once it
    /// has somewhere to print it.
    [[nodiscard]] std::span<const PluginMessage> pluginMessages() const noexcept
    {
        return m_pluginMessages;
    }

private:
    void loadPlugins();

    std::unique_ptr<services::SettingsStore> m_settings;
    std::unique_ptr<ui::ThemeManager> m_themes;

    NodeCatalog m_catalog{NodeCatalog::withBuiltinTypes()};

    plugins::PluginLoader m_plugins;
    std::vector<PluginMessage> m_pluginMessages;
};

} // namespace torquebus::app
