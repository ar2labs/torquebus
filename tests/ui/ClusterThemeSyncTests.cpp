// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The cluster's colours come from the application's Theme through one function and one QML
// file, and nothing but these tests notices when the two stop agreeing: the cluster would
// just keep drawing the colour it had.

#include "ui/dashboard/cluster/ClusterThemeSync.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/Contrast.h"
#include "ui/theme/Theme.h"

#include <QColor>
#include <QMetaObject>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <gtest/gtest.h>

#include <memory>

namespace torquebus::ui {
namespace {

/// One ClusterTheme.qml, read from the source tree. The engine has to outlive the object.
class LoadedTheme {
public:
    LoadedTheme()
    {
        m_engine.addImportPath(QStringLiteral(TORQUEBUS_QT_QML_DIR));

        QQmlComponent component{&m_engine,
                                QUrl::fromLocalFile(QStringLiteral(TORQUEBUS_CLUSTER_QML_DIR
                                                                   "/common/ClusterTheme.qml"))};
        if (component.isReady()) {
            m_object.reset(component.create());
        }
        if (m_object == nullptr) {
            qWarning("%s", qUtf8Printable(component.errorString()));
        }
    }

    [[nodiscard]] QObject* get() const { return m_object.get(); }

    [[nodiscard]] QColor color(const char* name) const
    {
        return m_object->property(name).value<QColor>();
    }

private:
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_object;
};

} // namespace

TEST(ClusterThemeSyncTests, TheQmlFileLoads)
{
    const LoadedTheme loaded;
    ASSERT_TRUE(loaded.get() != nullptr);
}

TEST(ClusterThemeSyncTests, EveryColourTheQmlTakesFromTheApplicationHasASource)
{
    // The failure this guards is quiet: a colour property added to ClusterTheme.qml and not to
    // the function stays at its default, and the cluster draws it that way on both themes.
    // The writable colours are the raw ones; everything derived is read-only.
    const LoadedTheme loaded;
    ASSERT_TRUE(loaded.get() != nullptr);

    const QStringList sources = clusterThemeColorProperties();
    const QMetaObject* meta = loaded.get()->metaObject();

    int seen = 0;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (property.metaType() != QMetaType::fromType<QColor>() || !property.isWritable()) {
            continue;
        }

        SCOPED_TRACE(property.name());
        ASSERT_TRUE(sources.contains(QString::fromLatin1(property.name())));
        ++seen;
    }

    // and the other way round: nothing the function writes is missing from the QML
    ASSERT_EQ(seen, sources.size());
}

TEST(ClusterThemeSyncTests, TheRolesOfEachVariantReachTheQml)
{
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        const Theme theme = Theme::forVariant(variant);
        SCOPED_TRACE(theme.name.toStdString());

        const LoadedTheme loaded;
        ASSERT_TRUE(loaded.get() != nullptr);
        ASSERT_TRUE(syncClusterTheme(*loaded.get(), theme));

        ASSERT_TRUE(loaded.color("text") == theme.text);
        ASSERT_TRUE(loaded.color("accent") == theme.accent);
        ASSERT_TRUE(loaded.color("canvas") == theme.canvas);
        ASSERT_TRUE(loaded.color("lampAmber") == theme.lampAmber);
        ASSERT_TRUE(loaded.color("instrumentScreen") == theme.instrumentScreen);
        ASSERT_TRUE(loaded.color("instrumentBezel") == theme.instrumentBezel);
    }
}

TEST(ClusterThemeSyncTests, TheFontsAreTheOnesTheApplicationUses)
{
    const LoadedTheme loaded;
    ASSERT_TRUE(loaded.get() != nullptr);
    ASSERT_TRUE(syncClusterTheme(*loaded.get(), Theme::dark()));

    ASSERT_FALSE(loaded.get()->property("displayFamily").toString().isEmpty());
    ASSERT_FALSE(loaded.get()->property("bodyFamily").toString().isEmpty());
    ASSERT_FALSE(loaded.get()->property("monoFamily").toString().isEmpty());
}

TEST(ClusterThemeSyncTests, TheFaceFollowsTheTheme)
{
    // lightFace picks the amounts of light and shade for the glass and the housing. If it
    // read the wrong way round, one theme would get the other's bezel.
    const LoadedTheme dark;
    const LoadedTheme light;
    ASSERT_TRUE(dark.get() != nullptr);
    ASSERT_TRUE(light.get() != nullptr);

    ASSERT_TRUE(syncClusterTheme(*dark.get(), Theme::dark()));
    ASSERT_TRUE(syncClusterTheme(*light.get(), Theme::light()));

    ASSERT_FALSE(dark.get()->property("lightFace").toBool());
    ASSERT_TRUE(light.get()->property("lightFace").toBool());
}

TEST(ClusterThemeSyncTests, TheLitPartOfAGaugeIsSeenOnTheGlassWhateverTheAccent)
{
    // The point of deriving the gauge colours from the accent instead of fixing them: the user
    // can pick any of eight, on either theme, and the part of a gauge that says "this much" has
    // to stay visible against the glass for all sixteen.
    for (const ThemeVariant variant : {ThemeVariant::Dark, ThemeVariant::Light}) {
        for (const AccentColor accent : accentColors()) {
            Theme theme = Theme::forVariant(variant);
            applyAccent(theme, accent);

            SCOPED_TRACE(::testing::Message()
                         << toString(accent).toStdString() << " on " << theme.name.toStdString());

            const LoadedTheme loaded;
            ASSERT_TRUE(loaded.get() != nullptr);
            ASSERT_TRUE(syncClusterTheme(*loaded.get(), theme));

            ASSERT_TRUE(contrastRatio(loaded.color("litEnd"), theme.instrumentScreen)
                        >= kMinimumUiContrast);
        }
    }
}

TEST(ClusterThemeSyncTests, AMissingPropertyIsReportedAndTheRestIsStillWritten)
{
    // QObject::setProperty would invent the missing property and say nothing, and the QML
    // that was meant to read it would see its default.
    QObject bare;
    ASSERT_FALSE(syncClusterTheme(bare, Theme::dark()));
    ASSERT_TRUE(bare.dynamicPropertyNames().isEmpty());
}

} // namespace torquebus::ui
