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
#include "core/transmit/TransmitEntry.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus::ui {

struct ScriptExplorerItem final {
    QString nodeId;
    QString label;
    QString toolTip;
};

struct LogExplorerItem final {
    QString nodeId;
    QString label;
    QString toolTip;
    bool isRecordAction{false};
};

class ProjectExplorerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectExplorerPanel(QWidget* parent = nullptr);

    /// Rebuilds the Hardware section from the given enabled device list.
    void setDevices(const CanDeviceInfoList& devices);

    /// Rebuilds the Databases section from the loaded .dbc file paths.
    void setDatabases(const QStringList& paths);

    /// Rebuilds the Transmit lists section from the project's transmit entries.
    void setTransmitEntries(const std::vector<TransmitEntry>& entries);

    /// Rebuilds the Scripts section from the Lua blocks in the pipeline.
    void setScripts(const QList<ScriptExplorerItem>& scripts);

    /// Rebuilds the Logs section from logger/replay blocks and active recordings.
    void setLogs(const QList<LogExplorerItem>& logs);

    /// Name shown on the root node.
    void setProjectName(const QString& name);

    [[nodiscard]] QTreeWidget* treeWidget() const noexcept { return m_tree; }

Q_SIGNALS:
    /// A hardware channel was selected; the Properties panel listens to this.
    void deviceSelected(const torquebus::CanDeviceInfo& device);

    /// A database item was selected.
    void databaseSelected(const QString& path);

    /// A transmit entry was selected.
    void transmitEntrySelected(int index);

    /// A script block was selected.
    void scriptSelected(const QString& nodeId);

    /// A logger or replay block was selected.
    void logNodeSelected(const QString& nodeId);

    /// The user clicked the Record item in the Logs section.
    void recordRequested();

    /// Selection moved to something that is not a device.
    void selectionCleared();

private Q_SLOTS:
    void onCurrentItemChanged(QTreeWidgetItem* current);
    void onItemClicked(QTreeWidgetItem* item);

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
    std::vector<TransmitEntry> m_transmitEntries;
    QList<ScriptExplorerItem> m_scriptItems;
    QList<LogExplorerItem> m_logItems;

    /// Icon resource stem per category item, so a theme change can re-tint
    /// them without rebuilding the tree and losing the user's selection.
    QHash<QTreeWidgetItem*, QString> m_itemIcons;
};

} // namespace torquebus::ui
