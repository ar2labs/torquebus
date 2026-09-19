// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Finding plugins, checking them, and saying what happened to each one.
//
// --- Failure here looks like absence, and absence is not diagnosable ---------
//
// Every way a plugin can fail to load ends the same way on screen: a backend
// that is not in the list. Somebody then checks the cable, the driver, the
// device manager, and the one thing they cannot check is the thing that went
// wrong. So nothing here fails quietly. A file that could not be opened, a
// symbol that was not there, a build key that did not match, a registration
// that threw - each becomes a line naming the file and the reason.
//
// --- Only one directory --------------------------------------------------
//
// Plugins are loaded from the `plugins` directory beside the executable and
// from nowhere else. Loading a library from the working directory or from PATH
// is how "open a project" turns into "run whatever was in that folder".
//
// --- Loaded is forever ------------------------------------------------------
//
// A plugin that registered a backend or a node type has left function pointers
// inside the registries, and the code behind those pointers lives in its
// library. Unloading it would leave the catalogue full of addresses into
// nothing, and the crash would happen later, somewhere unrelated, in whichever
// unlucky place first built a graph.
//
// So there is no unload. Handles are held until the process exits and the
// operating system takes them back. That is a deliberate limit, not an
// oversight: reloading a plugin without restarting would need the registries to
// be able to forget, and being able to forget is a much larger promise than
// being able to add.

#pragma once

#include "plugins/host/PluginApi.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace torquebus::plugins {

/// A plugin that loaded and registered what it brought.
struct LoadedPlugin final {
    std::string path;
    std::string name;
    std::string displayName;
    std::string version;
};

/// A file in the plugin directory that did not become a plugin.
struct RejectedPlugin final {
    std::string path;

    /// Written for the person who has to fix it, and specific enough to act on:
    /// which key was expected and which arrived, not "incompatible".
    std::string reason;
};

class PluginLoader final {
public:
    /// The file extension a plugin has on this platform.
    [[nodiscard]] static std::string_view extension() noexcept;

    /// The directory plugins are loaded from: `plugins`, beside `executable`.
    [[nodiscard]] static std::filesystem::path
    directoryFor(const std::filesystem::path& executable);

    /// Loads every plugin in `directory`, registering each through `host`.
    ///
    /// A missing directory is not an error - it is what a build with no plugins
    /// looks like, and the program is expected to work without any.
    void loadFrom(const std::filesystem::path& directory, const PluginHost& host);

    [[nodiscard]] std::span<const LoadedPlugin> loaded() const noexcept { return m_loaded; }

    [[nodiscard]] std::span<const RejectedPlugin> rejected() const noexcept { return m_rejected; }

    /// The key this host was built with, for a message that has to show both.
    [[nodiscard]] static std::string_view hostBuildKey() noexcept { return pluginBuildKey(); }

private:
    /// Tries one file. Everything it can go wrong with lands in m_rejected.
    void loadOne(const std::filesystem::path& file, const PluginHost& host);

    std::vector<LoadedPlugin> m_loaded;
    std::vector<RejectedPlugin> m_rejected;

    /// Library handles, held for the life of the process. See the note at the
    /// top of this file about why they are never released.
    std::vector<void*> m_handles;
};

} // namespace torquebus::plugins
