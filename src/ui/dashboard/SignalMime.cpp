// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/SignalMime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QStringList>

namespace torquebus::ui {
namespace {

constexpr auto kMessage = "message";
constexpr auto kSignal = "signal";

} // namespace

QByteArray encodeSignalReferences(const QList<SignalReference>& references)
{
    QJsonArray array;

    for (const SignalReference& reference : references) {
        QJsonObject entry;
        entry[QLatin1String{kMessage}] = reference.message;
        entry[QLatin1String{kSignal}] = reference.signal;
        array.append(entry);
    }

    return QJsonDocument{array}.toJson(QJsonDocument::Compact);
}

QList<SignalReference> decodeSignalReferences(const QByteArray& data)
{
    QList<SignalReference> references;

    const QJsonDocument document = QJsonDocument::fromJson(data);

    if (!document.isArray()) {
        return references;
    }

    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();

        SignalReference reference;
        reference.message = object.value(QLatin1String{kMessage}).toString();
        reference.signal = object.value(QLatin1String{kSignal}).toString();

        // A signal without a message is nothing; a message without a signal is a table.
        if (!reference.message.isEmpty()) {
            references.append(std::move(reference));
        }
    }

    return references;
}

QMimeData* makeSignalMimeData(const QList<SignalReference>& references)
{
    auto* mime = new QMimeData;
    mime->setData(QLatin1String{kSignalMimeType}, encodeSignalReferences(references));

    QStringList lines;

    for (const SignalReference& reference : references) {
        lines << (reference.signal.isEmpty()
                      ? reference.message
                      : reference.message + QLatin1Char('.') + reference.signal);
    }

    mime->setText(lines.join(QLatin1Char('\n')));
    return mime;
}

bool hasSignalReferences(const QMimeData* data)
{
    return data != nullptr && data->hasFormat(QLatin1String{kSignalMimeType});
}

QList<SignalReference> signalReferencesOf(const QMimeData* data)
{
    if (!hasSignalReferences(data)) {
        return {};
    }

    return decodeSignalReferences(data->data(QLatin1String{kSignalMimeType}));
}

} // namespace torquebus::ui
