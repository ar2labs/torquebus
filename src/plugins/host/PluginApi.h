// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The contract between TorqueBus and a plugin.
//
// This is the one header an out-of-tree plugin includes. Everything in it is
// part of a promise to somebody who is not in this repository, so nothing here
// changes without kPluginAbiVersion changing with it.
//
// --- Why this is C++ and not C ----------------------------------------------
//
// What crosses the boundary is ICanBackend and NodeCatalog, and those already
// speak std::function, std::span, std::string_view and Result. Rewriting them
// in C would make the boundary stable on any compiler, and would turn every
// call of every future feature into a translation written by hand on both
// sides, forever.
//
// So the C++ stays, and the cost is paid as a **rule instead of a layer**: a
// plugin built with a different compiler, a different standard library, or a
// different debug setting is **refused**, not loaded and hoped for. PLAN.md
// section 1 froze MSVC x64; this is a single-platform desktop application, not
// a library, and that is what makes the trade a good one here and a bad one
// somewhere else.
//
// Note what is *not* in the build key: Qt. Nothing crossing this boundary is a
// Qt type - architectural rule #3 keeps the core free of it - so a plugin does
// not have to agree with the host about Qt, and a driver plugin does not have
// to link it at all.
//
// --- The two fields that may never move -------------------------------------
//
// `abiVersion` and `buildKey` are read *before* anything else, because reading
// the rest is only safe once they have been checked. They are the first two
// members of PluginInfo and they stay there whatever else happens to this
// header. A plugin built against a future version must still be rejectable by
// a host that predates it, and that only works if the rejection can be decided
// from bytes whose position both sides already agreed on.

#pragma once

#include "core/pipeline/NodeCatalog.h"
#include "drivers/api/CanBackendRegistry.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace torquebus::plugins {

/// Bumped whenever anything a plugin can see changes shape.
///
/// Not a semantic version: there is no "compatible" change to an ABI that is
/// checked by exact match. One number, and it moves or it does not.
inline constexpr std::uint32_t kPluginAbiVersion = 1U;

#define TORQUEBUS_PLUGIN_STRINGIFY_(x) #x
#define TORQUEBUS_PLUGIN_STRINGIFY(x) TORQUEBUS_PLUGIN_STRINGIFY_(x)

#if defined(_MSC_VER)
#define TORQUEBUS_PLUGIN_COMPILER "msvc-" TORQUEBUS_PLUGIN_STRINGIFY(_MSC_VER)
#elif defined(__clang__)
#define TORQUEBUS_PLUGIN_COMPILER "clang-" TORQUEBUS_PLUGIN_STRINGIFY(__clang_major__)
#elif defined(__GNUC__)
#define TORQUEBUS_PLUGIN_COMPILER "gcc-" TORQUEBUS_PLUGIN_STRINGIFY(__GNUC__)
#else
#define TORQUEBUS_PLUGIN_COMPILER "unknown"
#endif

// The debug level of the standard library, which on MSVC decides the layout of
// every container in this header. A Debug host and a Release plugin share a
// compiler, a Qt and an ABI version, and crash on the first std::function they
// pass each other - so it is part of the key, not a footnote.
#if defined(_ITERATOR_DEBUG_LEVEL)
#define TORQUEBUS_PLUGIN_STL_DEBUG TORQUEBUS_PLUGIN_STRINGIFY(_ITERATOR_DEBUG_LEVEL)
#else
#define TORQUEBUS_PLUGIN_STL_DEBUG "0"
#endif

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
#define TORQUEBUS_PLUGIN_ARCH "64"
#else
#define TORQUEBUS_PLUGIN_ARCH "32"
#endif

/// The key a plugin must match, byte for byte, to be loaded.
///
/// Baked into the plugin at its compile time and into the host at its own, so
/// the comparison is between what each side was actually built with rather than
/// what either claims.
#define TORQUEBUS_PLUGIN_BUILD_KEY                                                                 \
    "torquebus-abi-" TORQUEBUS_PLUGIN_STRINGIFY(1) "/" TORQUEBUS_PLUGIN_COMPILER                   \
                                                   "/x" TORQUEBUS_PLUGIN_ARCH                      \
                                                   "/stl-" TORQUEBUS_PLUGIN_STL_DEBUG

/// The key this translation unit was compiled with.
[[nodiscard]] constexpr std::string_view pluginBuildKey() noexcept
{
    return TORQUEBUS_PLUGIN_BUILD_KEY;
}

/// What the host hands a plugin so it can register what it brings.
///
/// The same registries the built-in code uses, and the same calls. There is no
/// second API for outsiders - if there were, it would be the one that rots,
/// because nobody inside would be using it.
///
/// **Use these pointers. Never reach for a singleton.** CanBackendRegistry has
/// an instance() and it is the right way to get one *inside the application* -
/// and the wrong way inside a plugin. The registry lives in a static library
/// that both the application and the plugin link, so each ends up with its own
/// copy of it. A plugin that called instance() would register into a registry
/// nobody reads, succeed, report success, and add nothing to the list. That
/// failure looks exactly like the plugin not having loaded at all.
struct PluginHost final {
    /// Where a plugin registers a CAN backend. Never null.
    CanBackendRegistry* backends{nullptr};

    /// Where a plugin registers a node type. Never null.
    NodeCatalog* nodes{nullptr};

    /// Where a plugin says something to the person running the program. Never
    /// null, and it reaches the Output panel.
    std::function<void(std::string_view text, bool isError)> log;
};

/// What a plugin says about itself.
struct PluginInfo final {
    // --- These two never move. See the note at the top of this file. --------

    std::uint32_t abiVersion{kPluginAbiVersion};
    const char* buildKey{TORQUEBUS_PLUGIN_BUILD_KEY};

    // --- Everything below is read only after those two have been checked ----

    /// Short, stable, and what appears in a log line. "kvaser", not "Kvaser
    /// CANlib driver plugin v2".
    const char* name{nullptr};

    /// What a person reads in the plugin list.
    const char* displayName{nullptr};

    /// The plugin's own version, not this ABI's.
    const char* version{nullptr};

    /// Registers everything this plugin brings. Returns false when it cannot -
    /// a driver whose SDK turned out to be missing, say - having already said
    /// why through `host.log`.
    bool (*registerWith)(const PluginHost& host){nullptr};
};

/// The symbol a plugin exports, and the only one the host looks for.
///
/// `extern "C"` because C++ decorates names differently between compilers, and
/// a loader that cannot find the symbol cannot even say why it failed.
inline constexpr const char* kPluginEntrySymbol = "torquebusPluginQuery";

} // namespace torquebus::plugins

extern "C" {
/// The shape of the exported symbol.
using TorqueBusPluginQuery = const torquebus::plugins::PluginInfo* (*)();
}

/// Declares the entry point in a plugin. One line, so that getting it wrong is
/// hard and the symbol name lives in exactly one place.
#define TORQUEBUS_DECLARE_PLUGIN(infoExpression)                                                   \
    extern "C" __declspec(dllexport) const torquebus::plugins::PluginInfo* torquebusPluginQuery()  \
    {                                                                                              \
        static const torquebus::plugins::PluginInfo info = (infoExpression);                       \
        return &info;                                                                              \
    }
