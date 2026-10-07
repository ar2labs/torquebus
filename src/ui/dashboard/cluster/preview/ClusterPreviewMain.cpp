// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A window for working on the instrument cluster's QML without the rest of TorqueBus.
//
//   cmake --build --preset windows-msvc-vs --target torquebus_cluster_preview
//   build\windows-msvc-vs\bin\Debug\torquebus_cluster_preview.exe
//
// Leave it open: it watches the cluster's .qml files and reloads when one is saved, so the loop
// is edit, save, look. What it does not pick up is a change to an icon, which is compiled into
// the executable with the rest of the icon set - that one needs a build.
//
// Options: --light, --scene=hazard|anomaly|nodata|stop, and --shot=<file.png>, which saves a
// picture of the window and exits instead of staying open.
//
// It is the cluster the application will have. The colours and fonts come from the real Theme
// through the same function the Dashboard will use, the icons from the real icon set behind
// the real IconImageProvider, and a pretend vehicle (FakeVehicle.qml) stands where the bus will.
// The stock qml tool cannot stand in for it: it has no way to register an image provider, and
// without one every lamp is blank.
//
// The cluster is the module TorqueBus.Cluster, compiled into this executable like it will be into
// the application. To reload it, the files the module was built from are put in its place:
// RemapToSource answers every request for a file of the module with the file in the source tree.
// That is also why every file of the module that uses a type from another directory imports the
// module (see CMakeLists.txt): a file read from the source tree sees only its own directory.
//
// Qt's own QML modules are deployed beside the executable by the build (windeployqt), so it runs
// from any terminal.

#include "ui/dashboard/cluster/ClusterThemeSync.h"
#include "ui/theme/IconImageProvider.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickView>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <memory>

Q_IMPORT_QML_PLUGIN(TorqueBus_ClusterPlugin)

namespace {

using torquebus::ui::Theme;

/// The directory of the cluster in the source tree: where the module's files are read from, and
/// the root of the paths the module was built with ("qml/ClusterView.qml" and so on).
[[nodiscard]] QString clusterDir()
{
    return QStringLiteral(TORQUEBUS_CLUSTER_DIR);
}

/// A file of the preview window itself, which is not part of the module.
[[nodiscard]] QUrl previewQml()
{
    return QUrl::fromLocalFile(clusterDir() + QStringLiteral("/qml/preview/ClusterPreview.qml"));
}

/// Answers a request for a file of the module with the file in the source tree, wherever the
/// request came from. A file that has no counterpart there - the qmldir, which the build writes -
/// is left to the module.
class RemapToSource final : public QQmlAbstractUrlInterceptor {
public:
    QUrl intercept(const QUrl& url, DataType) override
    {
        static const QString prefix = QStringLiteral("/qt/qml/TorqueBus/Cluster/");

        if (url.scheme() != QLatin1String("qrc") || !url.path().startsWith(prefix)) {
            return url;
        }

        const QString file = clusterDir() + QLatin1Char('/') + url.path().mid(prefix.size());
        return QFile::exists(file) ? QUrl::fromLocalFile(file) : url;
    }
};

/// A ClusterTheme for `theme`, owned by `owner`; null, with the reason printed, if the module does
/// not provide it.
[[nodiscard]] QObject* makeTheme(QQmlEngine& engine, const Theme& theme, QObject& owner)
{
    QQmlComponent component{&engine, "TorqueBus.Cluster", "ClusterTheme"};
    QObject* object = component.create();

    if (object == nullptr) {
        qWarning("%s", qUtf8Printable(component.errorString()));
        return nullptr;
    }

    object->setParent(&owner);
    torquebus::ui::syncClusterTheme(*object, theme);
    return object;
}

/// Every .qml file under the cluster's QML directory, and the directories themselves, so that a
/// file saved by an editor that replaces it (and so drops it from the watch) is picked up again.
[[nodiscard]] QStringList watchedPaths()
{
    const QString qmlDir = clusterDir() + QStringLiteral("/qml");
    QStringList paths{qmlDir};

    QDirIterator it{qmlDir,
                    {QStringLiteral("*.qml")},
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories};
    while (it.hasNext()) {
        paths.append(it.next());
    }
    return paths;
}

} // namespace

int main(int argc, char** argv)
{
    const QGuiApplication app{argc, argv};

    bool wantsShot = false;
    for (const QString& argument : app.arguments()) {
        wantsShot = wantsShot || argument.startsWith(QStringLiteral("--shot="));
    }

    // The two themes live as long as the QML that was built against them: a reload makes new
    // ones, because the type they are instances of is compiled again. Declared before the view
    // so that they are destroyed after it - the QML is bound to them, and a theme that goes
    // first leaves every binding reading a null.
    std::unique_ptr<QObject> themes;

    QQuickView view;

    // The engine owns the provider and the interceptor's registration; the interceptor itself
    // belongs to this function and outlives the engine's use of it.
    view.engine()->addImageProvider(QString::fromLatin1(torquebus::ui::kIconProviderId),
                                    new torquebus::ui::IconImageProvider);
    RemapToSource remap;
    view.engine()->addUrlInterceptor(&remap);

    QObject::connect(view.engine(), &QQmlEngine::quit, &app, &QGuiApplication::quit);
    view.setResizeMode(QQuickView::SizeRootObjectToView);

    const auto load = [&]() -> bool {
        // The old QML goes first, completely: the objects Qt Quick deletes are deleted later,
        // from the event loop, and a theme gone before them leaves their bindings reading a null.
        view.setSource(QUrl{});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        themes.reset();
        view.engine()->clearComponentCache();

        themes = std::make_unique<QObject>();
        QObject* dark = makeTheme(*view.engine(), Theme::dark(), *themes);
        QObject* light = makeTheme(*view.engine(), Theme::light(), *themes);
        if (dark == nullptr || light == nullptr) {
            return false;
        }

        view.setInitialProperties({{QStringLiteral("darkTheme"), QVariant::fromValue(dark)},
                                   {QStringLiteral("lightTheme"), QVariant::fromValue(light)}});
        view.setSource(previewQml());
        return view.status() == QQuickView::Ready;
    };

    // A taken picture is a one-off, and an error there is the answer; in the window an error
    // is only the state until the next save.
    if (!load() && wantsShot) {
        return 1;
    }

    QFileSystemWatcher watcher;
    QTimer settle;
    settle.setSingleShot(true);
    settle.setInterval(200); // an editor can write a file more than once per save

    const auto watch = [&] { watcher.addPaths(watchedPaths()); };
    watch();

    if (!wantsShot) {
        const auto changed = [&settle] { settle.start(); };
        QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &settle, changed);
        QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &settle, changed);
        QObject::connect(&settle, &QTimer::timeout, &app, [&] {
            qInfo("Reloading the cluster.");
            load();
            watch();
        });
    }

    view.resize(1360, 760);
    view.show();
    return QGuiApplication::exec();
}
