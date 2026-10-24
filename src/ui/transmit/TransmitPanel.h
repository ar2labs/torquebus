// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Transmit (CAN Send) panel: hierarchical tree view with DBC signal support.

#pragma once

#include "core/database/CanMessage.h"
#include "core/transmit/TransmitEntry.h"

#include <QPersistentModelIndex>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QFrame;
class QLabel;
class QLineEdit;
class QTimer;
class QToolBar;
class QTreeView;

namespace torquebus {
class TransmitList;
}

namespace torquebus::ui {

class TransmitTreeModel;
class TransmitTreeFilterModel;
class TransmitTreeDelegate;

class TransmitPanel final : public QWidget {
    Q_OBJECT

public:
    explicit TransmitPanel(TransmitList& list, QWidget* parent = nullptr);

    /// Databases offered by "Add from message" and used to decode child signals.
    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

    /// Rebuilds every row from the list.
    void reload();

    /// Selects and scrolls to the row at `index` in the transmit list.
    void selectEntry(int index);

    /// Tells the panel whether a measurement is running. Send only transmits on
    /// a running bus; without this the button would queue a frame that is
    /// dropped when the measurement starts, and appear to do nothing.
    void setMeasurementRunning(bool running) noexcept { m_measurementRunning = running; }

    [[nodiscard]] QTreeView* treeView() const noexcept { return m_treeView; }
    [[nodiscard]] TransmitTreeModel* treeModel() const noexcept { return m_treeModel; }

Q_SIGNALS:
    void reported(const QString& message);
    void modified();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onAdd();
    void onAddFromMessage();
    void onRemove();
    void onSendSelected();
    void onEditSignals();
    void onExpandAll();
    void onCollapseAll();
    void refreshCounters();
    void onThemeChanged();

private:
    void buildUi();
    void buildSymbolCard();
    void updateSymbolCardAt(const QPoint& viewportPos);
    void hideSymbolCard();
    void updateActionState();
    [[nodiscard]] int selectedMessageIndex() const;

    TransmitList& m_list;
    bool m_measurementRunning{false};
    std::vector<std::shared_ptr<const CanDatabase>> m_databases;

    TransmitTreeModel* m_treeModel{nullptr};
    TransmitTreeFilterModel* m_proxyModel{nullptr};
    QTreeView* m_treeView{nullptr};

    QFrame* m_symbolCard{nullptr};
    QLabel* m_cardTitle{nullptr};
    QLabel* m_cardMeta{nullptr};
    QLabel* m_cardBody{nullptr};
    QLabel* m_cardValues{nullptr};
    QPersistentModelIndex m_hoveredSymbolIndex;

    QToolBar* m_toolBar{nullptr};
    QLineEdit* m_filterEdit{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_timer{nullptr};

    QAction* m_actionAdd{nullptr};
    QAction* m_actionAddFromMessage{nullptr};
    QAction* m_actionRemove{nullptr};
    QAction* m_actionSend{nullptr};
    QAction* m_actionEditSignals{nullptr};
    QAction* m_actionExpandAll{nullptr};
    QAction* m_actionCollapseAll{nullptr};
};

} // namespace torquebus::ui
