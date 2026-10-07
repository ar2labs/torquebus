// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Puts the QML instrument cluster on the Dashboard.
//
// The Dashboard is painted by hand, one QPainter for every widget, and the cluster is QML. The two
// meet here, in the least invasive way there is: each Cluster widget of the description gets a
// QQuickWidget laid over the panel at the widget's rectangle, and the panel keeps doing everything
// else - choosing, dragging, resizing, deleting - on the widget beneath it.
//
//   * The QQuickWidget is transparent to the mouse, so the panel still gets every click, and has a
//     transparent background, so what is round the cluster's frame is the panel's canvas.
//   * It is stacked above the panel's painting (Qt::WA_AlwaysStackOnTop), which is the one thing a
//     QQuickWidget cannot be talked out of: the Dashboard's other widgets are drawn *under* a
//     cluster that overlaps them.
//   * In Edit mode it is hidden, because the panel draws the outline and the resize handle of what
//     is selected and a QQuickWidget would cover them. The panel shows a picture of the cluster as
//     it last was (snapshotOf) instead, so a cluster being placed looks like a cluster.
//
// One QML engine serves every cluster, so the module is compiled once however many are on the
// panel, and one ClusterTheme serves them all, kept in step with the application's Theme.
//
// Teardown: the host owns its views and deletes them itself, in its destructor, before the engine
// and the theme they were built on. Qt deletes the members of a widget before its children
// (ARCHITECTURE.md, "Qt teardown order"), so a view left to the panel's destructor would outlive
// the engine it runs in. The host keeps no reference into its owner: the description and the
// reader come in with each call and are not remembered.

#pragma once

#include "ui/dashboard/cluster/ClusterDataSource.h"

#include <QImage>
#include <QObject>
#include <QQmlEngine>
#include <QString>

#include <memory>
#include <string>
#include <vector>

class QQuickWidget;
class QWidget;

namespace torquebus {
class DashboardDescription;
} // namespace torquebus

namespace torquebus::ui {

struct Theme;

/// Where the cluster's QML is in the compiled module: the root of the module, then the file. A
/// path that moved with a rename in the module's CMakeLists is what ClusterHostTests notices.
inline constexpr auto kClusterViewUrl = "qrc:/qt/qml/TorqueBus/Cluster/qml/ClusterView.qml";

class ClusterHost final : public QObject {
    Q_OBJECT

public:
    /// `canvas` is the widget the clusters are laid over - the Dashboard panel. Not owned, and not
    /// the host's parent either: the canvas holds the host as a member, so that it is destroyed
    /// while the canvas still has its children (the views), which ~QWidget would delete first.
    explicit ClusterHost(QWidget& canvas);
    ~ClusterHost() override;

    ClusterHost(const ClusterHost&) = delete;
    ClusterHost& operator=(const ClusterHost&) = delete;

    /// Makes the views match the description: one per Cluster widget, at its rectangle, shown in
    /// Run mode and hidden in Edit mode. Cheap to call whenever the description or the mode may
    /// have changed.
    void sync(const DashboardDescription& dashboard, bool editing);

    /// Gives every cluster its values. Called on the panel's own refresh, with the panel's own
    /// reader; clusters that are not on screen are not fed. `now` is for the tests.
    void feed(const ClusterDataSource::Reader& read,
              ClusterDataSource::Clock::time_point now = ClusterDataSource::Clock::now());

    /// The cluster `id` as it last was on screen, for the panel to draw while the real one is
    /// hidden; null before it has ever been drawn.
    [[nodiscard]] const QImage* snapshotOf(const std::string& id) const;

    /// Why the cluster cannot be shown, or empty. The QML module is not in the executable, or a
    /// file of it does not compile: the panel says so where the cluster would have been.
    [[nodiscard]] const QString& problem() const noexcept { return m_problem; }

    /// The views, for the tests that look at what is on screen.
    [[nodiscard]] QQuickWidget* viewOf(const std::string& id) const;

Q_SIGNALS:
    /// A line for the Output panel; the panel passes it on.
    void reported(const QString& text, bool isError);

private:
    struct Entry final {
        std::string id;
        std::string profile;
        QQuickWidget* view{nullptr}; ///< a child of the canvas, deleted by ~ClusterHost
        ClusterDataSource* data{nullptr}; ///< a child of the host
        QImage snapshot;
        bool snapshotTried{false}; ///< in this stay in Edit mode
    };

    /// The theme the clusters share, made on first use; null when the module cannot be loaded.
    [[nodiscard]] QObject* theme();
    void applyTheme(const Theme& theme);

    [[nodiscard]] Entry* find(const std::string& id);
    [[nodiscard]] bool create(const std::string& id);
    void fail(const QString& text);

    QWidget& m_canvas;

    // Declared before the entries, destroyed after them: see the note on teardown above.
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_theme;
    bool m_themeTried{false};

    std::vector<Entry> m_entries;
    QString m_problem;
    bool m_syncing{false};
};

} // namespace torquebus::ui
