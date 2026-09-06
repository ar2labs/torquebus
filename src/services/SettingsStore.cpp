// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/SettingsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace torquebus::services {
namespace {

constexpr int kCurrentSettingsVersion = 1;

} // namespace

SettingsStore::SettingsStore()
    : SettingsStore{defaultFilePath()}
{
}

SettingsStore::SettingsStore(QString filePath)
    : m_filePath{std::move(filePath)}
{
}

QString SettingsStore::defaultFilePath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir{directory}.filePath(QStringLiteral("settings.json"));
}

bool SettingsStore::load()
{
    QFile file{m_filePath};
    if (!file.exists()) {
        // First run. Not an error: the defaults are the defaults.
        m_root = QJsonObject{};
        m_root.insert(QLatin1String(keys::kSettingsVersion), kCurrentSettingsVersion);
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // A corrupt settings file must never prevent the application from
        // starting: fall back to defaults and let the next save overwrite it.
        m_root = QJsonObject{};
        m_root.insert(QLatin1String(keys::kSettingsVersion), kCurrentSettingsVersion);
        return false;
    }

    m_root = document.object();
    m_root.insert(QLatin1String(keys::kSettingsVersion), kCurrentSettingsVersion);
    return true;
}

bool SettingsStore::save() const
{
    const QFileInfo info{m_filePath};
    if (!info.absoluteDir().exists() && !QDir{}.mkpath(info.absolutePath())) {
        return false;
    }

    QSaveFile file{m_filePath};
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    const QJsonDocument document{m_root};
    if (file.write(document.toJson(QJsonDocument::Indented)) == -1) {
        return false;
    }

    return file.commit();
}

QString SettingsStore::value(const QString& key, const QString& fallback) const
{
    const QJsonValue stored = m_root.value(key);
    return stored.isString() ? stored.toString() : fallback;
}

void SettingsStore::setValue(const QString& key, const QString& value)
{
    m_root.insert(key, value);
}

bool SettingsStore::boolValue(const QString& key, bool fallback) const
{
    const QJsonValue stored = m_root.value(key);
    return stored.isBool() ? stored.toBool() : fallback;
}

void SettingsStore::setBoolValue(const QString& key, bool value)
{
    m_root.insert(key, value);
}

int SettingsStore::intValue(const QString& key, int fallback) const
{
    const QJsonValue stored = m_root.value(key);
    return stored.isDouble() ? stored.toInt(fallback) : fallback;
}

void SettingsStore::setIntValue(const QString& key, int value)
{
    m_root.insert(key, value);
}

QByteArray SettingsStore::binaryValue(const QString& key) const
{
    const QJsonValue stored = m_root.value(key);
    if (!stored.isString()) {
        return {};
    }
    return QByteArray::fromBase64(stored.toString().toLatin1());
}

void SettingsStore::setBinaryValue(const QString& key, const QByteArray& value)
{
    if (value.isEmpty()) {
        m_root.remove(key);
        return;
    }
    m_root.insert(key, QString::fromLatin1(value.toBase64()));
}

void SettingsStore::remove(const QString& key)
{
    m_root.remove(key);
}

bool SettingsStore::contains(const QString& key) const
{
    return m_root.contains(key);
}

QString bitrateKey(const QString& handle)
{
    return QString::fromLatin1(keys::kBitratePrefix) + handle;
}

std::uint32_t bitrateFor(const SettingsStore& settings, const QString& handle)
{
    const int stored = settings.intValue(bitrateKey(handle), 0);
    if (stored <= 0) {
        return kDefaultBitrate;
    }

    const auto value = static_cast<std::uint32_t>(stored);

    // Checked against the list rather than taken on trust. The settings file is
    // JSON so that people can edit it, which means a number in it is input, and
    // a rate no backend has segment timing for would open a channel that
    // produces error frames instead of failing outright.
    for (const std::uint32_t candidate : standardBitrates()) {
        if (candidate == value) {
            return value;
        }
    }

    return kDefaultBitrate;
}

} // namespace torquebus::services
