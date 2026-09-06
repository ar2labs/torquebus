// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/HardwareProfile.h"

#include "services/SettingsStore.h"

namespace torquebus::services {
namespace {

/// Where one interface's settings live: "can/channel/<handle>/<property>".
constexpr auto kChannelPrefix = "can/channel/";

constexpr auto kEnabled = "enabled";
constexpr auto kCanFd = "fd";
constexpr auto kBitRateSwitch = "brs";
constexpr auto kListenOnly = "listenOnly";
constexpr auto kErrorFrames = "errorFrames";

/// The saved channel order, as a list of handles.
constexpr auto kOrderKey = "can/channelOrder";

} // namespace

HardwareProfile::HardwareProfile(SettingsStore& settings)
    : m_settings{settings}
{
}

QString HardwareProfile::keyFor(const QString& handle, const char* property)
{
    return QString::fromLatin1(kChannelPrefix) + handle + QLatin1Char('/')
        + QString::fromLatin1(property);
}

ChannelPreferences HardwareProfile::preferencesFor(const QString& handle) const
{
    ChannelPreferences preferences;

    preferences.enabled = m_settings.boolValue(keyFor(handle, kEnabled), true);

    // Through the existing helper, which also checks the stored number against
    // the rates a backend has timing for. The key is unchanged from v0.7, so a
    // rate chosen before this class existed is still honoured.
    preferences.bitrate = bitrateFor(m_settings, handle);

    preferences.canFd = m_settings.boolValue(keyFor(handle, kCanFd), false);
    preferences.bitRateSwitch = m_settings.boolValue(keyFor(handle, kBitRateSwitch), false);
    preferences.listenOnly = m_settings.boolValue(keyFor(handle, kListenOnly), false);
    preferences.receiveErrorFrames = m_settings.boolValue(keyFor(handle, kErrorFrames), true);

    // BRS without FD is not a thing a channel can be opened as, and a settings
    // file is editable text - so the combination is resolved here rather than
    // being passed to a driver that would reject it with a message about
    // something else.
    if (!preferences.canFd) {
        preferences.bitRateSwitch = false;
    }

    return preferences;
}

void HardwareProfile::setPreferencesFor(const QString& handle,
                                        const ChannelPreferences& preferences)
{
    m_settings.setBoolValue(keyFor(handle, kEnabled), preferences.enabled);
    m_settings.setIntValue(bitrateKey(handle), static_cast<int>(preferences.bitrate));
    m_settings.setBoolValue(keyFor(handle, kCanFd), preferences.canFd);
    m_settings.setBoolValue(keyFor(handle, kBitRateSwitch),
                            preferences.canFd && preferences.bitRateSwitch);
    m_settings.setBoolValue(keyFor(handle, kListenOnly), preferences.listenOnly);
    m_settings.setBoolValue(keyFor(handle, kErrorFrames), preferences.receiveErrorFrames);

    // Written through, like the recent list: a hardware arrangement lost to a
    // crash is one somebody has to make again from memory, and they will only
    // find out at the next Start.
    static_cast<void>(m_settings.save());
}

QStringList HardwareProfile::order() const
{
    return m_settings.stringListValue(QString::fromLatin1(kOrderKey));
}

void HardwareProfile::setOrder(const QStringList& handles)
{
    m_settings.setStringListValue(QString::fromLatin1(kOrderKey), handles);
    static_cast<void>(m_settings.save());
}

QStringList HardwareProfile::arrange(const QStringList& detected, const QStringList& stored)
{
    QStringList result;

    // Saved order first, and only for what is actually present.
    for (const QString& handle : stored) {
        if (detected.contains(handle) && !result.contains(handle)) {
            result.append(handle);
        }
    }

    // Then anything new, in the order it was detected. A freshly plugged
    // adapter becomes the last channel rather than renumbering the ones
    // somebody has been reading all morning.
    for (const QString& handle : detected) {
        if (!result.contains(handle)) {
            result.append(handle);
        }
    }

    return result;
}

} // namespace torquebus::services
