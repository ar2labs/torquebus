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

#include <memory>

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

    /// Loads the settings file and applies the persisted theme. Call once,
    /// after QApplication exists and before creating any window.
    void initialize();

    [[nodiscard]] services::SettingsStore& settings() noexcept { return *m_settings; }
    [[nodiscard]] ui::ThemeManager& themes() noexcept { return *m_themes; }

private:
    std::unique_ptr<services::SettingsStore> m_settings;
    std::unique_ptr<ui::ThemeManager> m_themes;
};

} // namespace torquebus::app
