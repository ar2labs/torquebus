// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// QML fails by printing a warning and carrying on: a type that does not resolve, a binding that
// reads a null, a lamp whose icon never loaded all leave a screen that is merely wrong. The tests
// that load QML treat any warning Qt prints while they run as the failure it is, and this is what
// collects them.

#pragma once

#include <QString>
#include <QStringList>
#include <QtLogging>

#include <string>

namespace torquebus::ui {

/// Collects what Qt prints as a warning or worse while it lives. The errors of a QML file arrive
/// as warnings, so this is where they are.
class QmlWarningCatcher {
public:
    QmlWarningCatcher()
    {
        messages().clear();
        m_previous = qInstallMessageHandler(&QmlWarningCatcher::handle);
    }

    ~QmlWarningCatcher() { qInstallMessageHandler(m_previous); }

    QmlWarningCatcher(const QmlWarningCatcher&) = delete;
    QmlWarningCatcher& operator=(const QmlWarningCatcher&) = delete;

    [[nodiscard]] static QStringList& messages()
    {
        static QStringList collected;
        return collected;
    }

    /// What was caught, one per line, for the message of the assertion that found it.
    [[nodiscard]] static std::string describe()
    {
        return messages().join(QStringLiteral("; ")).toStdString();
    }

private:
    static void handle(QtMsgType type, const QMessageLogContext&, const QString& text)
    {
        // The offscreen platform the tests run on has no font directory, and says so the first
        // time any text is laid out. It is about the machine and not about the QML, so it is the
        // one message let through; anything else, from the QML or from Qt, is a failure.
        if (text.startsWith(QLatin1String("QFontDatabase:"))) {
            return;
        }

        if (type != QtDebugMsg && type != QtInfoMsg) {
            messages().append(text);
        }
    }

    QtMessageHandler m_previous{nullptr};
};

} // namespace torquebus::ui
