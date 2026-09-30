// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/project/ProjectExplorerPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QPalette>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

namespace torquebus::ui {
namespace {

/// Role holding the index of a device inside ProjectExplorerPanel::m_devices.
constexpr int kDeviceIndexRole = Qt::UserRole + 1;

QTreeWidgetItem* addCategory(QTreeWidgetItem* parent, const QString& title)
{
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, title);
    item->setExpanded(true);
    return item;
}

} // namespace

ProjectExplorerPanel::ProjectExplorerPanel(QWidget* parent)
    : QWidget{parent}
{
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);

    buildSkeleton();

    connect(m_tree,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { onCurrentItemChanged(current); });

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }
}

void ProjectExplorerPanel::buildSkeleton()
{
    m_root = new QTreeWidgetItem(m_tree);
    m_root->setText(0, tr("Untitled project"));
    m_root->setExpanded(true);

    m_hardware = addCategory(m_root, tr("Hardware"));
    m_databases = addCategory(m_root, tr("Databases"));
    m_transmitLists = addCategory(m_root, tr("Transmit lists"));
    m_scripts = addCategory(m_root, tr("Scripts"));
    m_logs = addCategory(m_root, tr("Logs"));

    m_itemIcons = {
        {m_root, QStringLiteral("project")},
        {m_hardware, QStringLiteral("hardware")},
        {m_databases, QStringLiteral("database")},
        {m_transmitLists, QStringLiteral("transmit")},
        {m_scripts, QStringLiteral("console")},
        {m_logs, QStringLiteral("save")},
    };

    applyIcons();
}

void ProjectExplorerPanel::applyIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return;
    }

    for (auto entry = m_itemIcons.constBegin(); entry != m_itemIcons.constEnd(); ++entry) {
        if (entry.key() != nullptr) {
            entry.key()->setIcon(0, themes->icon(entry.value()));
        }
    }
}

void ProjectExplorerPanel::onThemeChanged()
{
    applyIcons();

    // The device rows are rebuilt rather than walked: they are few, and
    // setDevices() is the one place that knows how a device row is composed.
    setDevices(m_devices);
    setDatabases(m_databasePaths);
}

void ProjectExplorerPanel::setProjectName(const QString& name)
{
    if (m_root != nullptr) {
        m_root->setText(0, name.isEmpty() ? tr("Untitled project") : name);
    }
}

void ProjectExplorerPanel::setDevices(const CanDeviceInfoList& devices)
{
    m_devices = devices;

    const QList<QTreeWidgetItem*> previous = m_hardware->takeChildren();
    qDeleteAll(previous);

    if (devices.empty()) {
        auto* empty = new QTreeWidgetItem(m_hardware);
        empty->setText(0, tr("No CAN interface detected"));
        empty->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        empty->setFlags(Qt::ItemIsEnabled);
        return;
    }

    ThemeManager* themes = ThemeManager::instance();

    for (int index = 0; index < static_cast<int>(devices.size()); ++index) {
        const CanDeviceInfo& device = devices[static_cast<std::size_t>(index)];

        auto* item = new QTreeWidgetItem(m_hardware);

        // The user works with application channels (CAN 1, CAN 2), never with
        // "hardware channel 4" - see PLAN.md section 13. The hardware name is
        // shown as the subtitle, not as the identity.
        item->setText(0,
                      tr("CAN %1  -  %2").arg(index + 1).arg(QString::fromStdString(device.name)));
        item->setToolTip(0, QString::fromStdString(device.handle));
        item->setData(0, kDeviceIndexRole, index);

        if (themes != nullptr) {
            item->setIcon(0, themes->icon(QStringLiteral("hardware")));
        }
    }

    m_hardware->setExpanded(true);
}

void ProjectExplorerPanel::setDatabases(const QStringList& paths)
{
    m_databasePaths = paths;
    if (m_databases == nullptr) {
        return;
    }

    const QList<QTreeWidgetItem*> previous = m_databases->takeChildren();
    qDeleteAll(previous);

    ThemeManager* themes = ThemeManager::instance();
    for (const QString& path : paths) {
        auto* item = new QTreeWidgetItem(m_databases);
        item->setText(0, QFileInfo{path}.fileName());
        item->setToolTip(0, path);
        if (themes != nullptr) {
            item->setIcon(0, themes->icon(QStringLiteral("database")));
        }
    }

    m_databases->setExpanded(true);
}

void ProjectExplorerPanel::onCurrentItemChanged(QTreeWidgetItem* current)
{
    if (current == nullptr) {
        Q_EMIT selectionCleared();
        return;
    }

    const QVariant stored = current->data(0, kDeviceIndexRole);
    if (!stored.isValid()) {
        Q_EMIT selectionCleared();
        return;
    }

    const int index = stored.toInt();
    if (index < 0 || index >= static_cast<int>(m_devices.size())) {
        Q_EMIT selectionCleared();
        return;
    }

    Q_EMIT deviceSelected(m_devices[static_cast<std::size_t>(index)]);
}

} // namespace torquebus::ui
