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
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QToolTip>
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

    m_actionAdd = new QAction(icon("add"), tr("Add"), this);
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
    m_actionSend->setShortcut(QKeySequence(Qt::Key_Space));
    m_actionSend->setShortcutContext(Qt::WidgetShortcut);
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
    connect(m_treeModel, &TransmitTreeModel::modified, this, &TransmitPanel::modified);

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
    m_treeView->setMouseTracking(true);
    m_treeView->viewport()->setMouseTracking(true);
    m_treeView->viewport()->installEventFilter(this);
    m_treeView->installEventFilter(this);
    m_treeView->addAction(m_actionSend);

    for (int col = 0; col < TransmitColumnCount; ++col) {
        m_treeView->setColumnWidth(col, kTransmitColumnWidths[col]);
    }

    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActionState();
    });
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        updateActionState();
    });

    connect(m_treeView->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        hideSymbolCard();
    });
    connect(m_treeView->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        hideSymbolCard();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this] {
        hideSymbolCard();
    });

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_treeView, 1);
    layout->addWidget(m_status);

    buildSymbolCard();
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
    hideSymbolCard();
    m_treeModel->reload();
    m_status->setText(tr("%n row(s)", nullptr, static_cast<int>(m_list.size())));
    updateActionState();
}

void TransmitPanel::selectEntry(int index)
{
    if (m_treeView == nullptr || m_treeModel == nullptr || m_proxyModel == nullptr) {
        return;
    }

    const QModelIndex srcIdx = m_treeModel->index(index, 0);
    if (!srcIdx.isValid()) {
        return;
    }

    const QModelIndex proxyIdx = m_proxyModel->mapFromSource(srcIdx);
    if (proxyIdx.isValid()) {
        m_treeView->setCurrentIndex(proxyIdx);
        m_treeView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    }
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
    Q_EMIT modified();

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
    Q_EMIT modified();

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
    hideSymbolCard();
    const int row = selectedMessageIndex();
    if (row < 0) {
        return;
    }

    m_list.remove(static_cast<std::size_t>(row));
    reload();
    Q_EMIT modified();

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

    if (!m_measurementRunning) {
        // Said, not queued: a frame queued while stopped is dropped when the
        // measurement starts, so accepting it silently looked like a dead button.
        Q_EMIT reported(tr("Nothing was sent: no measurement is running. Press Start, "
                           "then Send."));
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
    Q_EMIT modified();
    const QModelIndex proxyIdx = m_proxyModel->mapFromSource(m_treeModel->index(row, 0));
    if (proxyIdx.isValid()) {
        m_treeView->setCurrentIndex(proxyIdx);
    }
}

void TransmitPanel::onExpandAll()
{
    hideSymbolCard();
    if (m_treeView != nullptr) {
        m_treeView->expandAll();
    }
}

void TransmitPanel::onCollapseAll()
{
    hideSymbolCard();
    if (m_treeView != nullptr) {
        m_treeView->collapseAll();
    }
}

void TransmitPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) {
        const QWidget* focus = QApplication::focusWidget();
        const bool isEditorActive = (m_treeView != nullptr && focus != nullptr
                                     && focus != m_treeView && focus != m_treeView->viewport()
                                     && m_treeView->isAncestorOf(focus));
        if (!isEditorActive) {
            onSendSelected();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Delete) {
        const QWidget* focus = QApplication::focusWidget();
        const bool isEditorActive = (m_treeView != nullptr && focus != nullptr
                                     && focus != m_treeView && focus != m_treeView->viewport()
                                     && m_treeView->isAncestorOf(focus));
        if (!isEditorActive) {
            onRemove();
            event->accept();
            return;
        }
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

    QModelIndex proxyIdx;
    const QModelIndexList selected = m_treeView->selectionModel()->selectedRows();
    if (!selected.isEmpty()) {
        proxyIdx = selected.front();
    } else if (!m_treeView->selectionModel()->selectedIndexes().isEmpty()) {
        proxyIdx = m_treeView->selectionModel()->selectedIndexes().front();
    } else {
        proxyIdx = m_treeView->currentIndex();
    }

    if (!proxyIdx.isValid()) {
        return -1;
    }

    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    if (!srcIdx.isValid()) {
        return -1;
    }

    const int nodeType = srcIdx.data(TransmitTreeModel::kNodeTypeRole).toInt();
    if (nodeType == 1) {
        return srcIdx.data(TransmitTreeModel::kEntryIndexRole).toInt();
    }
    if (nodeType == 2) {
        if (srcIdx.parent().isValid()) {
            return srcIdx.parent().data(TransmitTreeModel::kEntryIndexRole).toInt();
        }
        return srcIdx.data(TransmitTreeModel::kEntryIndexRole).toInt();
    }

    return -1;
}

void TransmitPanel::onThemeChanged()
{
    if (ThemeManager* themes = ThemeManager::instance()) {
        m_actionAdd->setIcon(themes->icon(QStringLiteral("add")));
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

void TransmitPanel::buildSymbolCard()
{
    m_symbolCard = new QFrame(this,
                               Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput
                                   | Qt::WindowDoesNotAcceptFocus);
    m_symbolCard->setObjectName(QStringLiteral("transmitSymbolCard"));
    m_symbolCard->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_symbolCard->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_symbolCard->setMinimumWidth(300);
    m_symbolCard->setMaximumWidth(440);

    auto* cardLayout = new QVBoxLayout(m_symbolCard);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(4);

    m_cardTitle = new QLabel(m_symbolCard);
    m_cardTitle->setObjectName(QStringLiteral("transmitSymbolCardTitle"));
    m_cardTitle->setWordWrap(true);
    m_cardTitle->setTextFormat(Qt::PlainText);

    m_cardMeta = new QLabel(m_symbolCard);
    m_cardMeta->setObjectName(QStringLiteral("transmitSymbolCardMeta"));
    m_cardMeta->setWordWrap(true);
    m_cardMeta->setTextFormat(Qt::PlainText);

    m_cardBody = new QLabel(m_symbolCard);
    m_cardBody->setObjectName(QStringLiteral("transmitSymbolCardBody"));
    m_cardBody->setWordWrap(true);
    m_cardBody->setTextFormat(Qt::PlainText);

    m_cardValues = new QLabel(m_symbolCard);
    m_cardValues->setObjectName(QStringLiteral("transmitSymbolCardValues"));
    m_cardValues->setWordWrap(true);
    m_cardValues->setTextFormat(Qt::PlainText);

    cardLayout->addWidget(m_cardTitle);
    cardLayout->addWidget(m_cardMeta);
    cardLayout->addWidget(m_cardBody);
    cardLayout->addWidget(m_cardValues);

    m_symbolCard->hide();
}

void TransmitPanel::updateSymbolCardAt(const QPoint& viewportPos)
{
    if (m_treeView == nullptr || m_symbolCard == nullptr) {
        return;
    }

    const QModelIndex proxyIndex = m_treeView->indexAt(viewportPos);
    if (!proxyIndex.isValid() || proxyIndex.column() != ColumnSymbol) {
        hideSymbolCard();
        return;
    }

    const QString title = proxyIndex.data(TransmitTreeModel::kCardTitleRole).toString();
    const QString meta = proxyIndex.data(TransmitTreeModel::kCardMetaRole).toString();
    const QString body = proxyIndex.data(TransmitTreeModel::kCardBodyRole).toString();
    const QString values = proxyIndex.data(TransmitTreeModel::kCardValuesRole).toString();

    if (title.isEmpty() && meta.isEmpty() && body.isEmpty() && values.isEmpty()) {
        hideSymbolCard();
        return;
    }

    if (m_hoveredSymbolIndex != proxyIndex) {
        m_hoveredSymbolIndex = proxyIndex;

        m_cardTitle->setText(title);
        m_cardTitle->setVisible(!title.isEmpty());

        m_cardMeta->setText(meta);
        m_cardMeta->setVisible(!meta.isEmpty());

        if (!body.isEmpty()) {
            m_cardBody->setText(body);
            m_cardBody->setVisible(true);
            m_cardValues->setText(values);
            m_cardValues->setVisible(!values.isEmpty());
        } else if (!values.isEmpty()) {
            m_cardBody->setText(values);
            m_cardBody->setVisible(true);
            m_cardValues->clear();
            m_cardValues->setVisible(false);
        } else {
            m_cardBody->clear();
            m_cardBody->setVisible(false);
            m_cardValues->clear();
            m_cardValues->setVisible(false);
        }

        if (m_symbolCard->layout() != nullptr) {
            m_symbolCard->layout()->activate();
        }
        m_symbolCard->adjustSize();
    }

    const QRect cellRect = m_treeView->visualRect(proxyIndex);
    const QPoint globalCursor = m_treeView->viewport()->mapToGlobal(viewportPos);
    const QPoint cellBottomLeft = m_treeView->viewport()->mapToGlobal(cellRect.bottomLeft());
    const QPoint cellTopLeft = m_treeView->viewport()->mapToGlobal(cellRect.topLeft());

    const QSize cardSize = m_symbolCard->sizeHint().expandedTo(m_symbolCard->minimumSize());
    int x = std::max(cellBottomLeft.x(), globalCursor.x() - 24);
    int y = cellBottomLeft.y() + 6;

    if (const QScreen* screen = QGuiApplication::screenAt(globalCursor)) {
        const QRect avail = screen->availableGeometry();
        if (x + cardSize.width() > avail.right() - 8) {
            x = std::max(avail.left() + 8, avail.right() - cardSize.width() - 8);
        }
        if (y + cardSize.height() > avail.bottom() - 8) {
            y = std::max(avail.top() + 8, cellTopLeft.y() - cardSize.height() - 6);
        }
    }

    m_symbolCard->move(x, y);
    if (!m_symbolCard->isVisible()) {
        m_symbolCard->show();
    }
}

void TransmitPanel::hideSymbolCard()
{
    m_hoveredSymbolIndex = {};
    if (m_symbolCard != nullptr && m_symbolCard->isVisible()) {
        m_symbolCard->hide();
    }
}

bool TransmitPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (m_treeView != nullptr && event != nullptr) {
        if (watched == m_treeView || watched == m_treeView->viewport()) {
            if (event->type() == QEvent::KeyPress) {
                auto* keyEvent = static_cast<QKeyEvent*>(event);
                if (keyEvent->key() == Qt::Key_Space) {
                    const QWidget* focus = QApplication::focusWidget();
                    const bool isEditorActive = (focus != nullptr && focus != m_treeView
                                                 && focus != m_treeView->viewport()
                                                 && m_treeView->isAncestorOf(focus));
                    if (!isEditorActive) {
                        onSendSelected();
                        return true;
                    }
                } else if (keyEvent->key() == Qt::Key_Delete) {
                    const QWidget* focus = QApplication::focusWidget();
                    const bool isEditorActive = (focus != nullptr && focus != m_treeView
                                                 && focus != m_treeView->viewport()
                                                 && m_treeView->isAncestorOf(focus));
                    if (!isEditorActive) {
                        onRemove();
                        return true;
                    }
                }
            }
        }

        if (watched == m_treeView->viewport()) {
            switch (event->type()) {
            case QEvent::MouseMove: {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                updateSymbolCardAt(mouseEvent->pos());
                break;
            }
            case QEvent::ToolTip: {
                const auto* helpEvent = static_cast<QHelpEvent*>(event);
                const QModelIndex idx = m_treeView->indexAt(helpEvent->pos());
                if (idx.isValid() && idx.column() == ColumnSymbol) {
                    updateSymbolCardAt(helpEvent->pos());
                    QToolTip::hideText();
                    return true;
                }
                hideSymbolCard();
                break;
            }
            case QEvent::Leave:
            case QEvent::Wheel:
            case QEvent::Hide:
                hideSymbolCard();
                break;
            default:
                break;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TransmitPanel::hideEvent(QHideEvent* event)
{
    hideSymbolCard();
    QWidget::hideEvent(event);
}

} // namespace torquebus::ui
