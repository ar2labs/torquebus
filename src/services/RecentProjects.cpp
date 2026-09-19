// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/RecentProjects.h"

#include "services/SettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#include <utility>

namespace torquebus::services {

RecentProjects::RecentProjects(SettingsStore& settings)
    : m_settings{settings}
{
    m_paths = m_settings.stringListValue(QString::fromLatin1(keys::kRecentProjects));

    // Read defensively: this file can have been written by an older build, or
    // edited by hand - it is JSON on purpose, and somebody will. Blank entries
    // and duplicates are dropped here rather than being allowed to reach a menu.
    QStringList cleaned;
    QStringList seen;

    for (const QString& path : std::as_const(m_paths)) {
        if (path.trimmed().isEmpty()) {
            continue;
        }

        const QString key = comparable(path);
        if (seen.contains(key)) {
            continue;
        }

        seen.append(key);
        cleaned.append(QDir::toNativeSeparators(path));

        if (cleaned.size() >= kMaximum) {
            break;
        }
    }

    m_paths = cleaned;
}

QString RecentProjects::comparable(const QString& path)
{
    // absoluteFilePath() rather than canonicalFilePath(): the canonical form
    // resolves symlinks and returns *empty* for a file that does not exist,
    // which would make every entry on an unplugged drive compare equal to every
    // other one.
    QString result = QFileInfo{path}.absoluteFilePath();

#ifdef Q_OS_WIN
    result = result.toLower();
#endif

    return result;
}

void RecentProjects::add(const QString& path)
{
    if (path.trimmed().isEmpty()) {
        return;
    }

    const QString native = QDir::toNativeSeparators(QFileInfo{path}.absoluteFilePath());
    const QString key = comparable(path);

    for (int index = m_paths.size() - 1; index >= 0; --index) {
        if (comparable(m_paths.at(index)) == key) {
            m_paths.removeAt(index);
        }
    }

    m_paths.prepend(native);

    while (m_paths.size() > kMaximum) {
        m_paths.removeLast();
    }

    store();
}

void RecentProjects::remove(const QString& path)
{
    const QString key = comparable(path);
    bool changed = false;

    for (int index = m_paths.size() - 1; index >= 0; --index) {
        if (comparable(m_paths.at(index)) == key) {
            m_paths.removeAt(index);
            changed = true;
        }
    }

    if (changed) {
        store();
    }
}

void RecentProjects::clear()
{
    if (m_paths.isEmpty()) {
        return;
    }

    m_paths.clear();
    store();
}

void RecentProjects::store() const
{
    m_settings.setStringListValue(QString::fromLatin1(keys::kRecentProjects), m_paths);

    // Written through immediately rather than at exit. The list is worth least
    // in exactly the case where an exit does not happen - a crash, a killed
    // process - and one small write per project opened is not a cost anybody
    // can measure.
    static_cast<void>(m_settings.save());
}

} // namespace torquebus::services
