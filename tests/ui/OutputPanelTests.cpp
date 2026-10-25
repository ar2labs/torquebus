// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/output/OutputPanel.h"
#include "ui/theme/ThemeManager.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPlainTextEdit>

#include <memory>

using namespace torquebus::ui;

TEST(OutputPanelTests, AnEmptyOutputPanelHasAClearActionThatIsInitiallyDisabled)
{
    ThemeManager themes;
    OutputPanel panel;

    QAction* clear = panel.clearAction();
    ASSERT_NE(clear, nullptr);
    EXPECT_EQ(clear->objectName(), QStringLiteral("actionClearOutput"));
    EXPECT_EQ(clear->text(), QStringLiteral("Clear Output"));
    EXPECT_FALSE(clear->isEnabled());
    EXPECT_FALSE(clear->icon().isNull());
}

TEST(OutputPanelTests, AppendingTextEnablesTheClearAction)
{
    ThemeManager themes;
    OutputPanel panel;

    QAction* clear = panel.clearAction();
    ASSERT_NE(clear, nullptr);
    EXPECT_FALSE(clear->isEnabled());

    panel.appendInfo(QStringLiteral("Subsystem initialised"));
    EXPECT_TRUE(clear->isEnabled());
}

TEST(OutputPanelTests, TriggeringTheClearActionEmptiesTheViewAndDisablesTheAction)
{
    ThemeManager themes;
    OutputPanel panel;

    panel.appendWarning(QStringLiteral("High temperature detected"));
    QAction* clear = panel.clearAction();
    ASSERT_NE(clear, nullptr);
    EXPECT_TRUE(clear->isEnabled());

    auto* view = panel.findChild<QPlainTextEdit*>();
    ASSERT_NE(view, nullptr);
    EXPECT_FALSE(view->document()->isEmpty());

    clear->trigger();

    EXPECT_TRUE(view->document()->isEmpty());
    EXPECT_FALSE(clear->isEnabled());
}

TEST(OutputPanelTests, TheContextMenuIncludesStandardActionsAndTheClearAction)
{
    ThemeManager themes;
    OutputPanel panel;

    panel.append(OutputPanel::Level::Debug, QStringLiteral("Raw CAN frame received"));

    std::unique_ptr<QMenu> menu{panel.createContextMenu()};
    ASSERT_NE(menu, nullptr);

    const QList<QAction*> actions = menu->actions();
    EXPECT_TRUE(actions.contains(panel.clearAction()));
    EXPECT_TRUE(panel.clearAction()->isEnabled());

    bool hasSelectAll = false;
    for (const QAction* action : actions) {
        if (action->text().contains(QStringLiteral("Select All"))) {
            hasSelectAll = true;
            break;
        }
    }
    EXPECT_TRUE(hasSelectAll);
}

TEST(OutputPanelTests, TheContextMenuActionReflectsEmptyStateWhenCleared)
{
    ThemeManager themes;
    OutputPanel panel;

    std::unique_ptr<QMenu> menu{panel.createContextMenu()};
    ASSERT_NE(menu, nullptr);

    EXPECT_FALSE(panel.clearAction()->isEnabled());
}

TEST(OutputPanelTests, AThemeChangeKeepsClearActionIconValid)
{
    ThemeManager themes;
    OutputPanel panel;

    QAction* clear = panel.clearAction();
    ASSERT_NE(clear, nullptr);
    EXPECT_FALSE(clear->icon().isNull());

    themes.toggleVariant();
    EXPECT_FALSE(clear->icon().isNull());
}
