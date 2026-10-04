// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/properties/PropertiesPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QHeaderView>
#include <QPalette>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

QString yesNo(bool value)
{
    return value ? PropertiesPanel::tr("Yes") : PropertiesPanel::tr("No");
}

[[nodiscard]] QString iconNameForProperty(const QString& name)
{
    if (name.contains(QLatin1String("Name"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Backend"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Handle"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Serial"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Type"), Qt::CaseInsensitive)) {
        return QStringLiteral("hardware");
    }
    if (name.contains(QLatin1String("CAN"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Channel"), Qt::CaseInsensitive)) {
        return QStringLiteral("network");
    }
    if (name.contains(QLatin1String("Bit rate"), Qt::CaseInsensitive)) {
        return QStringLiteral("gauge");
    }
    if (name.contains(QLatin1String("Filter"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("Listen"), Qt::CaseInsensitive)) {
        return QStringLiteral("filter");
    }
    if (name.contains(QLatin1String("timestamp"), Qt::CaseInsensitive)) {
        return QStringLiteral("time-format");
    }
    if (name.contains(QLatin1String("Error"), Qt::CaseInsensitive)) {
        return QStringLiteral("diagnostics");
    }
    return QStringLiteral("properties");
}

} // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget{parent}
{
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Property"), tr("Value")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] {
            if (m_rows.isEmpty()) {
                clearProperties();
            } else {
                setProperties(m_title, m_rows);
            }
        });
    }

    clearProperties();
}

void PropertiesPanel::setProperties(const QString& title, const QVector<Row>& rows)
{
    m_title = title;
    m_rows = rows;
    m_tree->clear();

    if (rows.isEmpty()) {
        clearProperties();
        return;
    }

    m_tree->setHeaderLabels({title.isEmpty() ? tr("Property") : title, tr("Value")});

    const ThemeManager* themes = ThemeManager::instance();
    if (themes != nullptr && m_tree->headerItem() != nullptr) {
        m_tree->headerItem()->setIcon(0, themes->icon(QStringLiteral("properties")));
    }

    for (const Row& row : rows) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, row.name);
        item->setText(1, row.value);
        item->setToolTip(1, row.value);

        if (themes != nullptr) {
            item->setIcon(0, themes->icon(iconNameForProperty(row.name)));
            if (row.highlighted) {
                item->setForeground(1, themes->theme().accent);
            }
        }
    }
}

void PropertiesPanel::setDevice(const CanDeviceInfo& device)
{
    const CanCapabilities& caps = device.capabilities;

    QVector<Row> rows;
    rows.append({tr("Name"), QString::fromStdString(device.name), true});
    rows.append({tr("Backend"), QString::fromStdString(device.backend), false});
    rows.append({tr("Handle"), QString::fromStdString(device.handle), false});
    rows.append({tr("Channel"), QString::number(device.channelIndex), false});

    if (!device.serialNumber.empty()) {
        rows.append({tr("Serial number"), QString::fromStdString(device.serialNumber), false});
    }

    rows.append({tr("Type"), caps.virtualDevice ? tr("Virtual") : tr("Physical"), false});
    rows.append({tr("CAN"), yesNo(caps.canClassic), false});
    rows.append({tr("CAN FD"), yesNo(caps.canFd), false});
    rows.append({tr("Bit rate switch"), yesNo(caps.canFdBrs), false});
    rows.append({tr("Listen only"), yesNo(caps.listenOnly), false});
    rows.append({tr("Hardware timestamp"), yesNo(caps.hardwareTimestamp), false});
    rows.append({tr("Error frames"), yesNo(caps.errorFrames), false});
    rows.append({tr("Hardware filters"), yesNo(caps.hardwareFilters), false});
    rows.append({tr("Channels"), QString::number(caps.maxChannels), false});

    setProperties(tr("Channel"), rows);
}

void PropertiesPanel::clearProperties()
{
    m_rows.clear();
    m_title.clear();
    m_tree->clear();
    m_tree->setHeaderLabels({tr("Property"), tr("Value")});

    if (const ThemeManager* themes = ThemeManager::instance()) {
        if (m_tree->headerItem() != nullptr) {
            m_tree->headerItem()->setIcon(0, themes->icon(QStringLiteral("properties")));
        }
    }

    auto* item = new QTreeWidgetItem(m_tree);
    item->setText(0, tr("Nothing selected"));
    item->setFirstColumnSpanned(true);
    item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
}

} // namespace torquebus::ui
