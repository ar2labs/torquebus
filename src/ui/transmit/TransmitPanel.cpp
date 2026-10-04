// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/TransmitPanel.h"

#include "core/transmit/TransmitList.h"
#include "ui/theme/ThemeManager.h"
#include "ui/transmit/SignalValueDialog.h"

#include <QAction>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnEnabled = 0,
    ColumnChannel,
    ColumnName,
    ColumnIdentifier,
    ColumnFormat,
    ColumnLength,
    ColumnData,
    ColumnPeriodic,
    ColumnCycle,
    ColumnCount,
    ColumnCountTotal
};

/// How often the counter column is repainted.
///
/// Five times a second. The count is a "yes, this is running" indicator, not a
/// measurement - a user reading it wants to see it move, not to read a precise
/// value, and 25 Hz would repaint a table nobody is looking at that closely.
constexpr int kRefreshMs = 200;

[[nodiscard]] QString formatIdentifier(const CanFrame& frame)
{
    return frame.format == CanFrameFormat::Extended
               ? QStringLiteral("%1").arg(frame.identifier, 8, 16, QLatin1Char('0')).toUpper()
               : QStringLiteral("%1").arg(frame.identifier, 3, 16, QLatin1Char('0')).toUpper();
}

[[nodiscard]] QString formatPayload(const CanFrame& frame)
{
    QStringList bytes;
    for (std::size_t i = 0; i < frame.length; ++i) {
        bytes << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(' '));
}

/// Reads "0x101", "101" or " 101 " as 0x101.
///
/// Hex without a prefix, because a CAN identifier is always written in hex and
/// a panel that needed 0x on every row would be asking the user to type
/// punctuation for the tool's benefit.
[[nodiscard]] bool parseIdentifier(QString text, std::uint32_t& out)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }

    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok) {
        return false;
    }

    out = static_cast<std::uint32_t>(value);
    return true;
}

/// Reads "52 03 00" or "520300" into the payload.
///
/// Refuses a length the frame cannot carry: 8 bytes at most for a classic frame,
/// and for an FD frame only the sizes a DLC can encode (0-8, 12, 16, 20, 24, 32,
/// 48, 64). Anything else would be silently padded or truncated on the way out,
/// and the row would say one thing while the bus carried another.
[[nodiscard]] bool parsePayload(QString text, CanFrame& frame)
{
    text.remove(QLatin1Char(' '));
    text.remove(QLatin1Char('\t'));

    if (text.size() % 2 != 0) {
        return false;
    }

    const int count = text.size() / 2;
    if (count > static_cast<int>(kMaxCanPayload)) {
        return false;
    }

    const auto length = static_cast<std::uint8_t>(count);
    const std::uint8_t dlc = dlcFromPayloadLength(length, frame.fd);
    if (payloadLengthFromDlc(dlc, frame.fd) != length) {
        return false;
    }

    std::array<std::uint8_t, kMaxCanPayload> bytes{};
    for (int i = 0; i < count; ++i) {
        bool ok = false;
        const uint value = text.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok) {
            return false;
        }
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
    }

    frame.data = bytes;
    frame.length = length;
    frame.dlc = dlc;
    return true;
}

} // namespace

TransmitPanel::TransmitPanel(TransmitList& list, QWidget* parent)
    : QWidget{parent}
    , m_list{list}
{
    buildUi();
    reload();

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &TransmitPanel::refreshCounters);
    m_timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }
}

void TransmitPanel::buildUi()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.transmit"));
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_actionAdd = m_toolBar->addAction(tr("Add"));
    m_actionAdd->setToolTip(tr("Add an empty row, to be filled in by hand."));

    m_actionAddFromMessage = m_toolBar->addAction(tr("Add from message..."));
    m_actionAddFromMessage->setToolTip(
        tr("Add a row shaped like a message from a loaded database."));

    // Until a database is imported there is nothing to add from; setDatabases()
    // switches it on.
    m_actionAddFromMessage->setEnabled(false);

    m_actionRemove = m_toolBar->addAction(tr("Remove"));
    m_toolBar->addSeparator();

    m_actionSend = m_toolBar->addAction(tr("Send"));
    m_actionSend->setToolTip(tr("Send the selected row once, now."));

    m_actionEditSignals = m_toolBar->addAction(tr("Signals..."));
    m_actionEditSignals->setToolTip(
        tr("Edit the row's payload by signal name, using its database."));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCountTotal);
    m_table->setHorizontalHeaderLabels({tr("On"),
                                        tr("Ch"),
                                        tr("Name"),
                                        tr("ID"),
                                        tr("Fmt"),
                                        tr("DLC"),
                                        tr("Data"),
                                        tr("Cyc"),
                                        tr("ms"),
                                        tr("Count")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(ColumnEnabled, 34);
    m_table->setColumnWidth(ColumnChannel, 38);
    m_table->setColumnWidth(ColumnName, 150);
    m_table->setColumnWidth(ColumnIdentifier, 80);
    m_table->setColumnWidth(ColumnFormat, 46);
    m_table->setColumnWidth(ColumnLength, 44);
    m_table->setColumnWidth(ColumnData, 240);
    m_table->setColumnWidth(ColumnPeriodic, 40);
    m_table->setColumnWidth(ColumnCycle, 60);
    m_table->setColumnWidth(ColumnCount, 80);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 6, 4);
    layout->setSpacing(2);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_status);

    connect(m_actionAdd, &QAction::triggered, this, &TransmitPanel::onAdd);
    connect(m_actionAddFromMessage, &QAction::triggered, this, &TransmitPanel::onAddFromMessage);
    connect(m_actionRemove, &QAction::triggered, this, &TransmitPanel::onRemove);
    connect(m_actionSend, &QAction::triggered, this, &TransmitPanel::onSendSelected);
    connect(m_actionEditSignals, &QAction::triggered, this, &TransmitPanel::onEditSignals);

    // Editing by name is only meaningful for a row that came from a message,
    // so the button follows the selection rather than being always live and
    // sometimes complaining.
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { updateActionState(); });
    connect(m_table, &QTableWidget::itemChanged, this, &TransmitPanel::onItemChanged);
}

void TransmitPanel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_databases = std::move(databases);

    // Nothing to add *from* until a database is loaded, and an enabled button
    // that can only ever say "no databases" is a button that wastes a click.
    m_actionAddFromMessage->setEnabled(!m_databases.empty());

    // A row may have become editable by name because its database has just
    // arrived, so the selection-driven state is re-evaluated. Called directly:
    // a signal belongs to the object that declares it, and reaching in to emit
    // another widget's is not something a caller may do.
    updateActionState();
}

void TransmitPanel::reload()
{
    const std::vector<TransmitEntry> entries = m_list.entries();

    m_populating = true;
    m_table->setRowCount(static_cast<int>(entries.size()));

    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const TransmitEntry& entry = entries[static_cast<std::size_t>(row)];

        auto* enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        enabled->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, ColumnEnabled, enabled);

        // 1-based, because the Project Explorer and the status bar both say
        // "CAN 1" for channel 0 and a panel that said 0 would be the only place
        // in the window counting differently.
        m_table->setItem(
            row, ColumnChannel, new QTableWidgetItem{QString::number(entry.channel + 1)});

        m_table->setItem(row, ColumnName, new QTableWidgetItem{QString::fromStdString(entry.name)});
        m_table->setItem(
            row, ColumnIdentifier, new QTableWidgetItem{formatIdentifier(entry.frame)});

        m_table->setItem(row,
                         ColumnFormat,
                         new QTableWidgetItem{entry.frame.isExtended() ? tr("Ext") : tr("Std")});
        // Length is derived from the data, so it is shown and not typed. Its
        // flags are set *before* the item goes into the table: QTableWidgetItem
        // ::setFlags on an item that is already in a table emits itemChanged,
        // and doing it afterwards - which this used to, in a loop after
        // m_populating had been cleared - made every reload commit every row,
        // which reloaded, which did it again, until the stack ran out. That is
        // what closed the application on Add.
        auto* length = new QTableWidgetItem{QString::number(entry.frame.length)};
        length->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, ColumnLength, length);

        m_table->setItem(row, ColumnData, new QTableWidgetItem{formatPayload(entry.frame)});

        auto* periodic = new QTableWidgetItem;
        periodic->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        periodic->setCheckState(entry.isPeriodic() ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, ColumnPeriodic, periodic);

        m_table->setItem(row, ColumnCycle, new QTableWidgetItem{QString::number(entry.cycleMs)});

        // The count is the executor's, so it is not editable here. A cell the
        // user can type into but that is overwritten five times a second is a
        // cell that eats what they typed.
        auto* count = new QTableWidgetItem{QString::number(entry.sentCount)};
        count->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, ColumnCount, count);
    }

    m_populating = false;

    m_status->setText(tr("%n row(s)", nullptr, m_table->rowCount()));

    updateActionState();
}

void TransmitPanel::refreshCounters()
{
    const std::vector<TransmitEntry> entries = m_list.entries();

    // A row added or removed under us - by loading a project, say - means the
    // table is stale in a way a counter refresh cannot fix.
    if (entries.size() != static_cast<std::size_t>(m_table->rowCount())) {
        reload();
        return;
    }

    m_populating = true;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString sent = QString::number(entries[static_cast<std::size_t>(row)].sentCount);

        if (QTableWidgetItem* item = m_table->item(row, ColumnCount); item != nullptr) {
            // Only when it changed. Writing the same string still emits
            // itemChanged and still repaints the cell.
            if (item->text() != sent) {
                item->setText(sent);
            }
        }
    }
    m_populating = false;
}

void TransmitPanel::onAdd()
{
    TransmitEntry entry;
    entry.name = tr("New frame").toStdString();
    entry.frame.identifier = 0x100;
    entry.frame.length = 8;
    entry.frame.dlc = 8;

    // Manual, deliberately. See the header: a row that starts transmitting the
    // moment it appears puts traffic on a bus nobody was ready for.
    entry.trigger = TransmitTrigger::Manual;

    (void)m_list.add(std::move(entry));
    reload();

    m_table->selectRow(m_table->rowCount() - 1);
}

void TransmitPanel::onAddFromMessage()
{
    QStringList choices;
    std::vector<const CanMessage*> messages;

    for (const std::shared_ptr<const CanDatabase>& database : m_databases) {
        for (const CanMessage& message : database->messages()) {
            choices << QStringLiteral("%1  (%2)")
                           .arg(QString::fromStdString(message.name),
                                formatIdentifier(message.makeFrame()));
            messages.push_back(&message);
        }
    }

    if (choices.isEmpty()) {
        Q_EMIT reported(tr("No messages: import a database first."));
        return;
    }

    bool accepted = false;
    const QString picked = QInputDialog::getItem(
        this, tr("Add from message"), tr("Message:"), choices, 0, false, &accepted);
    if (!accepted) {
        return;
    }

    const int index = choices.indexOf(picked);
    if (index < 0) {
        return;
    }

    const CanMessage* message = messages[static_cast<std::size_t>(index)];

    TransmitEntry entry;
    entry.name = message->name;
    entry.messageName = message->name;
    entry.frame = message->makeFrame();

    // The database's own cycle time when it declares one. A message that says
    // GenMsgCycleTime 100 is telling us how it is meant to be sent, and making
    // the user retype it would be asking them for something already in the
    // file. Still Manual, though - the period is a suggestion, transmitting is
    // a decision.
    if (message->cycleTimeMs > 0) {
        entry.cycleMs = message->cycleTimeMs;
    }

    (void)m_list.add(std::move(entry));
    reload();

    m_table->selectRow(m_table->rowCount() - 1);
}

void TransmitPanel::onRemove()
{
    const int row = selectedRow();
    if (row < 0) {
        return;
    }

    m_list.remove(static_cast<std::size_t>(row));
    reload();

    if (m_table->rowCount() > 0) {
        m_table->selectRow(std::min(row, m_table->rowCount() - 1));
    }
}

void TransmitPanel::onSendSelected()
{
    const int row = selectedRow();
    if (row < 0) {
        Q_EMIT reported(tr("Select a row to send."));
        return;
    }

    if (!m_list.sendOnce(static_cast<std::size_t>(row))) {
        // Said, not swallowed. The two reasons a send is refused are a full
        // queue and a disabled row, and both look identical from the outside -
        // nothing happens - unless the panel says which.
        Q_EMIT reported(tr("Row %1 was not sent: it is switched off, or the "
                           "send queue is full.")
                            .arg(row + 1));
    }
}

const CanMessage* TransmitPanel::messageFor(const TransmitEntry& entry) const
{
    if (entry.messageName.empty()) {
        return nullptr;
    }

    for (const std::shared_ptr<const CanDatabase>& database : m_databases) {
        if (const CanMessage* message = database->findByName(entry.messageName)) {
            return message;
        }
    }

    return nullptr;
}

void TransmitPanel::onEditSignals()
{
    const int row = selectedRow();
    if (row < 0) {
        return;
    }

    TransmitEntry entry;
    if (!m_list.entryAt(static_cast<std::size_t>(row), entry)) {
        return;
    }

    const CanMessage* message = messageFor(entry);
    if (message == nullptr) {
        // The row remembers a name, and no database currently loaded has it.
        // Worth saying which, because the fix is to import a file rather than
        // to change anything about the row.
        Q_EMIT reported(tr("Row %1 was built from '%2', which no loaded database "
                           "describes. Import it and try again.")
                            .arg(row + 1)
                            .arg(QString::fromStdString(entry.messageName)));
        return;
    }

    SignalValueDialog dialog{*message, entry.frame, this};
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    entry.frame = dialog.frame();
    m_list.update(static_cast<std::size_t>(row), entry);

    reload();
    m_table->selectRow(row);
}

void TransmitPanel::updateActionState()
{
    const int row = selectedRow();

    TransmitEntry entry;
    const bool exists = row >= 0 && m_list.entryAt(static_cast<std::size_t>(row), entry);
    const bool known = exists && messageFor(entry) != nullptr;

    m_actionEditSignals->setEnabled(known);
    m_actionRemove->setEnabled(exists);
    m_actionSend->setEnabled(exists);
}

int TransmitPanel::selectedRow() const
{
    const QList<QTableWidgetItem*> selected = m_table->selectedItems();
    return selected.isEmpty() ? -1 : selected.front()->row();
}

void TransmitPanel::onItemChanged(QTableWidgetItem* item)
{
    if (m_populating || item == nullptr) {
        return;
    }

    commitRow(item->row());
}

void TransmitPanel::commitRow(int row)
{
    const auto index = static_cast<std::size_t>(row);

    TransmitEntry entry;
    if (!m_list.entryAt(index, entry)) {
        return;
    }

    bool rejected = false;

    if (const QTableWidgetItem* item = m_table->item(row, ColumnEnabled)) {
        entry.enabled = item->checkState() == Qt::Checked;
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnChannel)) {
        bool ok = false;
        const uint shown = item->text().toUInt(&ok);

        // Back to 0-based, and only for a channel that exists. Accepting CAN 9
        // on a two-channel setup would make a row that can never send and never
        // says why.
        if (ok && shown >= 1 && shown <= 255) {
            entry.channel = static_cast<std::uint8_t>(shown - 1);
        } else {
            rejected = true;
        }
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnName)) {
        entry.name = item->text().toStdString();
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnIdentifier)) {
        std::uint32_t identifier = 0;
        if (parseIdentifier(item->text(), identifier)
            && isValidIdentifier(identifier, entry.frame.format)) {
            entry.frame.identifier = identifier;
        } else {
            rejected = true;
        }
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnFormat)) {
        const QString text = item->text().trimmed();
        const bool extended = text.startsWith(QLatin1Char('E'), Qt::CaseInsensitive);
        const CanFrameFormat format =
            extended ? CanFrameFormat::Extended : CanFrameFormat::Standard;

        // Only if the identifier still fits. Switching a 29-bit identifier to
        // standard would otherwise leave a row that can never be sent.
        if (isValidIdentifier(entry.frame.identifier, format)) {
            entry.frame.format = format;
        } else {
            rejected = true;
        }
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnData)) {
        if (!parsePayload(item->text(), entry.frame)) {
            rejected = true;
        }
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnPeriodic)) {
        entry.trigger =
            item->checkState() == Qt::Checked ? TransmitTrigger::Periodic : TransmitTrigger::Manual;
    }

    if (const QTableWidgetItem* item = m_table->item(row, ColumnCycle)) {
        bool ok = false;
        const uint cycle = item->text().toUInt(&ok);
        if (ok) {
            entry.cycleMs = std::max<std::uint32_t>(cycle, TransmitList::kMinimumCycleMs);
        } else {
            rejected = true;
        }
    }

    m_list.update(index, entry);

    // Redrawn from what the list now holds, so a field that did not parse
    // visibly snaps back rather than sitting there looking committed. Somebody
    // believing they are transmitting on an identifier they are not is the
    // failure this prevents.
    //
    // Queued, not done here: this runs inside the itemChanged emission of a
    // cell, and reload() replaces every item in the table - including the one
    // still being reported. Qt does not promise that survives, and the cost of
    // waiting one pass of the event loop is not visible.
    if (!m_redrawPending) {
        m_redrawPending = true;
        QMetaObject::invokeMethod(
            this,
            [this, row] {
                m_redrawPending = false;
                reload();
                if (row < m_table->rowCount()) {
                    m_table->selectRow(row);
                }
            },
            Qt::QueuedConnection);
    }

    if (rejected) {
        Q_EMIT reported(tr("Row %1: some of that could not be read, and was put back. "
                           "Identifiers and data are hex; the cycle time is a number "
                           "of milliseconds.")
                            .arg(row + 1));
    }
}

void TransmitPanel::onThemeChanged()
{
    // Nothing to re-tint yet: the table takes its colours from the style sheet
    // and its own palette. Here so that a later icon column has an obvious
    // place to be re-coloured from, and so that the omission is deliberate
    // rather than forgotten.
}

} // namespace torquebus::ui
