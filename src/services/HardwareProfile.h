// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Which interfaces become CAN 1..N, in which order, and how each one is opened.
//
// Until now this was not a decision anybody could make: every device the
// registry found became a channel, in whatever order it was enumerated in. That
// is a reasonable first guess and a poor permanent answer. Two adapters on one
// desk are usually on two different buses, at two different rates; a machine
// with a Kvaser Virtual pair and a real PEAK finds four channels when the
// engineer wanted one; and "CAN 1" ends up meaning "whichever driver answered
// first this morning", which is exactly the sort of thing that makes somebody
// mistrust a trace.
//
// So this holds three separate things, and they are separate on purpose:
//
//   * **The order** - a list of handles. What makes an interface CAN 1 rather
//     than CAN 3, and therefore what every `channel` column in the trace, every
//     transmit row and every project file means.
//
//   * **Whether it is used at all.** A detected interface that is switched off
//     is not opened, does not consume a channel number and does not shift the
//     ones after it.
//
//   * **How it is opened** - rate, FD, listen-only, error frames. Per handle,
//     not per slot: unplug an adapter, plug it back in second, and it keeps its
//     settings.
//
// This belongs to the machine and not to the project. A .tbsproj carried to a
// colleague's desk should use *their* hardware, not describe the author's - so
// it lives in the settings file, beside the theme and the layout.

#pragma once

#include "core/can/CanTypes.h"

#include <QString>
#include <QStringList>

#include <cstdint>

namespace torquebus::services {

class SettingsStore;

/// How one interface is opened, or the defaults for one nobody has configured.
struct ChannelPreferences final {
    /// Whether this interface becomes an application channel at all.
    ///
    /// Defaults to true, so a machine whose adapters have never been configured
    /// behaves exactly as it did before this existed - which is also what
    /// somebody who has just plugged in their first adapter expects.
    bool enabled{true};

    std::uint32_t bitrate{kDefaultBitrate};

    bool canFd{false};
    bool bitRateSwitch{false};

    /// Receive only; never acknowledge. The safe way onto somebody else's bus,
    /// and the setting whose absence has ruined a vehicle test.
    bool listenOnly{false};

    bool receiveErrorFrames{true};
};

class HardwareProfile final {
public:
    /// Reads from `settings`. Borrowed; must outlive this object.
    explicit HardwareProfile(SettingsStore& settings);

    [[nodiscard]] ChannelPreferences preferencesFor(const QString& handle) const;

    /// Stores `preferences` and writes the settings file.
    void setPreferencesFor(const QString& handle, const ChannelPreferences& preferences);

    /// The saved channel order, as handles. Empty on a machine nobody has
    /// arranged, which means "enumeration order".
    [[nodiscard]] QStringList order() const;
    void setOrder(const QStringList& handles);

    /// Puts `detected` into the saved order.
    ///
    /// Handles the profile does not know about go at the end, keeping the order
    /// they were detected in: a newly plugged adapter appears as the last
    /// channel rather than renumbering the ones somebody has been reading all
    /// morning. Handles in `stored` that are not detected are simply absent -
    /// an adapter left at the office does not leave a gap where CAN 2 was.
    ///
    /// Static and pure, because this is the whole of the ordering rule and it
    /// is worth being able to test without a settings file.
    [[nodiscard]] static QStringList arrange(const QStringList& detected,
                                             const QStringList& stored);

private:
    /// The settings key for one of a handle's properties:
    /// "can/channel/peak:usb0/listenOnly".
    [[nodiscard]] static QString keyFor(const QString& handle, const char* property);

    SettingsStore& m_settings;
};

} // namespace torquebus::services
