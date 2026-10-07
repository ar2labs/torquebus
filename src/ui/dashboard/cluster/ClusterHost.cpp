// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/cluster/ClusterHost.h"

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "ui/dashboard/cluster/ClusterThemeSync.h"
#include "ui/theme/IconImageProvider.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickWidget>
#include <QRect>
#include <QRectF>
#include <QScopedValueRollback>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>

#include <algorithm>

namespace torquebus::ui {
namespace {

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

[[nodiscard]] QRect rectOf(const DashboardWidget& widget)
{
    return QRectF{widget.x, widget.y, widget.width, widget.height}.toAlignedRect();
}

} // namespace

ClusterHost::ClusterHost(QWidget& canvas)
    : m_canvas{canvas}
{
    // The cluster draws its icons from the application's own icon set, through the provider that
    // tints them in the cluster's colours. The engine owns the provider.
    m_engine.addImageProvider(QString::fromLatin1(kIconProviderId), new IconImageProvider);

    if (const ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this](const Theme& theme) {
            applyTheme(theme);
        });
    }
}

ClusterHost::~ClusterHost()
{
    // Before the engine and the theme go: see the note on teardown in the header.
    for (Entry& entry : m_entries) {
        delete entry.view;
        delete entry.data;
    }
}

QObject* ClusterHost::theme()
{
    if (m_themeTried) {
        return m_theme.get();
    }

    m_themeTried = true;

    QQmlComponent component{&m_engine, "TorqueBus.Cluster", "ClusterTheme"};
    m_theme.reset(component.create());

    if (m_theme == nullptr) {
        fail(tr("The instrument cluster could not be loaded: %1").arg(component.errorString()));
        return nullptr;
    }

    applyTheme(currentTheme());
    return m_theme.get();
}

void ClusterHost::applyTheme(const Theme& theme)
{
    if (m_theme == nullptr) {
        return;
    }

    if (!syncClusterTheme(*m_theme, theme)) {
        fail(tr("The cluster's QML theme does not match the application's Theme; some of its "
                "colours were not set."));
    }
}

void ClusterHost::fail(const QString& text)
{
    // Once: a cluster that cannot load fails for every widget and on every refresh, and the Output
    // panel is for being told, not for being filled.
    if (m_problem.isEmpty()) {
        m_problem = text;
        Q_EMIT reported(text, true);
    }
}

ClusterHost::Entry* ClusterHost::find(const std::string& id)
{
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found == m_entries.end() ? nullptr : &*found;
}

QQuickWidget* ClusterHost::viewOf(const std::string& id) const
{
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found == m_entries.end() ? nullptr : found->view;
}

const QImage* ClusterHost::snapshotOf(const std::string& id) const
{
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found == m_entries.end() || found->snapshot.isNull() ? nullptr : &found->snapshot;
}

bool ClusterHost::create(const std::string& id)
{
    // A cluster that failed to load once will fail again; do not make a view per refresh to find
    // out.
    if (!m_problem.isEmpty()) {
        return false;
    }

    QObject* sharedTheme = theme();
    if (sharedTheme == nullptr) {
        return false;
    }

    auto* data = new ClusterDataSource(this);

    auto* view = new QQuickWidget(&m_engine, &m_canvas);
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);

    // What makes it a layer over the panel and not a window of its own: see the header.
    view->setClearColor(Qt::transparent);
    view->setAttribute(Qt::WA_AlwaysStackOnTop);
    view->setAttribute(Qt::WA_TransparentForMouseEvents);
    view->setFocusPolicy(Qt::NoFocus);

    view->setInitialProperties({{QStringLiteral("theme"), QVariant::fromValue(sharedTheme)},
                                {QStringLiteral("vehicle"), QVariant::fromValue(data->vehicle())}});
    view->setSource(QUrl{QString::fromLatin1(kClusterViewUrl)});

    if (view->status() != QQuickWidget::Ready) {
        QStringList lines;
        for (const QQmlError& error : view->errors()) {
            lines.append(error.toString());
        }

        delete view;
        delete data;

        fail(tr("The instrument cluster could not be loaded: %1")
                 .arg(lines.join(QLatin1Char('\n'))));
        return false;
    }

    // Hidden until sync() has put it where it goes: a view created at the panel's corner would
    // flash there.
    view->hide();

    Entry entry;
    entry.id = id;
    entry.view = view;
    entry.data = data;
    m_entries.push_back(std::move(entry));
    return true;
}

void ClusterHost::sync(const DashboardDescription& dashboard, bool editing)
{
    // Making or showing a QQuickWidget lets the event loop run, and the panel's timer calls this
    // every tick: a second pass that began inside the first would make a second view for the same
    // widget. What it skips, the next tick does.
    if (m_syncing) {
        return;
    }

    const QScopedValueRollback syncing{m_syncing, true};

    // Views of widgets that are gone: deleted, or no longer a cluster.
    std::erase_if(m_entries, [&dashboard](Entry& entry) {
        const DashboardWidget* widget = dashboard.find(entry.id);

        if (widget != nullptr && widget->kind == DashboardWidgetKind::Cluster) {
            return false;
        }

        delete entry.view;
        delete entry.data;
        return true;
    });

    for (const DashboardWidget& widget : dashboard.widgets()) {
        if (widget.kind != DashboardWidgetKind::Cluster) {
            continue;
        }

        Entry* entry = find(widget.id);

        if (entry == nullptr) {
            if (!create(widget.id)) {
                continue;
            }

            entry = find(widget.id);
        }

        if (entry->profile != widget.profile) {
            entry->profile = widget.profile;
            entry->data->setProfile(ClusterProfiles::instance().find(widget.profile));
        }

        if (const QRect rect = rectOf(widget); entry->view->geometry() != rect) {
            entry->view->setGeometry(rect);
        }

        if (editing) {
            if (entry->view->isVisible()) {
                // On the way out of Run mode, while there is something on screen to take.
                entry->snapshot = entry->view->grabFramebuffer();
            } else if (!entry->snapshotTried && m_canvas.isVisible()) {
                // A cluster that was never on screen - added in Edit mode, or in a project opened
                // in it - is shown for the instant the picture takes. Once: if the platform cannot
                // grab it, asking on every drag would only flicker.
                entry->view->show();
                entry->snapshot = entry->view->grabFramebuffer();
            }

            entry->snapshotTried = true;
            entry->view->hide();
        } else {
            entry->snapshotTried = false;
            if (entry->view->isHidden()) {
                entry->view->show();
            }
        }
    }
}

void ClusterHost::feed(const ClusterDataSource::Reader& read)
{
    for (Entry& entry : m_entries) {
        if (entry.view->isVisible()) {
            entry.data->update(read);
        }
    }
}

} // namespace torquebus::ui
