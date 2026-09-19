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
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);

    clearProperties();
}

void PropertiesPanel::setProperties(const QString& title, const QVector<Row>& rows)
{
    m_tree->clear();

    if (rows.isEmpty()) {
        clearProperties();
        return;
    }

    m_tree->setHeaderLabels({title.isEmpty() ? tr("Property") : title, tr("Value")});

    const ThemeManager* themes = ThemeManager::instance();

    for (const Row& row : rows) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, row.name);
        item->setText(1, row.value);
        item->setToolTip(1, row.value);

        if (row.highlighted && themes != nullptr) {
            item->setForeground(1, themes->theme().accent);
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
    m_tree->clear();
    m_tree->setHeaderLabels({tr("Property"), tr("Value")});

    auto* item = new QTreeWidgetItem(m_tree);
    item->setText(0, tr("Nothing selected"));
    item->setFirstColumnSpanned(true);
    item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
}

} // namespace torquebus::ui
