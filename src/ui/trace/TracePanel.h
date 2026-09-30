// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The CAN Trace panel: the thing people actually open TorqueBus to look at.

#pragma once

#include "core/trace/TraceStore.h"
#include "ui/theme/Theme.h"

#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QLabel;
class QTableView;
class QToolBar;

namespace torquebus {
class CanDatabase;
}

namespace torquebus::ui {

class TraceModel;

class TracePanel final : public QWidget {
    Q_OBJECT

public:
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

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onRowsAppended(int firstRow, int lastRow);
    void onScrolled();
    void onFreezeToggled(bool frozen);
    void onClear();
    void onThemeChanged(const torquebus::ui::Theme& theme);
    void refreshStatus();

private:
    void createToolBar();
    void createView();
    void applyColumnWidths();

    TraceModel* m_model{nullptr};
    QTableView* m_view{nullptr};
    QToolBar* m_toolBar{nullptr};

    QAction* m_actionFreeze{nullptr};
    QAction* m_actionClear{nullptr};
    QAction* m_actionFollow{nullptr};

    QLabel* m_statusLabel{nullptr};

    const TraceStore* m_store{nullptr};

    /// Whether the view scrolls to the newest row as rows arrive.
    ///
    /// Turned off automatically when the user scrolls away from the bottom, and
    /// back on when they return to it. A trace that yanks itself to the end
    /// while someone is reading a frame from four seconds ago is unusable, and
    /// making them find a button to stop it is only slightly better.
    bool m_following{true};

    /// Guards against the auto-scroll we perform being mistaken for the user
    /// scrolling away.
    bool m_scrollingProgrammatically{false};
};

} // namespace torquebus::ui
