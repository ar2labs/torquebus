// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Transmit (CAN Send) panel: hierarchical tree view with DBC signal support.

#pragma once

#include "core/database/CanMessage.h"
#include "core/transmit/TransmitEntry.h"

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QAction;
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

    [[nodiscard]] QTreeView* treeView() const noexcept { return m_treeView; }
    [[nodiscard]] TransmitTreeModel* treeModel() const noexcept { return m_treeModel; }

Q_SIGNALS:
    void reported(const QString& message);

protected:
    void keyPressEvent(QKeyEvent* event) override;

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
    void updateActionState();
    [[nodiscard]] int selectedMessageIndex() const;

    TransmitList& m_list;
    std::vector<std::shared_ptr<const CanDatabase>> m_databases;

    TransmitTreeModel* m_treeModel{nullptr};
    TransmitTreeFilterModel* m_proxyModel{nullptr};
    QTreeView* m_treeView{nullptr};

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
