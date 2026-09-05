// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/SignalValueDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnSignal = 0,
    ColumnValue,
    ColumnUnit,
    ColumnRange,
    ColumnCountTotal
};

/// The signal a row stands for, kept on the row rather than looked up by name.
constexpr int kSignalIndexRole = Qt::UserRole + 1;

[[nodiscard]] QString formatPayload(const CanFrame& frame)
{
    QStringList bytes;
    for (std::size_t i = 0; i < frame.length; ++i) {
        bytes << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(' '));
}

/// "0 .. 250 km/h", or empty when the database declares no range.
[[nodiscard]] QString formatRange(const CanSignal& signal)
{
    if (signal.minimum == 0.0 && signal.maximum == 0.0) {
        return {};
    }

    return QStringLiteral("%1 .. %2")
        .arg(signal.minimum, 0, 'g', 10)
        .arg(signal.maximum, 0, 'g', 10);
}

} // namespace

SignalValueDialog::SignalValueDialog(const CanMessage& message, CanFrame frame, QWidget* parent)
    : QDialog{parent}
    , m_message{message}
    , m_frame{frame}
{
    setWindowTitle(tr("Signals - %1").arg(QString::fromStdString(message.name)));
    resize(520, 360);

    buildUi();
    reload();
}

void SignalValueDialog::buildUi()
{
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCountTotal);
    m_table->setHorizontalHeaderLabels({tr("Signal"), tr("Value"), tr("Unit"), tr("Range")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(ColumnSignal, 180);
    m_table->setColumnWidth(ColumnValue, 110);
    m_table->setColumnWidth(ColumnUnit, 60);
    m_table->horizontalHeader()->setStretchLastSection(true);

    // The bytes, so the effect of an edit is visible without leaving the
    // dialog. It is the thing that actually goes on the bus, and watching it
    // change while typing a physical value is how somebody learns to trust the
    // encoding rather than checking it by hand every time.
    m_payload = new QLabel(this);
    m_payload->setObjectName(QStringLiteral("panelStatus"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_payload);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemChanged, this, &SignalValueDialog::onItemChanged);
}

void SignalValueDialog::reload()
{
    // Which signals this frame carries, given whatever the multiplexer switch
    // currently reads. Recomputed on every reload, so changing the switch
    // changes the set of editable rows immediately.
    const std::vector<const CanSignal*> present =
        m_message.signalsIn(m_frame.data.data(), m_frame.length);

    m_populating = true;
    m_table->setRowCount(static_cast<int>(m_message.signalList.size()));

    for (int row = 0; row < static_cast<int>(m_message.signalList.size()); ++row) {
        const CanSignal& signal = m_message.signalList[static_cast<std::size_t>(row)];

        const bool carried =
            std::find(present.begin(), present.end(), &signal) != present.end();
        const bool fits = signal.fitsIn(m_frame.length);
        const bool editable = carried && fits;

        auto* name = new QTableWidgetItem{QString::fromStdString(signal.name)};
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, ColumnSignal, name);

        QString text;
        if (!fits) {
            // The database and the frame disagree about length. Saying so beats
            // showing a zero that looks like a reading.
            text = tr("(does not fit)");
        } else if (!carried) {
            text = tr("(not on this page)");
        } else {
            const std::int64_t raw = signal.rawValue(m_frame.data.data(), m_frame.length);
            const double value = static_cast<double>(raw) * signal.factor + signal.offset;

            // A value the database has a word for is shown as the word beside
            // the number - "3 (Reverse)". The number stays because it is what
            // has to be typed back in.
            const std::string_view named = signal.nameForValue(raw);
            text = named.empty()
                       ? QString::number(value, 'g', 10)
                       : QStringLiteral("%1  (%2)")
                             .arg(QString::number(value, 'g', 10),
                                  QString::fromUtf8(named.data(),
                                                    static_cast<int>(named.size())));
        }

        auto* value = new QTableWidgetItem{text};
        if (!editable) {
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        value->setData(kSignalIndexRole, row);
        m_table->setItem(row, ColumnValue, value);

        auto* unit = new QTableWidgetItem{QString::fromStdString(signal.unit)};
        unit->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, ColumnUnit, unit);

        auto* range = new QTableWidgetItem{formatRange(signal)};
        range->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, ColumnRange, range);
    }

    m_populating = false;

    m_payload->setText(tr("Payload: %1").arg(formatPayload(m_frame)));
}

void SignalValueDialog::onItemChanged(QTableWidgetItem* item)
{
    if (m_populating || item == nullptr || item->column() != ColumnValue) {
        return;
    }

    const int row = item->data(kSignalIndexRole).toInt();
    if (row < 0 || row >= static_cast<int>(m_message.signalList.size())) {
        return;
    }

    const CanSignal& signal = m_message.signalList[static_cast<std::size_t>(row)];

    // Only the number is read. A cell showing "3  (Reverse)" is the dialog's
    // own formatting, and a user who edits the 3 in place should not have to
    // delete the word first.
    QString text = item->text().trimmed();
    if (const int space = text.indexOf(QLatin1Char(' ')); space > 0) {
        text = text.left(space);
    }

    bool ok = false;
    const double value = text.toDouble(&ok);

    if (ok) {
        // encode saturates and returns false when the value does not fit the
        // field. Not an error worth a dialog: the reload below shows what
        // actually landed, which says it better than a message box would.
        (void)signal.encode(value, m_frame.data.data(), m_frame.length);
    }

    // Rebuilt from the frame either way - so a value that would not parse
    // snaps back, and a value that was quantised or saturated shows what the
    // bus will really carry rather than what was typed. Also re-evaluates the
    // multiplexer, because the edit may have been to the switch itself.
    reload();
}

} // namespace torquebus::ui
