// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Transmit panel: hierarchical CAN Send with DBC signal support.

#include "core/database/CanMessage.h"
#include "core/transmit/TransmitList.h"
#include "ui/transmit/TransmitPanel.h"
#include "ui/transmit/TransmitTreeModel.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QTreeView>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

std::shared_ptr<CanDatabase> makeTestDatabase()
{
    auto db = std::make_shared<CanDatabase>();

    CanMessage msg;
    msg.identifier = 0x655;
    msg.format = CanFrameFormat::Standard;
    msg.name = "Out_RTC_SetTime";
    msg.length = 8;

    CanSignal sigSec;
    sigSec.name = "RTC_SetSec";
    sigSec.startBit = 0;
    sigSec.bitLength = 8;
    sigSec.byteOrder = ByteOrder::Intel;
    msg.signalList.push_back(sigSec);

    CanSignal sigHour;
    sigHour.name = "RTC_SetHour";
    sigHour.startBit = 16;
    sigHour.bitLength = 8;
    sigHour.byteOrder = ByteOrder::Intel;
    msg.signalList.push_back(sigHour);

    CanSignal sigMonth;
    sigMonth.name = "RTC_SetMonth";
    sigMonth.startBit = 40;
    sigMonth.bitLength = 8;
    sigMonth.byteOrder = ByteOrder::Intel;
    sigMonth.valueNames.push_back(SignalValueName{11, "November"});
    msg.signalList.push_back(sigMonth);

    db->addMessage(msg);

    CanMessage msgSpeed;
    msgSpeed.identifier = 0x1;
    msgSpeed.format = CanFrameFormat::Standard;
    msgSpeed.name = "EngineSpeed";
    msgSpeed.length = 8;

    CanSignal sigRpm;
    sigRpm.name = "RPM";
    sigRpm.startBit = 0;
    sigRpm.bitLength = 16;
    sigRpm.byteOrder = ByteOrder::Intel;
    sigRpm.factor = 0.125;
    sigRpm.offset = 0.0;
    sigRpm.unit = "rpm";
    msgSpeed.signalList.push_back(sigRpm);

    db->addMessage(msgSpeed);
    return db;
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

    [[nodiscard]] QTreeView* tree() const
    {
        return panel.findChild<QTreeView*>("torquebusTransmitTree");
    }

    [[nodiscard]] QAbstractItemModel* model() const
    {
        return tree() != nullptr ? tree()->model() : nullptr;
    }

    TransmitList list;
    TransmitPanel panel;
};

} // namespace

TEST_F(TransmitPanelTest, AddAppendsAManualRowWithoutHangingOrCrashing)
{
    QAction* add = action(QStringLiteral("Add"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(tree(), nullptr);

    add->trigger();
    settle();

    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(model()->rowCount(), 1);

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_FALSE(entry.isPeriodic());
    EXPECT_TRUE(entry.enabled);
}

TEST_F(TransmitPanelTest, AddingManyRowsKeepsTreeAndListTogether)
{
    QAction* add = action(QStringLiteral("Add"));
    ASSERT_NE(add, nullptr);

    for (int i = 0; i < 25; ++i) {
        add->trigger();
    }
    settle();

    EXPECT_EQ(list.size(), 25U);
    EXPECT_EQ(model()->rowCount(), 25);
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
    EXPECT_EQ(model()->rowCount(), 0);
    EXPECT_FALSE(remove->isEnabled());
}

TEST_F(TransmitPanelTest, EditingTheIdentifierReachesTheList)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    const QModelIndex idIdx = model()->index(0, TransmitTreeModel::ColumnIdentifier, QModelIndex());
    model()->setData(idIdx, QStringLiteral("1F4"), Qt::EditRole);
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.identifier, 0x1F4U);
    EXPECT_EQ(model()->data(idIdx, Qt::DisplayRole).toString(), QStringLiteral("1F4"));
}

TEST_F(TransmitPanelTest, AnUnreadableIdentifierSnapsBack)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    const QModelIndex idIdx = model()->index(0, TransmitTreeModel::ColumnIdentifier, QModelIndex());
    model()->setData(idIdx, QStringLiteral("not hex"), Qt::EditRole);
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.identifier, 0x100U);
    EXPECT_EQ(model()->data(idIdx, Qt::DisplayRole).toString(), QStringLiteral("100"));
}

TEST_F(TransmitPanelTest, AClassicFrameCannotCarryMoreThanEightBytes)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    const QModelIndex dataIdx = model()->index(0, TransmitTreeModel::ColumnData, QModelIndex());
    model()->setData(dataIdx, QStringLiteral("01 02 03 04 05 06 07 08 09"), Qt::EditRole);
    settle();

    TransmitEntry entry;
    ASSERT_TRUE(list.entryAt(0, entry));
    EXPECT_EQ(entry.frame.length, 8U);
}

TEST_F(TransmitPanelTest, ShorterPayloadsUpdateTheLength)
{
    action(QStringLiteral("Add"))->trigger();
    settle();

    const QModelIndex dataIdx = model()->index(0, TransmitTreeModel::ColumnData, QModelIndex());
    model()->setData(dataIdx, QStringLiteral("AA BB"), Qt::EditRole);
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

    EXPECT_EQ(model()->rowCount(), 1);

    list.clear();
    TransmitEntry other;
    other.name = "another";
    list.add(other);

    panel.reload();
    settle();

    const QModelIndex symIdx = model()->index(0, TransmitTreeModel::ColumnSymbol, QModelIndex());
    EXPECT_EQ(model()->data(symIdx, Qt::DisplayRole).toString(), QStringLiteral("another"));
}

TEST_F(TransmitPanelTest, DbcMessageExpandsSignalsAndEditsInline)
{
    auto db = makeTestDatabase();
    panel.setDatabases({db});

    TransmitEntry entry;
    entry.name = "Out_RTC_SetTime";
    entry.messageName = "Out_RTC_SetTime";
    entry.frame.identifier = 0x655;
    entry.frame.length = 8;
    entry.frame.data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    list.add(entry);
    panel.reload();
    settle();

    ASSERT_EQ(model()->rowCount(), 1);
    const QModelIndex msgIdx = model()->index(0, 0, QModelIndex());

    // Message has 3 child signals
    ASSERT_EQ(model()->rowCount(msgIdx), 3);

    const QModelIndex sigSec = model()->index(0, TransmitTreeModel::ColumnSymbol, msgIdx);
    const QModelIndex sigHour = model()->index(1, TransmitTreeModel::ColumnSymbol, msgIdx);
    const QModelIndex sigMonth = model()->index(2, TransmitTreeModel::ColumnSymbol, msgIdx);

    EXPECT_EQ(model()->data(sigSec, Qt::DisplayRole).toString(), QStringLiteral("RTC_SetSec"));
    EXPECT_EQ(model()->data(sigHour, Qt::DisplayRole).toString(), QStringLiteral("RTC_SetHour"));
    EXPECT_EQ(model()->data(sigMonth, Qt::DisplayRole).toString(), QStringLiteral("RTC_SetMonth"));

    // Edit RTC_SetHour to 17
    const QModelIndex hourData = model()->index(1, TransmitTreeModel::ColumnData, msgIdx);
    model()->setData(hourData, QStringLiteral("17"), Qt::EditRole);
    settle();

    TransmitEntry updated;
    ASSERT_TRUE(list.entryAt(0, updated));
    EXPECT_EQ(updated.frame.data[2], 17); // byte 2 is startBit 16

    // Edit RTC_SetMonth to "November" (enum value 11)
    const QModelIndex monthData = model()->index(2, TransmitTreeModel::ColumnData, msgIdx);
    model()->setData(monthData, QStringLiteral("November"), Qt::EditRole);
    settle();

    ASSERT_TRUE(list.entryAt(0, updated));
    EXPECT_EQ(updated.frame.data[5], 11); // byte 5 is startBit 40
}

TEST_F(TransmitPanelTest, FilterReducesVisibleTransmitItems)
{
    auto db = makeTestDatabase();
    panel.setDatabases({db});

    TransmitEntry entry1;
    entry1.name = "Out_RTC_SetTime";
    entry1.frame.identifier = 0x655;
    entry1.frame.length = 8;

    TransmitEntry entry2;
    entry2.name = "EngineSpeedCmd";
    entry2.frame.identifier = 0x200;
    entry2.frame.length = 8;

    list.add(entry1);
    list.add(entry2);
    panel.reload();
    settle();

    EXPECT_EQ(model()->rowCount(), 2);

    auto* filterEdit = panel.findChild<QLineEdit*>();
    ASSERT_NE(filterEdit, nullptr);

    // Filter for "RTC"
    filterEdit->setText(QStringLiteral("RTC"));
    settle();

    EXPECT_EQ(model()->rowCount(), 1);

    // Clear filter
    filterEdit->clear();
    settle();
    EXPECT_EQ(model()->rowCount(), 2);
}

TEST_F(TransmitPanelTest, EditingSignalValueWithUnitUpdatesMessageBytesAndDisplay)
{
    auto db = makeTestDatabase();
    panel.setDatabases({db});

    TransmitEntry entry;
    entry.name = "EngineSpeed";
    entry.messageName = "EngineSpeed";
    entry.frame.identifier = 0x001;
    entry.frame.length = 8;
    // 512 rpm: 512 / 0.125 = 4096 = 0x1000 -> byte0=0x00, byte1=0x10
    entry.frame.data = {0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    list.add(entry);
    panel.reload();
    settle();

    ASSERT_EQ(model()->rowCount(), 1);
    const QModelIndex msgIdx = model()->index(0, 0, QModelIndex());
    ASSERT_EQ(model()->rowCount(msgIdx), 1);

    const QModelIndex sigData = model()->index(0, TransmitTreeModel::ColumnData, msgIdx);
    const QModelIndex msgData = model()->index(0, TransmitTreeModel::ColumnData, QModelIndex());

    // DisplayRole shows formatted with '=' and unit
    EXPECT_EQ(model()->data(sigData, Qt::DisplayRole).toString(), QStringLiteral("= 512 rpm"));
    // EditRole provides clean numeric value for the editor
    EXPECT_EQ(model()->data(sigData, Qt::EditRole).toString(), QStringLiteral("512"));

    // User edits the value to "1000 rpm"
    model()->setData(sigData, QStringLiteral("1000 rpm"), Qt::EditRole);
    settle();

    // 1000 / 0.125 = 8000 = 0x1F40 -> byte0 = 0x40, byte1 = 0x1F
    TransmitEntry updated;
    ASSERT_TRUE(list.entryAt(0, updated));
    EXPECT_EQ(updated.frame.data[0], 0x40);
    EXPECT_EQ(updated.frame.data[1], 0x1F);

    // Both signal and message display rows update immediately
    EXPECT_EQ(model()->data(sigData, Qt::DisplayRole).toString(), QStringLiteral("= 1000 rpm"));
    EXPECT_TRUE(
        model()->data(msgData, Qt::DisplayRole).toString().startsWith(QStringLiteral("40 1F 00")));
}
