// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/mainwindow/Docking.h"
#include "ui/theme/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <memory>

using namespace torquebus::ui;

namespace {

void settleLayout()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

void clickTab(QTabBar* tabs, int index)
{
    const QPointF position{tabs->tabRect(index).center()};
    const QPointF global{tabs->mapToGlobal(position.toPoint())};
    QMouseEvent press{
        QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(tabs, &press);
    QMouseEvent release{
        QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
    QApplication::sendEvent(tabs, &release);
    settleLayout();
}

class BottomPanelTest : public testing::Test {
protected:
    void SetUp() override
    {
        themes.applyVariant(ThemeVariant::Dark);
        window.resize(1000, 700);
        window.show();
        settleLayout();

        auto* mainContent = new QWidget;
        mainContent->setMinimumSize(250, 150);
        mainDock.reset(
            createDockWidget(QStringLiteral("test-main"), QStringLiteral("Main"), mainContent));
        addDockTo(&window, mainDock.get(), DockLocation::Top);

        auto* outputContent = new QWidget;
        outputContent->setMinimumSize(250, 120);
        output.reset(createDockWidget(
            QStringLiteral("test-output"), QStringLiteral("Output"), outputContent));
        addDockNextTo(&window, output.get(), DockLocation::Bottom, mainDock.get(), QSize{0, 220});
        statistics.reset(createDockWidget(
            QStringLiteral("test-statistics"), QStringLiteral("Statistics"), new QWidget));
        output->addDockWidgetAsTab(statistics.get());
        output->setAsCurrentTab();

        button = new QToolButton;
        button->setObjectName(QStringLiteral("dockTabCornerButton"));
        button->setIconSize(QSize{20, 20});
        button->setFixedSize(32, 28);
        QObject::connect(button, &QToolButton::clicked, &window, [this] {
            setDockGroupCollapsed(output.get(), !isDockGroupCollapsed(output.get()));
        });
        setDockGroupCornerWidget(output.get(), button, [this](bool collapsed) {
            button->setIcon(themes.icon(collapsed ? QStringLiteral("panel-expand")
                                                  : QStringLiteral("panel-collapse")));
        });
        settleLayout();
        for (QWidget* parent = output->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* tabWidget = qobject_cast<QTabWidget*>(parent)) {
                tabs = tabWidget->findChild<QTabBar*>();
                pages = tabWidget->findChild<QStackedWidget*>();
                group = tabWidget->parentWidget();
                break;
            }
        }
        ASSERT_NE(tabs, nullptr);
        ASSERT_NE(pages, nullptr);
        ASSERT_NE(group, nullptr);
        button->setIcon(themes.icon(QStringLiteral("panel-collapse")));
    }

    void collapse()
    {
        const int bottom = group->mapToGlobal(group->rect().bottomLeft()).y();
        button->click();
        settleLayout();
        ASSERT_TRUE(isDockGroupCollapsed(output.get()));
        EXPECT_FALSE(pages->isVisible());
        EXPECT_TRUE(tabs->isVisible());
        EXPECT_TRUE(button->isVisible());
        EXPECT_LE(group->height(), tabs->height() + 2);
        EXPECT_EQ(group->mapToGlobal(group->rect().bottomLeft()).y(), bottom);
    }

    ThemeManager themes;
    DockMainWindowBase window{QStringLiteral("test-bottom-panel")};
    std::unique_ptr<DockWidget> mainDock;
    std::unique_ptr<DockWidget> output;
    std::unique_ptr<DockWidget> statistics;
    QToolButton* button{nullptr};
    QTabBar* tabs{nullptr};
    QStackedWidget* pages{nullptr};
    QWidget* group{nullptr};
};

TEST_F(BottomPanelTest, ButtonRestoresPreviousHeightRepeatedly)
{
    const int expandedHeight = group->height();
    for (int i = 0; i < 3; ++i) {
        collapse();
        button->click();
        settleLayout();
        EXPECT_FALSE(isDockGroupCollapsed(output.get()));
        EXPECT_TRUE(pages->isVisible());
        EXPECT_EQ(group->height(), expandedHeight);
    }
}

TEST_F(BottomPanelTest, ClickingCurrentOrOtherTabExpands)
{
    const int expandedHeight = group->height();
    collapse();
    clickTab(tabs, tabs->currentIndex());
    EXPECT_FALSE(isDockGroupCollapsed(output.get()));
    EXPECT_TRUE(pages->isVisible());
    EXPECT_EQ(group->height(), expandedHeight);

    collapse();
    clickTab(tabs, 1);
    EXPECT_FALSE(isDockGroupCollapsed(output.get()));
    EXPECT_EQ(tabs->currentIndex(), 1);
    EXPECT_TRUE(statistics->isVisible());
    EXPECT_EQ(group->height(), expandedHeight);
}

TEST_F(BottomPanelTest, WindowResizeAndThemeChangeKeepOnlyTabs)
{
    const int expandedHeight = group->height();
    collapse();
    window.resize(1200, 900);
    themes.applyVariant(ThemeVariant::Light);
    settleLayout();
    EXPECT_TRUE(isDockGroupCollapsed(output.get()));
    EXPECT_FALSE(pages->isVisible());
    EXPECT_LE(group->height(), tabs->height() + 2);
    EXPECT_FALSE(button->icon().isNull());
    button->click();
    settleLayout();
    EXPECT_EQ(group->height(), expandedHeight);
}

} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    if (GTEST_FLAG_GET(list_tests)) {
        return RUN_ALL_TESTS();
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app{argc, argv};
    configureDockingSystem();
    return RUN_ALL_TESTS();
}
