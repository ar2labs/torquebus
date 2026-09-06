// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Application-level settings, stored as JSON.
//
// JSON rather than the registry or an INI file (PLAN.md, section 1): a settings
// file an engineer can read, diff, copy between machines and check into a lab's
// configuration repository. Project-level state lives in the .tbsproj instead -
// this store is only for preferences that follow the user, not the project.

#pragma once

#include "core/can/CanTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace torquebus::services {

class SettingsStore final {
public:
    /// Loads from the default location, creating nothing on disk yet.
    SettingsStore();

    /// Loads from an explicit path. Used by the tests.
    explicit SettingsStore(QString filePath);

    /// Absolute path of the backing file.
    [[nodiscard]] const QString& filePath() const noexcept { return m_filePath; }

    /// Default location: <AppDataLocation>/settings.json
    [[nodiscard]] static QString defaultFilePath();

    [[nodiscard]] bool load();

    /// Writes atomically (temporary file + rename), so an interrupted save
    /// cannot leave a truncated settings file behind.
    bool save() const;

    // --- Typed accessors --------------------------------------------------

    [[nodiscard]] QString value(const QString& key, const QString& fallback = {}) const;
    void setValue(const QString& key, const QString& value);

    [[nodiscard]] bool boolValue(const QString& key, bool fallback = false) const;
    void setBoolValue(const QString& key, bool value);

    [[nodiscard]] int intValue(const QString& key, int fallback = 0) const;
    void setIntValue(const QString& key, int value);

    /// Binary blobs (window geometry, dock layouts) are stored base64-encoded
    /// so the file stays valid, readable JSON.
    [[nodiscard]] QByteArray binaryValue(const QString& key) const;
    void setBinaryValue(const QString& key, const QByteArray& value);

    void remove(const QString& key);
    [[nodiscard]] bool contains(const QString& key) const;

private:
    QString m_filePath;
    QJsonObject m_root;
};

/// Keys used by the application. Centralised so a typo becomes a link error
/// rather than a silently forgotten preference.
namespace keys {

inline constexpr auto kTheme            = "ui/theme";
inline constexpr auto kAccent           = "ui/accent";
inline constexpr auto kFollowSystemTheme = "ui/followSystemTheme";
inline constexpr auto kDensity          = "ui/density";

/// Splitters *inside* a panel. The dock layout saver knows about docks and the
/// boundaries between them; a splitter a panel put inside itself is invisible
/// to it, so each one that exists is stored here by name.
inline constexpr auto kCanvasSplitter     = "ui/canvas/splitter";
inline constexpr auto kStatisticsSplitter = "ui/statistics/splitter";
inline constexpr auto kGraphSplitter      = "ui/graph/splitter";

inline constexpr auto kTraceRefreshMs      = "trace/refreshMs";
inline constexpr auto kDecimalIdentifiers  = "trace/decimalIdentifiers";

/// Prefix for the bitrate chosen for one interface: "can/bitrate/peak:usb0".
///
/// Keyed by the device handle rather than by the application channel, because
/// the rate belongs to the bus the adapter is plugged into, not to the slot it
/// happens to occupy in this session's channel list. Unplug an adapter, plug it
/// back in second, and it keeps its rate.
inline constexpr auto kBitratePrefix = "can/bitrate/";

inline constexpr auto kWindowGeometry   = "ui/window/geometry";
inline constexpr auto kWindowState      = "ui/window/state";
inline constexpr auto kDockLayout       = "ui/window/dockLayout";
inline constexpr auto kDockLayoutVersion = "ui/window/dockLayoutVersion";
inline constexpr auto kLastProject      = "project/lastOpened";
inline constexpr auto kRestoreLayout    = "ui/window/restoreLayout";
inline constexpr auto kSettingsVersion  = "meta/version";

} // namespace keys

/// The settings key holding `handle`'s bitrate.
///
/// A function rather than a concatenation at each call site, so the one place
/// that decides what a per-device key looks like is the one place that has to
/// change when devices grow a second per-device setting.
[[nodiscard]] QString bitrateKey(const QString& handle);

/// The bitrate stored for `handle`, or the default when nothing is stored.
///
/// Also the default when what is stored is not one of the rates TorqueBus can
/// actually configure - a hand-edited settings file is a supported thing to
/// have, and a number nothing can honour is better replaced than obeyed.
[[nodiscard]] std::uint32_t bitrateFor(const SettingsStore& settings, const QString& handle);

} // namespace torquebus::services
