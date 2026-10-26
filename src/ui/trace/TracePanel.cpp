// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TracePanel.h"

#include "ui/theme/ThemeManager.h"
#include "ui/trace/TraceModel.h"
#include "ui/trace/TraceTreeDelegate.h"
#include "ui/trace/TraceTreeFilterModel.h"
#include "ui/trace/TraceTreeModel.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

constexpr int kColumnWidths[TraceModel::ColumnCount] = {
    110, // Time
    85, // Delta
    70, // Channel
    75, // Direction
    110, // Identifier
    165, // Name
    75, // Type
    55, // DLC
    270, // Data
    320, // Signals
    95, // Cycle
    80, // Count
    65, // Flags
};

constexpr int kTreeColumnWidths[TreeColumnCount] = {
    130, // Bus ("Receive" / "Transmit" + branch expander + "CAN 1" + margins)
    85, // Type ("STD", "EXT", "FD", "FD EXT", "J1939" pill badges)
    125, // CAN-ID (Hex) ("CAN-ID (Hex)" header + sort arrow + 8-digit hex)
    100, // PGN / Dec ("PGN / Dec" header + 6-digit PGNs)
    70, // Length ("Length" header)
    240, // Symbol ("Symbol" header + 📦 message name / 🏷️ signal name)
    270, // Data ("Data" header + 8-byte hex with change pills / signal values)
    105, // Cycle Time ("Cycle Time" header + "100.0 ms")
    85, // Count ("Count" header + numbers)
    250 // Description ("Description" header + DBC comments, stretches to right edge)
};

constexpr int kBottomTolerance = 4;

} // namespace

TracePanel::TracePanel(QWidget* parent)
    : QWidget{parent}
{
    createViews();
    createToolBar();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_splitter, 1);

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, &TracePanel::onThemeChanged);
    }

    auto* statusTimer = new QTimer(this);
    statusTimer->setInterval(250);
    statusTimer->setTimerType(Qt::CoarseTimer);
    connect(statusTimer, &QTimer::timeout, this, &TracePanel::refreshStatus);
    statusTimer->start();

    setViewMode(ViewMode::Grouped);
}

void TracePanel::createViews()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("torquebusTraceSplitter"));
    m_splitter->setChildrenCollapsible(false);

    // 1. Hierarchical Tree View (PCAN-Explorer 7 / CANoe grouped mode)
    m_treeModel = new TraceTreeModel(this);
    m_proxyModel = new TraceTreeFilterModel(this);
    m_proxyModel->setSourceModel(m_treeModel);

    m_treeView = new QTreeView(this);
    m_treeView->setObjectName(QStringLiteral("torquebusTraceTree"));
    m_treeView->setFont(ThemeManager::monospaceFont(9.5));
    m_treeView->setModel(m_proxyModel);
    m_treeView->setItemDelegate(new TraceTreeDelegate(this));
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->setAnimated(true);
    m_treeView->setIndentation(16);
    m_treeView->setExpandsOnDoubleClick(true);
    m_treeView->header()->setStretchLastSection(true);
    m_treeView->header()->setHighlightSections(false);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        m_treeView, &QTreeView::customContextMenuRequested, this, &TracePanel::onTreeContextMenu);

    connect(
        m_treeModel, &TraceTreeModel::groupPopulated, this, [this](const QModelIndex& groupIdx) {
            const QModelIndex proxyIdx = m_proxyModel->mapFromSource(groupIdx);
            if (proxyIdx.isValid() && !m_treeView->isExpanded(proxyIdx)) {
                m_treeView->expand(proxyIdx);
            }
        });

    m_splitter->addWidget(m_treeView);

    // 2. Chronological Streaming Table View
    m_model = new TraceModel(this);

    m_view = new QTableView(this);
    m_view->setObjectName(QStringLiteral("torquebusTraceTable"));
    m_view->setFont(ThemeManager::monospaceFont(9.5));
    m_view->setModel(m_model);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setDefaultSectionSize(20);
    m_view->setShowGrid(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setWordWrap(false);
    m_view->setTextElideMode(Qt::ElideRight);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setHighlightSections(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QTableView::customContextMenuRequested, this, &TracePanel::onTableContextMenu);

    connect(m_model, &TraceModel::rowsAppended, this, &TracePanel::onRowsAppended);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &TracePanel::onScrolled);

    m_splitter->addWidget(m_view);

    applyColumnWidths();
}

void TracePanel::createToolBar()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.trace"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    ThemeManager* themes = ThemeManager::instance();
    const auto icon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    // Mode Selector
    m_modeCombo = new QComboBox(m_toolBar);
    m_modeCombo->addItem(tr("Grouped (Tree)"));
    m_modeCombo->addItem(tr("Chronological"));
    m_modeCombo->addItem(tr("Split View"));
    m_modeCombo->setToolTip(tr("Switch between Grouped Tree (PCAN/CANoe style), "
                               "Chronological Stream, or Split View"));
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, &TracePanel::onViewModeChanged);
    m_toolBar->addWidget(m_modeCombo);

    m_toolBar->addSeparator();

    m_actionFreeze = new QAction(icon("pause"), tr("Freeze"), this);
    m_actionFreeze->setCheckable(true);
    m_actionFreeze->setToolTip(tr("Stop adding new frames to this view. The measurement, "
                                  "the logger and the counters keep running."));
    connect(m_actionFreeze, &QAction::toggled, this, &TracePanel::onFreezeToggled);

    m_actionClear = new QAction(icon("clear"), tr("Clear"), this);
    m_actionClear->setToolTip(tr("Discard the frames currently held by this view"));
    connect(m_actionClear, &QAction::triggered, this, &TracePanel::onClear);

    m_toolBar->addAction(m_actionFreeze);
    m_toolBar->addAction(m_actionClear);
    m_toolBar->addSeparator();

    m_actionExpandAll = new QAction(icon("panel-expand"), tr("Expand All"), this);
    m_actionExpandAll->setToolTip(tr("Expand all messages to show decoded signals"));
    connect(m_actionExpandAll, &QAction::triggered, this, &TracePanel::onExpandAll);

    m_actionCollapseAll = new QAction(icon("panel-collapse"), tr("Collapse All"), this);
    m_actionCollapseAll->setToolTip(tr("Collapse signals"));
    connect(m_actionCollapseAll, &QAction::triggered, this, &TracePanel::onCollapseAll);

    m_toolBar->addAction(m_actionExpandAll);
    m_toolBar->addAction(m_actionCollapseAll);
    m_toolBar->addSeparator();

    // Instant Filter Box
    m_filterEdit = new QLineEdit(m_toolBar);
    m_filterEdit->setPlaceholderText(tr("Filter (ID, Symbol, Signal)..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setMaximumWidth(220);
    m_filterEdit->setToolTip(
        tr("Search across CAN IDs, message names, signal names and descriptions"));
    connect(
        m_filterEdit, &QLineEdit::textChanged, m_proxyModel, &TraceTreeFilterModel::setFilterText);
    m_toolBar->addWidget(m_filterEdit);

    m_actionFollow = new QAction(icon("trace"), tr("Follow"), this);
    m_actionFollow->setCheckable(true);
    m_actionFollow->setChecked(true);
    m_actionFollow->setToolTip(tr("Scroll to the newest frame as it arrives. Turns itself "
                                  "off when you scroll up to read."));
    connect(m_actionFollow, &QAction::toggled, this, [this](bool follow) {
        m_following = follow;
        if (follow && m_view != nullptr) {
            m_scrollingProgrammatically = true;
            m_view->scrollToBottom();
            m_scrollingProgrammatically = false;
        }
    });

    m_toolBar->addAction(m_actionFollow);

    auto* spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_toolBar->addWidget(spacer);

    m_statusLabel = new QLabel(m_toolBar);
    m_statusLabel->setProperty("torquebusState", "ready");
    m_toolBar->addWidget(m_statusLabel);
}

void TracePanel::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    switch (mode) {
    case ViewMode::Grouped:
        m_treeView->show();
        m_view->hide();
        m_actionExpandAll->setVisible(true);
        m_actionCollapseAll->setVisible(true);
        m_filterEdit->setVisible(true);
        m_actionFollow->setVisible(false);
        break;

    case ViewMode::Chronological:
        m_treeView->hide();
        m_view->show();
        m_actionExpandAll->setVisible(false);
        m_actionCollapseAll->setVisible(false);
        m_filterEdit->setVisible(false);
        m_actionFollow->setVisible(true);
        break;

    case ViewMode::Split:
        m_treeView->show();
        m_view->show();
        m_actionExpandAll->setVisible(true);
        m_actionCollapseAll->setVisible(true);
        m_filterEdit->setVisible(true);
        m_actionFollow->setVisible(true);
        const int half = std::max(width() / 2, 200);
        m_splitter->setSizes({half, half});
        break;
    }
}

void TracePanel::onViewModeChanged(int index)
{
    setViewMode(static_cast<ViewMode>(index));
}

void TracePanel::onExpandAll()
{
    if (m_treeView != nullptr) {
        m_treeView->expandAll();
    }
}

void TracePanel::onCollapseAll()
{
    if (m_treeView != nullptr && m_treeModel != nullptr && m_proxyModel != nullptr) {
        m_treeView->collapseAll();
        // Keep the top-level Receive and Transmit groups expanded
        const QModelIndex rxIdx = m_proxyModel->mapFromSource(m_treeModel->rxGroupIndex());
        const QModelIndex txIdx = m_proxyModel->mapFromSource(m_treeModel->txGroupIndex());
        if (rxIdx.isValid()) {
            m_treeView->expand(rxIdx);
        }
        if (txIdx.isValid()) {
            m_treeView->expand(txIdx);
        }
    }
}

void TracePanel::applyColumnWidths()
{
    for (int column = 0; column < TraceModel::ColumnCount; ++column) {
        m_view->setColumnWidth(column, kColumnWidths[column]);
    }
    for (int column = 0; column < TreeColumnCount; ++column) {
        m_treeView->setColumnWidth(column, kTreeColumnWidths[column]);
    }
}

void TracePanel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_model->setDatabases(databases);
    m_treeModel->setDatabases(std::move(databases));
}

void TracePanel::applyPreferences(int refreshMs, bool decimalIdentifiers)
{
    m_model->setRefreshIntervalMs(refreshMs);
    m_model->setDecimalIdentifiers(decimalIdentifiers);

    m_treeModel->setRefreshIntervalMs(refreshMs);
    m_treeModel->setDecimalIdentifiers(decimalIdentifiers);
}

void TracePanel::poll()
{
    if (m_model != nullptr) {
        QMetaObject::invokeMethod(m_model, "pollStore");
    }
    if (m_treeModel != nullptr) {
        m_treeModel->pollStore();
    }
}

void TracePanel::setStore(const TraceStore* store)
{
    m_store = store;
    m_model->setStore(store);
    m_treeModel->setStore(store);
    poll();
    refreshStatus();
}

void TracePanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    poll();
    refreshStatus();
    if (m_following && m_view != nullptr && m_model != nullptr && m_model->rowCount() > 0) {
        m_scrollingProgrammatically = true;
        m_view->scrollToBottom();
        m_scrollingProgrammatically = false;
    }
}

void TracePanel::onRowsAppended(int firstRow, int lastRow)
{
    (void)firstRow;

    if (!m_following || m_view == nullptr || !isVisible()) {
        return;
    }

    m_scrollingProgrammatically = true;
    m_view->scrollTo(m_model->index(lastRow, 0), QAbstractItemView::PositionAtBottom);
    m_scrollingProgrammatically = false;
}

void TracePanel::onScrolled()
{
    if (m_scrollingProgrammatically || m_view == nullptr) {
        return;
    }

    const QScrollBar* bar = m_view->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - kBottomTolerance;

    if (atBottom != m_following) {
        m_following = atBottom;

        QSignalBlocker blocker{m_actionFollow};
        m_actionFollow->setChecked(atBottom);
    }
}

void TracePanel::onFreezeToggled(bool frozen)
{
    m_model->setFrozen(frozen);
    m_treeModel->setFrozen(frozen);

    m_actionFreeze->setText(frozen ? tr("Resume") : tr("Freeze"));

    if (m_statusLabel != nullptr) {
        m_statusLabel->setProperty("torquebusState", frozen ? "warning" : "ready");
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
    }
}

void TracePanel::onClear()
{
    m_model->reset();
    m_treeModel->reset();
    refreshStatus();
}

void TracePanel::onThemeChanged(const Theme& theme)
{
    (void)theme;

    if (ThemeManager* themes = ThemeManager::instance()) {
        m_actionFreeze->setIcon(themes->icon(QStringLiteral("pause")));
        m_actionClear->setIcon(themes->icon(QStringLiteral("clear")));
        m_actionFollow->setIcon(themes->icon(QStringLiteral("trace")));
        m_actionExpandAll->setIcon(themes->icon(QStringLiteral("panel-expand")));
        m_actionCollapseAll->setIcon(themes->icon(QStringLiteral("panel-collapse")));
    }

    if (m_view != nullptr) {
        m_view->viewport()->update();
    }
    if (m_treeView != nullptr) {
        m_treeView->viewport()->update();
    }
}

void TracePanel::refreshStatus()
{
    if (m_statusLabel == nullptr || !isVisible()) {
        return;
    }

    if (m_store == nullptr) {
        m_statusLabel->setText(tr("No measurement"));
        return;
    }

    const std::uint64_t total = m_store->totalAppended();
    const std::uint64_t dropped = m_store->discarded();

    if (dropped > 0) {
        m_statusLabel->setText(tr("%L1 frames  ·  showing last %L2  ·  %L3 dropped  ·  %L4 IDs")
                                   .arg(total)
                                   .arg(m_store->size())
                                   .arg(dropped)
                                   .arg(m_store->identifiers().size()));
    } else {
        m_statusLabel->setText(
            tr("%L1 frames  ·  %L2 unique IDs").arg(total).arg(m_store->identifiers().size()));
    }
}

void TracePanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) {
        if (m_filterEdit == nullptr || !m_filterEdit->hasFocus()) {
            if (m_actionFreeze != nullptr) {
                m_actionFreeze->toggle();
                event->accept();
                return;
            }
        }
    }
    QWidget::keyPressEvent(event);
}

void TracePanel::onTreeContextMenu(const QPoint& pos)
{
    if (m_treeView == nullptr || m_proxyModel == nullptr || m_treeModel == nullptr) {
        return;
    }

    const QModelIndex proxyIdx = m_treeView->indexAt(pos);
    if (!proxyIdx.isValid()) {
        return;
    }

    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    const TraceTreeNode* node = m_treeModel->nodeFromIndex(srcIdx);
    if (node == nullptr) {
        return;
    }

    QMenu menu(this);
    ThemeManager* themes = ThemeManager::instance();
    const auto icon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    if (node->type == TreeNodeType::Message) {
        const QString name = node->dbMessage != nullptr
                                 ? QString::fromStdString(node->dbMessage->name)
                                 : tr("0x%1")
                                       .arg(node->identifier,
                                            (node->format == CanFrameFormat::Extended ? 8 : 3),
                                            16,
                                            QLatin1Char('0'))
                                       .toUpper();
        const QString hexId = QStringLiteral("%1")
                                  .arg(node->identifier,
                                       (node->format == CanFrameFormat::Extended ? 8 : 3),
                                       16,
                                       QLatin1Char('0'))
                                  .toUpper();

        QAction* title = menu.addAction(tr("Message: %1").arg(name));
        title->setEnabled(false);
        menu.addSeparator();

        QAction* filterAction = menu.addAction(icon("search"), tr("Filter by ID (%1)").arg(hexId));
        connect(filterAction, &QAction::triggered, this, [this, hexId] {
            if (m_filterEdit != nullptr) {
                m_filterEdit->setText(hexId);
            }
        });

        QAction* copyIdAction = menu.addAction(tr("Copy Identifier (%1)").arg(hexId));
        connect(copyIdAction, &QAction::triggered, this, [hexId] {
            if (QClipboard* clip = QGuiApplication::clipboard()) {
                clip->setText(hexId);
            }
        });

        const auto optFrame = m_treeModel->frameForNode(node);
        if (optFrame.has_value()) {
            const CanFrame& frame = *optFrame;
            QStringList hexBytes;
            for (std::size_t i = 0; i < frame.payloadSize(); ++i) {
                hexBytes
                    << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
            }
            const QString dataStr = hexBytes.join(QLatin1Char(' '));

            QAction* copyDataAction = menu.addAction(tr("Copy Data Bytes"));
            connect(copyDataAction, &QAction::triggered, this, [dataStr] {
                if (QClipboard* clip = QGuiApplication::clipboard()) {
                    clip->setText(dataStr);
                }
            });

            menu.addSeparator();

            QAction* transmitAction = menu.addAction(icon("transmit"), tr("Send to Transmit List"));
            connect(transmitAction, &QAction::triggered, this, [this, frame, name] {
                Q_EMIT sendToTransmitRequested(frame, name);
            });
        }
    } else if (node->type == TreeNodeType::Signal && node->dbSignal != nullptr) {
        const QString sigName = QString::fromStdString(node->dbSignal->name);

        QAction* title = menu.addAction(tr("Signal: %1").arg(sigName));
        title->setEnabled(false);
        menu.addSeparator();

        QAction* plotAction = menu.addAction(icon("graph"), tr("Plot in Graph"));
        connect(plotAction, &QAction::triggered, this, [this, sigName] {
            Q_EMIT plotSignalRequested(sigName);
        });

        QAction* filterAction = menu.addAction(icon("search"), tr("Filter by Signal Name"));
        connect(filterAction, &QAction::triggered, this, [this, sigName] {
            if (m_filterEdit != nullptr) {
                m_filterEdit->setText(sigName);
            }
        });

        QAction* copySigNameAction = menu.addAction(tr("Copy Signal Name"));
        connect(copySigNameAction, &QAction::triggered, this, [sigName] {
            if (QClipboard* clip = QGuiApplication::clipboard()) {
                clip->setText(sigName);
            }
        });

        const QModelIndex valIdx =
            m_proxyModel->index(proxyIdx.row(), ColumnData, proxyIdx.parent());
        const QString valText = m_proxyModel->data(valIdx, Qt::DisplayRole).toString();
        if (!valText.isEmpty()) {
            QAction* copyValAction = menu.addAction(tr("Copy Value (%1)").arg(valText));
            connect(copyValAction, &QAction::triggered, this, [valText] {
                if (QClipboard* clip = QGuiApplication::clipboard()) {
                    clip->setText(valText);
                }
            });
        }

        if (node->parent != nullptr) {
            const auto optFrame = m_treeModel->frameForNode(node->parent);
            if (optFrame.has_value()) {
                const CanFrame& parentFrame = *optFrame;
                const QString parentName =
                    node->parent->dbMessage != nullptr
                        ? QString::fromStdString(node->parent->dbMessage->name)
                        : QStringLiteral("0x%1").arg(parentFrame.identifier, 0, 16).toUpper();

                menu.addSeparator();
                QAction* transmitAction =
                    menu.addAction(icon("transmit"), tr("Send Parent Message to Transmit List"));
                connect(transmitAction, &QAction::triggered, this, [this, parentFrame, parentName] {
                    Q_EMIT sendToTransmitRequested(parentFrame, parentName);
                });
            }
        }
    }

    if (!menu.actions().isEmpty()) {
        menu.exec(m_treeView->viewport()->mapToGlobal(pos));
    }
}

void TracePanel::onTableContextMenu(const QPoint& pos)
{
    if (m_view == nullptr || m_model == nullptr) {
        return;
    }

    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    const auto optRow = m_model->rowAt(index.row());
    if (!optRow.has_value()) {
        return;
    }

    const TraceRow& trRow = *optRow;
    const CanFrame& frame = trRow.frame;

    const QString hexId = QStringLiteral("%1")
                              .arg(frame.identifier,
                                   (frame.format == CanFrameFormat::Extended ? 8 : 3),
                                   16,
                                   QLatin1Char('0'))
                              .toUpper();
    const QString name =
        m_model->data(m_model->index(index.row(), TraceModel::Name), Qt::DisplayRole).toString();
    const QString displayTitle =
        name.isEmpty() ? tr("Frame: 0x%1").arg(hexId) : tr("%1 (0x%2)").arg(name, hexId);

    QMenu menu(this);
    ThemeManager* themes = ThemeManager::instance();
    const auto icon = [themes](const char* name) {
        return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
    };

    QAction* title = menu.addAction(displayTitle);
    title->setEnabled(false);
    menu.addSeparator();

    QAction* filterAction = menu.addAction(icon("search"), tr("Filter by ID (%1)").arg(hexId));
    connect(filterAction, &QAction::triggered, this, [this, hexId] {
        if (m_filterEdit != nullptr) {
            m_filterEdit->setText(hexId);
            if (m_viewMode == ViewMode::Chronological) {
                setViewMode(ViewMode::Grouped);
                if (m_modeCombo != nullptr) {
                    m_modeCombo->setCurrentIndex(0);
                }
            }
        }
    });

    QAction* copyIdAction = menu.addAction(tr("Copy Identifier (%1)").arg(hexId));
    connect(copyIdAction, &QAction::triggered, this, [hexId] {
        if (QClipboard* clip = QGuiApplication::clipboard()) {
            clip->setText(hexId);
        }
    });

    QStringList hexBytes;
    for (std::size_t i = 0; i < frame.payloadSize(); ++i) {
        hexBytes << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    const QString dataStr = hexBytes.join(QLatin1Char(' '));

    QAction* copyDataAction = menu.addAction(tr("Copy Data Bytes"));
    connect(copyDataAction, &QAction::triggered, this, [dataStr] {
        if (QClipboard* clip = QGuiApplication::clipboard()) {
            clip->setText(dataStr);
        }
    });

    menu.addSeparator();

    QAction* transmitAction = menu.addAction(icon("transmit"), tr("Send to Transmit List"));
    connect(transmitAction, &QAction::triggered, this, [this, frame, name] {
        Q_EMIT sendToTransmitRequested(frame, name);
    });

    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

} // namespace torquebus::ui
