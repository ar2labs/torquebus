// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "plugins/host/PluginLoader.h"

#include <algorithm>
#include <format>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace torquebus::plugins {
namespace {

#if defined(_WIN32)

[[nodiscard]] void* openLibrary(const std::filesystem::path& file, std::string& error)
{
    // Two directories, and this used to be one.
    //
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR is the plugin's own directory, which is
    // what the original LOAD_WITH_ALTERED_SEARCH_PATH was chosen for: a vendor
    // SDK sitting next to the driver that needs it. What that flag also does is
    // *replace* the executable's directory rather than add to it - and the
    // shared Qt lives beside the executable, one level up from `plugins`.
    //
    // So the PEAK plugin, which links Qt6::SerialBus, could not find it. On a
    // machine with Qt on PATH that never showed; on one without, the interface
    // list came up with Kvaser in it and no PEAK. Found by running the
    // application with a deliberately hostile PATH and reading which modules
    // the process had actually loaded.
    //
    // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS adds the application directory, the
    // user-added directories and System32 - which is where both vendor runtimes
    // actually are, because their driver installers put them there.
    //
    // What this pair does *not* include is PATH, and that is a gain rather than
    // a cost: it is one fewer way for a foreign DLL earlier on somebody's PATH
    // to be loaded in place of the intended one. A vendor SDK that lives only
    // on PATH and nowhere else now fails to load - and says so by name, with
    // the reason, which is what the lines below are for.
    //
    // The path must be absolute for these flags, which it is.
    HMODULE handle = ::LoadLibraryExW(file.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                          | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (handle != nullptr) {
        return handle;
    }

    const DWORD code = ::GetLastError();
    error = std::system_category().message(static_cast<int>(code));

    // The message alone is usually "The specified module could not be found",
    // which is also what a *dependency* of the plugin being missing looks like
    // - the commonest failure for a driver plugin whose SDK is not installed,
    // and the one most often misread as "the plugin is not there".
    error += std::format(" (code {})", code);
    return nullptr;
}

[[nodiscard]] void* findSymbol(void* handle, const char* name)
{
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(handle), name));
}

#else

[[nodiscard]] void* openLibrary(const std::filesystem::path& file, std::string& error)
{
    void* handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = ::dlerror();
        error = message != nullptr ? message : "dlopen failed without saying why";
    }

    return handle;
}

[[nodiscard]] void* findSymbol(void* handle, const char* name)
{
    return ::dlsym(handle, name);
}

#endif

} // namespace

std::string_view PluginLoader::extension() noexcept
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::filesystem::path PluginLoader::directoryFor(const std::filesystem::path& executable)
{
    return executable.parent_path() / "plugins";
}

void PluginLoader::loadFrom(const std::filesystem::path& directory, const PluginHost& host)
{
    std::error_code ignored;
    if (!std::filesystem::is_directory(directory, ignored)) {
        // Not an error. A build with no plugins has no directory, and the
        // program is expected to work without any.
        return;
    }

    // Sorted, so that a list of what loaded reads the same way twice and a
    // report from one machine can be compared with a report from another.
    std::vector<std::filesystem::path> files;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, ignored}) {
        // Directly in the directory, and nothing else: not recursive, so a
        // plugin cannot bring a folder of libraries that all get loaded, and
        // not symlinks followed anywhere.
        if (!entry.is_regular_file(ignored)) {
            continue;
        }

        if (entry.path().extension() != extension()) {
            continue;
        }

        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& file : files) {
        loadOne(file, host);
    }
}

void PluginLoader::loadOne(const std::filesystem::path& file, const PluginHost& host)
{
    const std::string path = file.string();

    const auto reject = [this, &path, &host](std::string reason) {
        if (host.log) {
            host.log(std::format("Plugin refused: {} - {}", path, reason), true);
        }

        m_rejected.push_back(RejectedPlugin{.path = path, .reason = std::move(reason)});
    };

    std::string error;
    void* handle = openLibrary(file, error);
    if (handle == nullptr) {
        reject(std::format("could not be opened: {}", error));
        return;
    }

    // Held from here on whatever happens. A library that got as far as running
    // its static initialisers may already have handed something out, and
    // unloading it is the one thing that could turn a refused plugin into a
    // crash somewhere else.
    m_handles.push_back(handle);

    void* symbol = findSymbol(handle, kPluginEntrySymbol);
    if (symbol == nullptr) {
        reject(std::format("does not export {}() - it may not be a TorqueBus plugin",
                           kPluginEntrySymbol));
        return;
    }

    const PluginInfo* info = nullptr;

    try {
        info = reinterpret_cast<TorqueBusPluginQuery>(symbol)();
    } catch (const std::exception& thrown) {
        reject(std::format("threw while being asked what it is: {}", thrown.what()));
        return;
    } catch (...) {
        reject("threw while being asked what it is");
        return;
    }

    if (info == nullptr) {
        reject(std::format("{}() returned nothing", kPluginEntrySymbol));
        return;
    }

    // The version first, then the key: only these two fields have a position
    // both sides agreed on, and reading anything else before they check out is
    // reading a layout this build does not know.
    if (info->abiVersion != kPluginAbiVersion) {
        reject(std::format("was built for plugin ABI {} and this build is ABI {}",
                           info->abiVersion, kPluginAbiVersion));
        return;
    }

    const std::string_view key =
        info->buildKey != nullptr ? std::string_view{info->buildKey} : std::string_view{};

    if (key != hostBuildKey()) {
        // Both keys, because "incompatible" is not something anybody can act
        // on and the difference between the two strings usually names the fix.
        reject(std::format("was built as [{}] and this build is [{}]",
                           key.empty() ? "no build key" : key, hostBuildKey()));
        return;
    }

    if (info->registerWith == nullptr) {
        reject("has nothing to register");
        return;
    }

    bool registered = false;

    try {
        registered = info->registerWith(host);
    } catch (const std::exception& thrown) {
        // One plugin throwing must not take the others, or the program, with
        // it. Everything it managed to register before throwing stays - it is
        // already referenced, and the library is never unloaded.
        reject(std::format("threw while registering: {}", thrown.what()));
        return;
    } catch (...) {
        reject("threw while registering");
        return;
    }

    if (!registered) {
        // The plugin declined, and has already said why through host.log - a
        // driver whose SDK turned out not to be installed, most often. Recorded
        // so the plugin list can show it, without a second message repeating
        // what the plugin just said better.
        m_rejected.push_back(
            RejectedPlugin{.path = path, .reason = "declined to register itself"});
        return;
    }

    const auto text = [](const char* value) -> std::string {
        return value != nullptr ? std::string{value} : std::string{};
    };

    m_loaded.push_back(LoadedPlugin{.path = path,
                                    .name = text(info->name),
                                    .displayName = text(info->displayName),
                                    .version = text(info->version)});

    if (host.log) {
        host.log(std::format("Plugin loaded: {} {}",
                             m_loaded.back().displayName.empty() ? m_loaded.back().name
                                                                 : m_loaded.back().displayName,
                             m_loaded.back().version),
                 false);
    }
}

} // namespace torquebus::plugins
