// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The cluster is QML, and QML fails by printing a warning and carrying on: a type that does not
// resolve, a binding that reads a null, a lamp whose icon never loaded all leave a screen that is
// merely wrong. These tests load the real module, TorqueBus.Cluster, the way the Dashboard will,
// feed it vehicles of every kind, and treat any warning Qt prints as the failure it is.

#include "QmlWarningCatcher.h"

#include "ui/dashboard/cluster/ClusterThemeSync.h"
#include "ui/theme/IconImageProvider.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QStringList>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>

// A static QML module has to be named by whoever loads it: nothing finds it by itself.
Q_IMPORT_QML_PLUGIN(TorqueBus_ClusterPlugin)

namespace torquebus::ui {
namespace {

/// One ClusterView from the module, over the Theme of `variant`, fed `vehicle` (a QML expression).
class Cluster {
public:
    Cluster(ThemeVariant variant, const QString& vehicle)
    {
        m_engine.addImportPath(QStringLiteral(TORQUEBUS_QT_QML_DIR));
        m_engine.addImageProvider(QString::fromLatin1(kIconProviderId), new IconImageProvider);

        QQmlComponent component{&m_engine};
        component.setData(QStringLiteral("import QtQuick\n"
                                         "import TorqueBus.Cluster\n"
                                         "ClusterView { theme: ClusterTheme {} vehicle: %1 }")
                              .arg(vehicle)
                              .toUtf8(),
                          QUrl{});

        m_root.reset(component.create());
        m_errors = component.errorString();

        if (m_root != nullptr) {
            auto* theme = m_root->property("theme").value<QObject*>();
            if (theme != nullptr) {
                syncClusterTheme(*theme, Theme::forVariant(variant));
            }
        }

        // Let the bindings settle and the lamp test of the start-up begin.
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
        }
    }

    [[nodiscard]] QObject* root() const { return m_root.get(); }
    [[nodiscard]] const QString& errors() const { return m_errors; }

    [[nodiscard]] double number(const char* property) const
    {
        return m_root->property(property).toDouble();
    }

private:
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_root; // before the engine goes
    QString m_errors;
};

/// A vehicle with something for every role the cluster reads.
const QString kFullVehicle = QStringLiteral(
    "({ speed: 72, rpm: 2400, gear: 5, coolant: 90, fuel: 40, def: 30, oil: 400, battery: 27.6,"
    " airPressure: 8.5, ambient: 20, odometer: 12345.6, hours: 100.1,"
    " aiRegime: 4, aiAnomaly: 80, aiConfidence: 90, aiHealth: 20, cruise: true,"
    " lampHigh: true, lampLow: true, lampPosition: true, lampPark: true, lampBelt: true,"
    " lampEngine: true, lampOil: true, lampAbs: true, lampLeft: true, lampRight: true,"
    " lampHazard: true, dmStop: true, dmWarn: true, dmMil: true, dmProtect: true })");

constexpr ThemeVariant kVariants[] = {ThemeVariant::Dark, ThemeVariant::Light};

} // namespace

TEST(ClusterViewTests, TheModuleLoadsAndTheViewHasItsDesignSize)
{
    // The first thing that breaks when the plugin is not linked, or the module is not built the
    // way its files import each other: "module TorqueBus.Cluster is not installed".
    const QmlWarningCatcher catcher;

    const Cluster cluster{ThemeVariant::Dark, QStringLiteral("null")};

    ASSERT_TRUE(cluster.errors().isEmpty());
    ASSERT_TRUE(cluster.root() != nullptr);

    const auto* item = qobject_cast<QQuickItem*>(cluster.root());
    ASSERT_TRUE(item != nullptr);
    ASSERT_EQ(item->implicitWidth(), 1280.0);
    ASSERT_EQ(item->implicitHeight(), 560.0);
    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterViewTests, NoDataIsDrawnWithoutAWarningOnBothThemes)
{
    // The cluster is meant to be on screen with nothing to show: a role that never arrives is
    // dashes, not an error. "No data" has several spellings, and each of them has to be one.
    for (const ThemeVariant variant : kVariants) {
        for (const char* vehicle : {"null",
                                    "({})",
                                    "({ speed: NaN, rpm: undefined, coolant: 'hot', gear: null })",
                                    "({ ignition: false })",
                                    "5"}) {
            SCOPED_TRACE(vehicle);

            const QmlWarningCatcher catcher;

            const Cluster cluster{variant, QString::fromLatin1(vehicle)};

            ASSERT_TRUE(cluster.errors().isEmpty());
            ASSERT_TRUE(cluster.root() != nullptr);
            ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
        }
    }
}

TEST(ClusterViewTests, EveryRoleTheClusterReadsIsDrawnWithoutAWarningOnBothThemes)
{
    // Every lamp lit, every gauge in its alarm: the states the screen is for, and the ones the
    // empty vehicle never reaches.
    for (const ThemeVariant variant : kVariants) {
        const QmlWarningCatcher catcher;

        const Cluster cluster{variant, kFullVehicle};

        ASSERT_TRUE(cluster.errors().isEmpty());
        ASSERT_TRUE(cluster.root() != nullptr);
        ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
    }
}

TEST(ClusterViewTests, ARoleIsANumberOrNoDataAndNothingInBetween)
{
    // What the rest of the cluster trusts: a role that is not a finite number reads as NaN, so
    // every place that draws dashes asks one question.
    const QmlWarningCatcher catcher;

    const Cluster present{ThemeVariant::Dark,
                          QStringLiteral("({ speed: 72, rpm: NaN, gear: 'D' })")};
    ASSERT_TRUE(present.root() != nullptr);
    ASSERT_EQ(present.number("speed"), 72.0);
    ASSERT_TRUE(std::isnan(present.number("rpm")));
    ASSERT_TRUE(std::isnan(present.number("gear")));
    ASSERT_TRUE(std::isnan(present.number("coolant"))); // never mentioned

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

} // namespace torquebus::ui
