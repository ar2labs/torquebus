// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transmit panel. Its first test is a regression: Add closed the application.
// Every reload() set a flag on each row's Length cell *after* the cell was in the
// table, which emits itemChanged, which committed the row, which reloaded - until
// the stack ran out. The cases below drive the panel through its real toolbar and
// table, because the bug lived in the wiring and not in any one function.

#include "core/transmit/TransmitList.h"
#include "ui/transmit/TransmitPanel.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QTableWidget>
#include <QTableWidgetItem>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

constexpr int kColumnIdentifier = 3;
constexpr int kColumnData = 6;

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

class TransmitPanelTest : public testing::Test {
protected:
    TransmitPanelTest()
        : panel{list}
    {
        panel.resize(900, 400);
        panel.show();
        settle();
    }

    [[nodiscard]] QAction* action(const QString& text) const
    {
        for (QAction* candidate : panel.findChildren<QAction*>()) {
            if (candidate->text() == text) {
                return candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] QTableWidget* table() const { return panel.findChild<QTableWidget*>(); }

    TransmitList list;
    TransmitPanel panel;
};

TEST_F(TransmitPanelTest, AddAppendsAManualRowWithoutHangingOrCrashing)
{
    QAction* add = action(QStringLiteral("Add"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(table(), nullptr);

    add->trigger();
    settle();

    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(table()->rowCount(), 1);

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_FALSE(entry.isPeriodic());
    EXPECT_TRUE(entry.enabled);
}

TEST_F(TransmitPanelTest, AddingManyRowsKeepsTableAndListTogether)
{
    QAction* add = action(QStringLiteral("Add"));
    ASSERT_NE(add, nullptr);

    for (int i = 0; i < 25; ++i) {
        add->trigger();
    }
    settle();

    EXPECT_EQ(list.size(), 25U);
    EXPECT_EQ(table()->rowCount(), 25);
}

TEST_F(TransmitPanelTest, RemoveAndSendFollowTheSelection)
{
    QAction* add = action(QStringLiteral("Add"));
    QAction* remove = action(QStringLiteral("Remove"));
    QAction* send = action(QStringLiteral("Send"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(send, nullptr);

    EXPECT_FALSE(remove->isEnabled());
    EXPECT_FALSE(send->isEnabled());

    add->trigger();
    settle();

    // Add selects the row it created.
    EXPECT_TRUE(remove->isEnabled());
    EXPECT_TRUE(send->isEnabled());

    remove->trigger();
    settle();

    EXPECT_EQ(list.size(), 0U);
    EXPECT_EQ(table()->rowCount(), 0);
    EXPECT_FALSE(remove->isEnabled());
}

TEST_F(TransmitPanelTest, EditingTheIdentifierReachesTheList)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    table()->item(0, kColumnIdentifier)->setText(QStringLiteral("1F4"));
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.identifier, 0x1F4U);
    EXPECT_EQ(table()->item(0, kColumnIdentifier)->text(), QStringLiteral("1F4"));
}

TEST_F(TransmitPanelTest, AnUnreadableIdentifierSnapsBack)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    table()->item(0, kColumnIdentifier)->setText(QStringLiteral("not hex"));
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.identifier, 0x100U);
    EXPECT_EQ(table()->item(0, kColumnIdentifier)->text(), QStringLiteral("100"));
}

TEST_F(TransmitPanelTest, AClassicFrameCannotCarryMoreThanEightBytes)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    table()->item(0, kColumnData)->setText(QStringLiteral("01 02 03 04 05 06 07 08 09"));
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.length, 8U);
}

TEST_F(TransmitPanelTest, ShorterPayloadsUpdateTheLength)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    table()->item(0, kColumnData)->setText(QStringLiteral("AA BB"));
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.length, 2U);
    EXPECT_EQ(entry.frame.data[0], 0xAA);
    EXPECT_EQ(entry.frame.data[1], 0xBB);
}

TEST_F(TransmitPanelTest, ReloadShowsAListReplacedUnderThePanel)
{
    TransmitEntry entry;
    entry.name = "from a project";
    list.add(entry);
    panel.reload();
    settle();

    EXPECT_EQ(table()->rowCount(), 1);

    list.clear();
    TransmitEntry other;
    other.name = "another";
    list.add(other);

    // Same row count, different content: only an explicit reload can tell.
    panel.reload();
    settle();

    EXPECT_EQ(table()->item(0, 2)->text(), QStringLiteral("another"));
}

} // namespace
