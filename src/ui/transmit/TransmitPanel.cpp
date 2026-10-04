// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/TransmitPanel.h"

#include "core/transmit/TransmitList.h"
#include "ui/theme/ThemeManager.h"
#include "ui/transmit/SignalValueDialog.h"
#include "ui/transmit/TransmitTreeDelegate.h"
#include "ui/transmit/TransmitTreeFilterModel.h"
#include "ui/transmit/TransmitTreeModel.h"

#include <QAction>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace torquebus::ui {
using enum TransmitTreeModel::Column;

namespace {

constexpr int kTransmitColumnWidths[TransmitColumnCount] = {
    48, // On (Checkbox + expander arrow)
    55, // Bus ("1")
    75, // Type ("STD", "EXT", "FD")
    105, // CAN-ID ("655", "18FEF100")
    65, // Length ("8")
    240, // Symbol ("✉ Out_RTC_SetTime" / "🏷️ RTC_SetHour")
    270, // Data ("00 0C 11 00 19 0B E8 07" / "= 17")
    105, // Cycle Time ("Wait" / "100 ms")
    85 // Count ("0", "1540")
};

constexpr int kRefreshMs = 200;

[[nodiscard]] QString formatIdentifier(const CanFrame& frame)
{
    return frame.format == CanFrameFormat::Extended
               ? QStringLiteral("%1").arg(frame.identifier, 8, 16, QLatin1Char('0')).toUpper()
               : QStringLiteral("%1").arg(frame.identifier, 3, 16, QLatin1Char('0')).toUpper();
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
        connect(themes, &ThemeManager::themeChanged, this, &TransmitPanel::onThemeChanged);
    }
}

void TransmitPanel::buildUi()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.transmit"));
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    ThemeManager* themes = ThemeManager::instance();
    const auto icon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    m_actionAdd = new QAction(icon("new"), tr("Add"), this);
    m_actionAdd->setToolTip(tr("Add an empty row, to be filled in by hand."));
    connect(m_actionAdd, &QAction::triggered, this, &TransmitPanel::onAdd);

    m_actionAddFromMessage = new QAction(icon("database"), tr("Add from message..."), this);
    m_actionAddFromMessage->setToolTip(
        tr("Add a row shaped like a message from a loaded database."));
    m_actionAddFromMessage->setEnabled(false);
    connect(m_actionAddFromMessage, &QAction::triggered, this, &TransmitPanel::onAddFromMessage);

    m_actionRemove = new QAction(icon("clear"), tr("Remove"), this);
    connect(m_actionRemove, &QAction::triggered, this, &TransmitPanel::onRemove);

    m_actionSend = new QAction(icon("transmit"), tr("Send"), this);
    m_actionSend->setToolTip(tr("Send the selected row once, now (Space)."));
    connect(m_actionSend, &QAction::triggered, this, &TransmitPanel::onSendSelected);

    m_actionEditSignals = new QAction(icon("properties"), tr("Signals..."), this);
    m_actionEditSignals->setToolTip(
        tr("Edit the row's payload by signal name, using its database."));
    connect(m_actionEditSignals, &QAction::triggered, this, &TransmitPanel::onEditSignals);

    m_actionExpandAll = new QAction(icon("panel-expand"), tr("Expand All"), this);
    m_actionExpandAll->setToolTip(tr("Expand all messages to show signals"));
    connect(m_actionExpandAll, &QAction::triggered, this, &TransmitPanel::onExpandAll);

    m_actionCollapseAll = new QAction(icon("panel-collapse"), tr("Collapse All"), this);
    m_actionCollapseAll->setToolTip(tr("Collapse signals"));
    connect(m_actionCollapseAll, &QAction::triggered, this, &TransmitPanel::onCollapseAll);

    m_toolBar->addAction(m_actionAdd);
    m_toolBar->addAction(m_actionAddFromMessage);
    m_toolBar->addAction(m_actionRemove);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_actionSend);
    m_toolBar->addAction(m_actionEditSignals);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_actionExpandAll);
    m_toolBar->addAction(m_actionCollapseAll);
    m_toolBar->addSeparator();

    // Instant Filter Box
    m_filterEdit = new QLineEdit(m_toolBar);
    m_filterEdit->setPlaceholderText(tr("Filter (ID, Symbol, Signal)..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setMaximumWidth(220);
    m_filterEdit->setToolTip(tr("Search across CAN IDs, message names and signal names"));

    m_toolBar->addWidget(m_filterEdit);

    auto* spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_toolBar->addWidget(spacer);

    // Tree View
    m_treeModel = new TransmitTreeModel(m_list, this);
    connect(m_treeModel, &TransmitTreeModel::reported, this, &TransmitPanel::reported);

    m_proxyModel = new TransmitTreeFilterModel(this);
    m_proxyModel->setSourceModel(m_treeModel);
    connect(m_filterEdit,
            &QLineEdit::textChanged,
            m_proxyModel,
            &TransmitTreeFilterModel::setFilterText);

    m_treeView = new QTreeView(this);
    m_treeView->setObjectName(QStringLiteral("torquebusTransmitTree"));
    m_treeView->setFont(ThemeManager::monospaceFont(9.5));
    m_treeView->setModel(m_proxyModel);
    m_treeView->setItemDelegate(new TransmitTreeDelegate(this));
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->setAnimated(true);
    m_treeView->setIndentation(16);
    m_treeView->setExpandsOnDoubleClick(true);
    m_treeView->header()->setStretchLastSection(true);
    m_treeView->header()->setHighlightSections(false);

    for (int col = 0; col < TransmitColumnCount; ++col) {
        m_treeView->setColumnWidth(col, kTransmitColumnWidths[col]);
    }

    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActionState();
    });

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_treeView, 1);
    layout->addWidget(m_status);
}

void TransmitPanel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_databases = std::move(databases);
    m_actionAddFromMessage->setEnabled(!m_databases.empty());
    m_treeModel->setDatabases(m_databases);
    updateActionState();
}

void TransmitPanel::reload()
{
    m_treeModel->reload();
    m_status->setText(tr("%n row(s)", nullptr, static_cast<int>(m_list.size())));
    updateActionState();
}

void TransmitPanel::refreshCounters()
{
    m_treeModel->refreshCounters();
}

void TransmitPanel::onAdd()
{
    TransmitEntry entry;
    entry.name = tr("New frame").toStdString();
    entry.frame.identifier = 0x100;
    entry.frame.length = 8;
    entry.frame.dlc = 8;
    entry.trigger = TransmitTrigger::Manual;

    (void)m_list.add(std::move(entry));
    reload();

    const int newRow = static_cast<int>(m_list.size()) - 1;
    const QModelIndex proxyIdx = m_proxyModel->mapFromSource(m_treeModel->index(newRow, 0));
    if (proxyIdx.isValid()) {
        m_treeView->setCurrentIndex(proxyIdx);
    }
}

void TransmitPanel::onAddFromMessage()
{
    QStringList choices;
    std::vector<const CanMessage*> messages;

    for (const auto& database : m_databases) {
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

    if (message->cycleTimeMs > 0) {
        entry.cycleMs = message->cycleTimeMs;
    }

    (void)m_list.add(std::move(entry));
    reload();

    const int newRow = static_cast<int>(m_list.size()) - 1;
    const QModelIndex srcIdx = m_treeModel->index(newRow, 0);
    const QModelIndex proxyIdx = m_proxyModel->mapFromSource(srcIdx);
    if (proxyIdx.isValid()) {
        m_treeView->setCurrentIndex(proxyIdx);
        m_treeView->expand(proxyIdx);
    }
}

void TransmitPanel::onRemove()
{
    const int row = selectedMessageIndex();
    if (row < 0) {
        return;
    }

    m_list.remove(static_cast<std::size_t>(row));
    reload();

    if (m_list.size() > 0) {
        const int nextRow = std::min(row, static_cast<int>(m_list.size()) - 1);
        const QModelIndex proxyIdx = m_proxyModel->mapFromSource(m_treeModel->index(nextRow, 0));
        if (proxyIdx.isValid()) {
            m_treeView->setCurrentIndex(proxyIdx);
        }
    }
}

void TransmitPanel::onSendSelected()
{
    const int row = selectedMessageIndex();
    if (row < 0) {
        Q_EMIT reported(tr("Select a row to send."));
        return;
    }

    if (!m_list.sendOnce(static_cast<std::size_t>(row))) {
        Q_EMIT reported(tr("Row %1 was not sent: it is switched off, or the "
                           "send queue is full.")
                            .arg(row + 1));
    }
}

void TransmitPanel::onEditSignals()
{
    const int row = selectedMessageIndex();
    if (row < 0) {
        return;
    }

    TransmitEntry entry;
    if (!m_list.entryAt(static_cast<std::size_t>(row), entry)) {
        return;
    }

    const CanMessage* message = m_treeModel->messageFor(entry);
    if (message == nullptr) {
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
    const QModelIndex proxyIdx = m_proxyModel->mapFromSource(m_treeModel->index(row, 0));
    if (proxyIdx.isValid()) {
        m_treeView->setCurrentIndex(proxyIdx);
    }
}

void TransmitPanel::onExpandAll()
{
    if (m_treeView != nullptr) {
        m_treeView->expandAll();
    }
}

void TransmitPanel::onCollapseAll()
{
    if (m_treeView != nullptr) {
        m_treeView->collapseAll();
    }
}

void TransmitPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) {
        onSendSelected();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        onRemove();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TransmitPanel::updateActionState()
{
    const int row = selectedMessageIndex();

    TransmitEntry entry;
    const bool exists = row >= 0 && m_list.entryAt(static_cast<std::size_t>(row), entry);
    const bool known = exists && m_treeModel->messageFor(entry) != nullptr;

    m_actionEditSignals->setEnabled(known);
    m_actionRemove->setEnabled(exists);
    m_actionSend->setEnabled(exists);
}

int TransmitPanel::selectedMessageIndex() const
{
    if (m_treeView == nullptr || m_treeView->selectionModel() == nullptr) {
        return -1;
    }

    const QModelIndexList selected = m_treeView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return -1;
    }

    const QModelIndex srcIdx = m_proxyModel->mapToSource(selected.front());
    if (!srcIdx.isValid()) {
        return -1;
    }

    const int nodeType = srcIdx.data(TransmitTreeModel::kNodeTypeRole).toInt();
    if (nodeType == 1) {
        return srcIdx.data(TransmitTreeModel::kEntryIndexRole).toInt();
    }
    if (nodeType == 2 && srcIdx.parent().isValid()) {
        return srcIdx.parent().data(TransmitTreeModel::kEntryIndexRole).toInt();
    }

    return -1;
}

void TransmitPanel::onThemeChanged()
{
    if (ThemeManager* themes = ThemeManager::instance()) {
        m_actionAdd->setIcon(themes->icon(QStringLiteral("new")));
        m_actionAddFromMessage->setIcon(themes->icon(QStringLiteral("database")));
        m_actionRemove->setIcon(themes->icon(QStringLiteral("clear")));
        m_actionSend->setIcon(themes->icon(QStringLiteral("transmit")));
        m_actionEditSignals->setIcon(themes->icon(QStringLiteral("properties")));
        m_actionExpandAll->setIcon(themes->icon(QStringLiteral("panel-expand")));
        m_actionCollapseAll->setIcon(themes->icon(QStringLiteral("panel-collapse")));
    }

    if (m_treeView != nullptr) {
        m_treeView->viewport()->update();
    }
}

} // namespace torquebus::ui
