// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The left-hand tree: everything a project owns, in one place.
//
// In v0.1 the tree is populated from the backend registry only. From v0.10 it
// is driven by the open .tbsproj, and the categories below become its sections.

#pragma once

#include "core/can/CanTypes.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus::ui {

class ProjectExplorerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectExplorerPanel(QWidget* parent = nullptr);

    /// Rebuilds the Hardware section from the given device list.
    void setDevices(const CanDeviceInfoList& devices);

    /// Rebuilds the Databases section from the loaded .dbc file paths.
    void setDatabases(const QStringList& paths);

    /// Name shown on the root node.
    void setProjectName(const QString& name);

Q_SIGNALS:
    /// A hardware channel was selected; the Properties panel listens to this.
    void deviceSelected(const torquebus::CanDeviceInfo& device);

    /// Selection moved to something that is not a device.
    void selectionCleared();

private Q_SLOTS:
    void onCurrentItemChanged(QTreeWidgetItem* current);

    /// Re-tints every icon in the tree for the new theme.
    ///
    /// Icons are monochrome SVGs coloured at load time, so an icon created
    /// under the dark theme is a pale glyph - invisible once the window turns
    /// light. Anything that caches a tinted icon has to re-tint it here.
    void onThemeChanged();

private:
    void buildSkeleton();
    void applyIcons();

    QTreeWidget* m_tree{nullptr};

    QTreeWidgetItem* m_root{nullptr};
    QTreeWidgetItem* m_hardware{nullptr};
    QTreeWidgetItem* m_databases{nullptr};
    QTreeWidgetItem* m_transmitLists{nullptr};
    QTreeWidgetItem* m_scripts{nullptr};
    QTreeWidgetItem* m_logs{nullptr};

    CanDeviceInfoList m_devices;
    QStringList m_databasePaths;

    /// Icon resource stem per category item, so a theme change can re-tint
    /// them without rebuilding the tree and losing the user's selection.
    QHash<QTreeWidgetItem*, QString> m_itemIcons;
};

} // namespace torquebus::ui
