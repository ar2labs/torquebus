// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/Workspaces.h"

#include "services/SettingsStore.h"

namespace torquebus::services {
namespace {

/// Where a workspace's parts live: "ui/workspace/<name>/layout".
constexpr auto kPrefix = "ui/workspace/";

/// The list of names, which is also their order.
constexpr auto kNamesKey = "ui/workspaces";

} // namespace

Workspaces::Workspaces(SettingsStore& settings)
    : m_settings{settings}
{ }

bool Workspaces::isValidName(const QString& name)
{
    const QString trimmed = name.trimmed();

    if (trimmed.isEmpty()) {
        return false;
    }

    // A slash would split the settings key and read back as a different
    // workspace - or as none. Refused at the door rather than silently
    // replaced, because a name somebody typed and a name they get back should
    // be the same string.
    return !trimmed.contains(QLatin1Char('/'));
}

QString Workspaces::layoutKey(const QString& name)
{
    return QString::fromLatin1(kPrefix) + name.trimmed() + QStringLiteral("/layout");
}

QString Workspaces::versionKey(const QString& name)
{
    return QString::fromLatin1(kPrefix) + name.trimmed() + QStringLiteral("/layoutVersion");
}

QStringList Workspaces::names() const
{
    QStringList result;

    // Filtered on the way out: this file is editable JSON, and a name that
    // cannot be a key is a name whose layout could never be found again.
    for (const QString& name : m_settings.stringListValue(QString::fromLatin1(kNamesKey))) {
        if (isValidName(name) && !result.contains(name.trimmed())) {
            result.append(name.trimmed());
        }
    }

    return result;
}

bool Workspaces::contains(const QString& name) const
{
    return names().contains(name.trimmed());
}

void Workspaces::storeNames(const QStringList& names) const
{
    m_settings.setStringListValue(QString::fromLatin1(kNamesKey), names);

    // Written through. A workspace is saved by somebody who has just spent a
    // minute arranging panels, and losing that to a crash means doing it again
    // from memory.
    static_cast<void>(m_settings.save());
}

bool Workspaces::save(const QString& name, const QByteArray& layout, int layoutVersion)
{
    if (!isValidName(name)) {
        return false;
    }

    const QString key = name.trimmed();

    m_settings.setBinaryValue(layoutKey(key), layout);
    m_settings.setIntValue(versionKey(key), layoutVersion);

    QStringList existing = names();
    if (!existing.contains(key)) {
        // Appended rather than prepended: this list is an order somebody chose
        // by creating them, not a most-recently-used stack. A menu whose
        // entries move around is a menu that has to be read every time.
        existing.append(key);
    }

    storeNames(existing);
    return true;
}

QByteArray Workspaces::layoutFor(const QString& name, int layoutVersion) const
{
    const QString key = name.trimmed();

    if (!contains(key) || isStale(key, layoutVersion)) {
        return {};
    }

    return m_settings.binaryValue(layoutKey(key));
}

bool Workspaces::isStale(const QString& name, int layoutVersion) const
{
    const QString key = name.trimmed();

    if (!contains(key)) {
        return false;
    }

    // Absent counts as stale: a workspace written before versions were stored
    // describes a panel set nobody can now name, and restoring it would put a
    // window on screen with panels missing and no explanation.
    return m_settings.intValue(versionKey(key), -1) != layoutVersion;
}

void Workspaces::remove(const QString& name)
{
    const QString key = name.trimmed();

    QStringList existing = names();
    if (!existing.removeAll(key)) {
        return;
    }

    m_settings.remove(layoutKey(key));
    m_settings.remove(versionKey(key));

    storeNames(existing);
}

} // namespace torquebus::services
