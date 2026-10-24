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
constexpr int kDatabasePathRole = Qt::UserRole + 2;
constexpr int kTransmitIndexRole = Qt::UserRole + 3;
constexpr int kScriptNodeIdRole = Qt::UserRole + 4;
constexpr int kLogNodeIdRole = Qt::UserRole + 5;
constexpr int kLogRecordActionRole = Qt::UserRole + 6;

[[nodiscard]] QString formatCanIdentifier(const CanFrame& frame)
{
    return frame.format == CanFrameFormat::Extended
               ? QStringLiteral("0x%1").arg(frame.identifier, 8, 16, QLatin1Char('0')).toUpper()
               : QStringLiteral("0x%1").arg(frame.identifier, 3, 16, QLatin1Char('0')).toUpper();
}

QTreeWidgetItem* addCategory(QTreeWidgetItem* parent, const QString& title)
{
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, title);
    item->setExpanded(true);
    return item;
}

void addEmptyPlaceholder(QTreeWidgetItem* category, const QString& text, const QPalette& palette)
{
    auto* empty = new QTreeWidgetItem(category);
    empty->setText(0, text);
    empty->setForeground(0, palette.color(QPalette::Disabled, QPalette::Text));
    empty->setFlags(Qt::ItemIsEnabled);
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
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        onItemClicked(item);
    });

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

    // The section rows are rebuilt rather than walked: they are few, and the
    // setters are the one place that knows how each row is composed.
    setDevices(m_devices);
    setDatabases(m_databasePaths);
    setTransmitEntries(m_transmitEntries);
    setScripts(m_scriptItems);
    setLogs(m_logItems);
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
        addEmptyPlaceholder(m_hardware, tr("No CAN interface detected"), palette());
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

    if (paths.isEmpty()) {
        addEmptyPlaceholder(m_databases, tr("No database imported"), palette());
        return;
    }

    ThemeManager* themes = ThemeManager::instance();
    for (const QString& path : paths) {
        auto* item = new QTreeWidgetItem(m_databases);
        item->setText(0, QFileInfo{path}.fileName());
        item->setToolTip(0, path);
        item->setData(0, kDatabasePathRole, path);
        if (themes != nullptr) {
            item->setIcon(0, themes->icon(QStringLiteral("database")));
        }
    }

    m_databases->setExpanded(true);
}

void ProjectExplorerPanel::setTransmitEntries(const std::vector<TransmitEntry>& entries)
{
    m_transmitEntries = entries;
    if (m_transmitLists == nullptr) {
        return;
    }

    const QList<QTreeWidgetItem*> previous = m_transmitLists->takeChildren();
    qDeleteAll(previous);

    if (entries.empty()) {
        addEmptyPlaceholder(m_transmitLists, tr("No transmit frames configured"), palette());
        return;
    }

    ThemeManager* themes = ThemeManager::instance();
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const TransmitEntry& entry = entries[static_cast<std::size_t>(index)];
        const QString name = !entry.name.empty() ? QString::fromStdString(entry.name)
                             : !entry.messageName.empty()
                                 ? QString::fromStdString(entry.messageName)
                                 : tr("Frame %1").arg(index + 1);
        const QString idHex = formatCanIdentifier(entry.frame);
        const QString mode = entry.isPeriodic() ? tr("%1 ms").arg(entry.cycleMs) : tr("Manual");

        auto* item = new QTreeWidgetItem(m_transmitLists);
        item->setText(0, QStringLiteral("%1  (%2)").arg(name, idHex));
        item->setToolTip(0,
                         tr("CAN %1  |  %2  |  %3 bytes  |  %4")
                             .arg(entry.channel + 1)
                             .arg(idHex)
                             .arg(entry.frame.length)
                             .arg(mode));
        item->setData(0, kTransmitIndexRole, index);
        if (themes != nullptr) {
            item->setIcon(0, themes->icon(QStringLiteral("transmit")));
        }
    }

    m_transmitLists->setExpanded(true);
}

void ProjectExplorerPanel::setScripts(const QList<ScriptExplorerItem>& scripts)
{
    m_scriptItems = scripts;
    if (m_scripts == nullptr) {
        return;
    }

    const QList<QTreeWidgetItem*> previous = m_scripts->takeChildren();
    qDeleteAll(previous);

    if (scripts.isEmpty()) {
        addEmptyPlaceholder(m_scripts, tr("No scripts in pipeline"), palette());
        return;
    }

    ThemeManager* themes = ThemeManager::instance();
    for (const ScriptExplorerItem& script : scripts) {
        auto* item = new QTreeWidgetItem(m_scripts);
        item->setText(0, script.label);
        item->setToolTip(0, script.toolTip.isEmpty() ? script.label : script.toolTip);
        item->setData(0, kScriptNodeIdRole, script.nodeId);
        if (themes != nullptr) {
            item->setIcon(0, themes->icon(QStringLiteral("console")));
        }
    }

    m_scripts->setExpanded(true);
}

void ProjectExplorerPanel::setLogs(const QList<LogExplorerItem>& logs)
{
    m_logItems = logs;
    if (m_logs == nullptr) {
        return;
    }

    const QList<QTreeWidgetItem*> previous = m_logs->takeChildren();
    qDeleteAll(previous);

    if (logs.isEmpty()) {
        addEmptyPlaceholder(m_logs, tr("No active log or logger block"), palette());
        return;
    }

    ThemeManager* themes = ThemeManager::instance();
    for (const LogExplorerItem& log : logs) {
        auto* item = new QTreeWidgetItem(m_logs);
        item->setText(0, log.label);
        item->setToolTip(0, log.toolTip.isEmpty() ? log.label : log.toolTip);
        if (!log.nodeId.isEmpty()) {
            item->setData(0, kLogNodeIdRole, log.nodeId);
        }
        if (log.isRecordAction) {
            item->setData(0, kLogRecordActionRole, true);
        }
        if (themes != nullptr) {
            const QString iconName =
                log.isRecordAction ? QStringLiteral("record") : QStringLiteral("save");
            item->setIcon(0, themes->icon(iconName));
        }
    }

    m_logs->setExpanded(true);
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

void ProjectExplorerPanel::onItemClicked(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }

    if (const QVariant dbPath = item->data(0, kDatabasePathRole); dbPath.isValid()) {
        Q_EMIT databaseSelected(dbPath.toString());
        return;
    }

    if (const QVariant txIdx = item->data(0, kTransmitIndexRole); txIdx.isValid()) {
        Q_EMIT transmitEntrySelected(txIdx.toInt());
        return;
    }

    if (const QVariant scriptId = item->data(0, kScriptNodeIdRole); scriptId.isValid()) {
        Q_EMIT scriptSelected(scriptId.toString());
        return;
    }

    if (item->data(0, kLogRecordActionRole).toBool()) {
        Q_EMIT recordRequested();
        return;
    }

    if (const QVariant logNodeId = item->data(0, kLogNodeIdRole); logNodeId.isValid()) {
        Q_EMIT logNodeSelected(logNodeId.toString());
        return;
    }
}

} // namespace torquebus::ui
