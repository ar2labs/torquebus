// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/hardware/HardwareDialog.h"

#include "services/HardwareProfile.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnUse = 0,
    ColumnChannel,
    ColumnDevice,
    ColumnBackend,
    ColumnBitrate,
    ColumnFd,
    ColumnListenOnly,
    ColumnCount,
};

/// A checkbox centred in its cell.
///
/// A checkable QTableWidgetItem would be less code, but its box sits against
/// the left edge of a cell whose heading is centred, and the item is also
/// editable text somebody can double-click into. This reads as a control
/// because it is one.
[[nodiscard]] QWidget* checkBoxCell(bool checked, bool enabled, QCheckBox** out)
{
    auto* holder = new QWidget;

    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignCenter);

    auto* box = new QCheckBox;
    box->setChecked(checked);
    box->setEnabled(enabled);
    layout->addWidget(box);

    *out = box;
    return holder;
}

} // namespace

HardwareDialog::HardwareDialog(const CanDeviceInfoList& devices,
                               services::HardwareProfile& profile,
                               bool measurementRunning,
                               QWidget* parent)
    : QDialog{parent}
    , m_profile{profile}
    , m_readOnly{measurementRunning}
{
    setWindowTitle(tr("Hardware Configuration"));
    setModal(true);
    resize(760, 420);

    QStringList detected;
    detected.reserve(static_cast<qsizetype>(devices.size()));

    for (const CanDeviceInfo& device : devices) {
        detected.append(QString::fromStdString(device.handle));
    }

    // Rows in the order the profile says, which for a machine nobody has
    // arranged is the order they were detected in.
    for (const QString& handle : services::HardwareProfile::arrange(detected, profile.order())) {
        const auto found =
            std::find_if(devices.begin(), devices.end(), [&handle](const CanDeviceInfo& device) {
                return QString::fromStdString(device.handle) == handle;
            });

        if (found == devices.end()) {
            continue; // Cannot happen: the order was arranged from these devices.
        }

        const services::ChannelPreferences preferences = profile.preferencesFor(handle);

        Row row;
        row.handle = handle;
        row.name = QString::fromStdString(found->name);
        row.backend = QString::fromStdString(found->backend);

        row.enabled = preferences.enabled;
        row.bitrate = preferences.bitrate;

        row.supportsFd = found->capabilities.canFd;
        row.supportsListenOnly = found->capabilities.listenOnly;

        // A stored setting the adapter cannot honour is shown as off rather
        // than as a tick that will be ignored: the settings file may have been
        // written when a different adapter held this handle.
        row.canFd = preferences.canFd && row.supportsFd;
        row.listenOnly = preferences.listenOnly && row.supportsListenOnly;

        m_rows.append(row);
    }

    buildUi(measurementRunning);
    fillTable();
    onSelectionChanged();
}

void HardwareDialog::buildUi(bool measurementRunning)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("preferencesHint"));
    m_hint->setWordWrap(true);

    if (measurementRunning) {
        m_hint->setText(tr("A measurement is running. Interfaces are opened at Start, and "
                           "the channel numbers are what the trace already shows - so this "
                           "can be read but not changed until you press Stop."));
    } else {
        m_hint->setText(tr("The order is the numbering: the first interface in use is CAN 1. "
                           "Settings are remembered per adapter, so unplugging one and "
                           "plugging it back in somewhere else keeps its rate."));
    }

    layout->addWidget(m_hint);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({tr("Use"),
                                        tr("Channel"),
                                        tr("Interface"),
                                        tr("Driver"),
                                        tr("Bitrate"),
                                        tr("CAN FD"),
                                        tr("Listen only")});

    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    connect(
        m_table, &QTableWidget::itemSelectionChanged, this, &HardwareDialog::onSelectionChanged);

    layout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;

    m_up = new QPushButton(tr("Move &up"), this);
    m_down = new QPushButton(tr("Move &down"), this);

    for (QPushButton* button : {m_up, m_down}) {
        button->setAutoDefault(false);
        button->setEnabled(false);
        buttons->addWidget(button);
    }

    connect(m_up, &QPushButton::clicked, this, &HardwareDialog::onMoveUp);
    connect(m_down, &QPushButton::clicked, this, &HardwareDialog::onMoveDown);

    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &HardwareDialog::onAccepted);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (measurementRunning) {
        // Cancel only. An OK that silently did nothing would be worse than not
        // offering one.
        box->button(QDialogButtonBox::Ok)->setEnabled(false);
    }

    layout->addWidget(box);
}

void HardwareDialog::fillTable()
{
    m_table->setRowCount(m_rows.size());

    for (int index = 0; index < m_rows.size(); ++index) {
        const Row& row = m_rows.at(index);

        QCheckBox* use = nullptr;
        m_table->setCellWidget(index, ColumnUse, checkBoxCell(row.enabled, !m_readOnly, &use));

        connect(use, &QCheckBox::toggled, this, [this, index](bool on) {
            m_rows[index].enabled = on;

            // Only the numbering changes, so only the numbering is redrawn -
            // rebuilding here would delete the checkbox that is mid-signal.
            renumber();
        });

        auto* channel = new QTableWidgetItem;
        channel->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(index, ColumnChannel, channel);

        auto* name = new QTableWidgetItem(row.name);
        // The handle in the tooltip: two adapters of the same model share a
        // name and differ by handle, and the handle is what settings and
        // projects are keyed on.
        name->setToolTip(row.handle);
        m_table->setItem(index, ColumnDevice, name);

        m_table->setItem(index, ColumnBackend, new QTableWidgetItem(row.backend));

        auto* bitrate = new QComboBox;
        bitrate->setEnabled(!m_readOnly);

        for (const std::uint32_t rate : standardBitrates()) {
            bitrate->addItem(QString::fromStdString(describeBitrate(rate)),
                             static_cast<uint>(rate));
        }

        if (const int found = bitrate->findData(static_cast<uint>(row.bitrate)); found >= 0) {
            bitrate->setCurrentIndex(found);
        }

        connect(bitrate, &QComboBox::currentIndexChanged, this, [this, bitrate, index](int) {
            m_rows[index].bitrate = static_cast<std::uint32_t>(bitrate->currentData().toUInt());
        });

        m_table->setCellWidget(index, ColumnBitrate, bitrate);

        QCheckBox* fd = nullptr;
        m_table->setCellWidget(
            index, ColumnFd, checkBoxCell(row.canFd, !m_readOnly && row.supportsFd, &fd));

        connect(
            fd, &QCheckBox::toggled, this, [this, index](bool on) { m_rows[index].canFd = on; });

        if (!row.supportsFd) {
            fd->setToolTip(tr("This adapter does not do CAN FD."));
        }

        QCheckBox* listenOnly = nullptr;
        m_table->setCellWidget(
            index,
            ColumnListenOnly,
            checkBoxCell(row.listenOnly, !m_readOnly && row.supportsListenOnly, &listenOnly));

        connect(listenOnly, &QCheckBox::toggled, this, [this, index](bool on) {
            m_rows[index].listenOnly = on;
        });

        if (!row.supportsListenOnly) {
            listenOnly->setToolTip(tr("This driver has no listen-only mode."));
        }
    }

    renumber();

    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(ColumnDevice, QHeaderView::Stretch);
}

void HardwareDialog::renumber()
{
    int channel = 1;

    for (int index = 0; index < m_rows.size(); ++index) {
        QTableWidgetItem* item = m_table->item(index, ColumnChannel);
        if (item == nullptr) {
            continue;
        }

        if (!m_rows.at(index).enabled) {
            // An interface that is switched off takes no number and does not
            // push the ones below it down one - which is what lets somebody
            // turn off CAN 1 without renaming everything they were reading.
            item->setText(QStringLiteral("-"));
            continue;
        }

        item->setText(tr("CAN %1").arg(channel));
        ++channel;
    }
}

void HardwareDialog::onSelectionChanged()
{
    const int row = m_table->currentRow();
    const bool movable = !m_readOnly && row >= 0;

    m_up->setEnabled(movable && row > 0);
    m_down->setEnabled(movable && row < m_rows.size() - 1);
}

void HardwareDialog::onMoveUp()
{
    moveSelected(-1);
}

void HardwareDialog::onMoveDown()
{
    moveSelected(1);
}

void HardwareDialog::moveSelected(int delta)
{
    const int row = m_table->currentRow();
    const int target = row + delta;

    if (row < 0 || target < 0 || target >= m_rows.size()) {
        return;
    }

    // The rows are what moves; the table is rebuilt from them. Shuffling live
    // cell widgets between rows looks cheaper and is not: setCellWidget deletes
    // whatever the cell held, so the obvious version of that loop destroys the
    // very controls it is trying to move.
    m_rows.swapItemsAt(row, target);
    fillTable();

    m_table->setCurrentCell(target, ColumnDevice);
    onSelectionChanged();
}

void HardwareDialog::onAccepted()
{
    if (m_readOnly) {
        reject();
        return;
    }

    QStringList order;
    order.reserve(m_rows.size());

    for (const Row& row : m_rows) {
        const services::ChannelPreferences before = m_profile.preferencesFor(row.handle);

        services::ChannelPreferences wanted = before;
        wanted.enabled = row.enabled;
        wanted.bitrate = row.bitrate;
        wanted.canFd = row.canFd;
        wanted.listenOnly = row.listenOnly;

        // Written only when something changed, so that opening the dialog and
        // pressing OK does not rewrite the settings file - and, more to the
        // point, does not report a change that would rebind every interface.
        if (wanted.enabled != before.enabled || wanted.bitrate != before.bitrate
            || wanted.canFd != before.canFd || wanted.listenOnly != before.listenOnly) {
            m_profile.setPreferencesFor(row.handle, wanted);
            m_changed = true;
        }

        order.append(row.handle);
    }

    if (order != m_profile.order()) {
        m_profile.setOrder(order);
        m_changed = true;
    }

    accept();
}

} // namespace torquebus::ui
