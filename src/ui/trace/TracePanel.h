// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The CAN Trace panel: grouped tree and chronological views.

#pragma once

#include "core/trace/TraceStore.h"
#include "ui/theme/Theme.h"

#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QToolBar;
class QTreeView;

namespace torquebus {
class CanDatabase;
}

namespace torquebus::ui {

class TraceModel;
class TraceTreeModel;
class TraceTreeFilterModel;
class TraceTreeDelegate;

class TracePanel final : public QWidget {
    Q_OBJECT

public:
    enum class ViewMode {
        Grouped, ///< Hierarchical tree view by ID with decoded signals
        Chronological, ///< Linear streaming table view
        Split ///< Both side by side in a splitter
    };
    Q_ENUM(ViewMode)

    explicit TracePanel(QWidget* parent = nullptr);

    /// Attaches the store this panel displays. Not owned.
    void setStore(const TraceStore* store);

    /// Databases used to name messages and fill the Signals column.
    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

    /// Applies the Trace preferences. Called by the window when the
    /// Preferences dialog changes them, and once at startup.
    void applyPreferences(int refreshMs, bool decimalIdentifiers);

    /// True while the view is following new rows.
    [[nodiscard]] bool isFollowing() const noexcept { return m_following; }

    void setViewMode(ViewMode mode);
    [[nodiscard]] ViewMode viewMode() const noexcept { return m_viewMode; }

    /// Polls both models immediately against the store.
    void poll();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onRowsAppended(int firstRow, int lastRow);
    void onScrolled();
    void onFreezeToggled(bool frozen);
    void onClear();
    void onThemeChanged(const torquebus::ui::Theme& theme);
    void refreshStatus();
    void onExpandAll();
    void onCollapseAll();
    void onViewModeChanged(int index);

private:
    void createToolBar();
    void createViews();
    void applyColumnWidths();

    TraceModel* m_model{nullptr};
    QTableView* m_view{nullptr};

    TraceTreeModel* m_treeModel{nullptr};
    TraceTreeFilterModel* m_proxyModel{nullptr};
    QTreeView* m_treeView{nullptr};

    QSplitter* m_splitter{nullptr};
    QToolBar* m_toolBar{nullptr};

    QComboBox* m_modeCombo{nullptr};
    QLineEdit* m_filterEdit{nullptr};

    QAction* m_actionFreeze{nullptr};
    QAction* m_actionClear{nullptr};
    QAction* m_actionExpandAll{nullptr};
    QAction* m_actionCollapseAll{nullptr};
    QAction* m_actionFollow{nullptr};

    QLabel* m_statusLabel{nullptr};

    const TraceStore* m_store{nullptr};

    ViewMode m_viewMode{ViewMode::Grouped};
    bool m_following{true};
    bool m_scrollingProgrammatically{false};
};

} // namespace torquebus::ui
