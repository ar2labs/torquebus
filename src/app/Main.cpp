// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See the LICENSE file at the root of this repository.

#include "app/ApplicationContext.h"
#include "services/SettingsStore.h"
#include "ui/mainwindow/Docking.h"
#include "ui/mainwindow/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>

namespace {

constexpr auto kOrganizationName = "TorqueBus";
constexpr auto kOrganizationDomain = "torquebus.org";
constexpr auto kApplicationName = "TorqueBus Studio";

} // namespace

int main(int argc, char* argv[])
{
    QApplication application{argc, argv};

    QCoreApplication::setOrganizationName(QString::fromLatin1(kOrganizationName));
    QCoreApplication::setOrganizationDomain(QString::fromLatin1(kOrganizationDomain));
    QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(TORQUEBUS_VERSION));

    QGuiApplication::setWindowIcon(QIcon{QStringLiteral(":/icons/torquebus.svg")});

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Open Automotive Network & Diagnostics Workbench"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption resetLayoutOption{
        QStringList{QStringLiteral("reset-layout")},
        QCoreApplication::translate("main",
                                    "Start with the default window layout, ignoring the "
                                    "one saved by the previous session.")};
    parser.addOption(resetLayoutOption);

    parser.addPositionalArgument(
        QCoreApplication::translate("main", "project"),
        QCoreApplication::translate("main", "A .tbsproj project file to open on startup."));

    parser.process(application);

    torquebus::app::ApplicationContext context;
    context.initialize();

    if (parser.isSet(resetLayoutOption)) {
        context.settings().remove(
            QString::fromLatin1(torquebus::services::keys::kDockLayout));
    }

    // The docking framework must be configured after QApplication and before
    // the first dock widget is created.
    torquebus::ui::configureDockingSystem();

    torquebus::ui::MainWindow window{context.settings(), context.themes()};
    window.show();

    // The positional argument has been advertised in --help since v0.1 and
    // ignored for just as long. Opened after show() so that a failure reports
    // itself in the Output panel of a window the user can actually see.
    const QStringList positional = parser.positionalArguments();

    if (!positional.isEmpty()) {
        window.openProject(positional.first());
    } else if (context.settings().boolValue(
                   QString::fromLatin1(torquebus::services::keys::kRestoreLastProject), true)) {
        // Where the last session left off. A project named on the command line
        // always wins - somebody who typed a path meant that path - and a
        // stored path that no longer opens takes itself off the recent list
        // rather than stopping the launch.
        const QString last =
            context.settings().value(QString::fromLatin1(torquebus::services::keys::kLastProject));

        if (!last.isEmpty() && QFileInfo::exists(last)) {
            window.openProject(last);
        }
    }

    return QApplication::exec();
}
