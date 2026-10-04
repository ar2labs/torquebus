// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/can/CanFrame.h"
#include "core/database/CanMessage.h"
#include "core/trace/TraceStore.h"
#include "ui/trace/TracePanel.h"
#include "ui/trace/TraceTreeFilterModel.h"
#include "ui/trace/TraceTreeModel.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTableView>
#include <QTreeView>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void settle()
{
    for (int i = 0; i < 10; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

CanFrame makeFrame(std::uint32_t id,
                   CanDirection direction = CanDirection::Rx,
                   CanFrameFormat format = CanFrameFormat::Standard,
                   std::uint8_t length = 8)
{
    CanFrame frame;
    frame.identifier = id;
    frame.direction = direction;
    frame.format = format;
    frame.length = length;
    frame.dlc = length;
    frame.timestampNs = 1'000'000;
    frame.data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    return frame;
}

std::shared_ptr<CanDatabase> makeTestDatabase()
{
    auto db = std::make_shared<CanDatabase>();

    CanMessage msg;
    msg.identifier = 0x100;
    msg.format = CanFrameFormat::Standard;
    msg.name = "EngineData";
    msg.comment = "Engine status broadcast";

    CanSignal sigSpeed;
    sigSpeed.name = "VehicleSpeed";
    sigSpeed.startBit = 0;
    sigSpeed.bitLength = 16;
    sigSpeed.byteOrder = ByteOrder::Intel;
    sigSpeed.factor = 0.1;
    sigSpeed.offset = 0.0;
    sigSpeed.unit = "km/h";
    sigSpeed.comment = "Calculated vehicle speed";
    msg.signalList.push_back(sigSpeed);

    CanSignal sigRpm;
    sigRpm.name = "EngineRPM";
    sigRpm.startBit = 16;
    sigRpm.bitLength = 16;
    sigRpm.byteOrder = ByteOrder::Intel;
    sigRpm.factor = 1.0;
    sigRpm.offset = 0.0;
    sigRpm.unit = "rpm";
    msg.signalList.push_back(sigRpm);

    db->addMessage(msg);
    return db;
}

} // namespace

TEST(TracePanelTests, InitializesInGroupedTreeMode)
{
    TraceStore store{100};
    TracePanel panel;
    panel.setStore(&store);
    panel.resize(900, 500);
    panel.show();
    settle();

    auto* tree = panel.findChild<QTreeView*>("torquebusTraceTree");
    auto* table = panel.findChild<QTableView*>("torquebusTraceTable");

    ASSERT_NE(tree, nullptr);
    ASSERT_NE(table, nullptr);

    EXPECT_EQ(panel.viewMode(), TracePanel::ViewMode::Grouped);
    EXPECT_TRUE(tree->isVisible());
    EXPECT_FALSE(table->isVisible());
}

TEST(TracePanelTests, CanSwitchModesBetweenGroupedStreamAndSplit)
{
    TraceStore store{100};
    TracePanel panel;
    panel.setStore(&store);
    panel.resize(900, 500);
    panel.show();
    settle();

    auto* tree = panel.findChild<QTreeView*>("torquebusTraceTree");
    auto* table = panel.findChild<QTableView*>("torquebusTraceTable");

    // 1. Chronological
    panel.setViewMode(TracePanel::ViewMode::Chronological);
    settle();
    EXPECT_FALSE(tree->isVisible());
    EXPECT_TRUE(table->isVisible());

    // 2. Split
    panel.setViewMode(TracePanel::ViewMode::Split);
    settle();
    EXPECT_TRUE(tree->isVisible());
    EXPECT_TRUE(table->isVisible());

    // 3. Back to Grouped
    panel.setViewMode(TracePanel::ViewMode::Grouped);
    settle();
    EXPECT_TRUE(tree->isVisible());
    EXPECT_FALSE(table->isVisible());
}

TEST(TracePanelTests, SeparatesReceiveAndTransmitAndDecodesDbcSignals)
{
    TraceStore store{100};
    TracePanel panel;
    panel.setStore(&store);

    auto db = makeTestDatabase();
    panel.setDatabases({db});

    panel.resize(1000, 600);
    panel.show();
    settle();

    // Append 1 Rx frame for 0x100 and 1 Tx frame for 0x200
    const CanFrame rxFrame = makeFrame(0x100, CanDirection::Rx);
    const CanFrame txFrame = makeFrame(0x200, CanDirection::Tx);
    store.append(std::span<const CanFrame>{&rxFrame, 1});
    store.append(std::span<const CanFrame>{&txFrame, 1});

    // Refresh model
    panel.poll();
    settle();

    auto* tree = panel.findChild<QTreeView*>("torquebusTraceTree");
    ASSERT_NE(tree, nullptr);
    auto* model = tree->model();
    ASSERT_NE(model, nullptr);

    // Root should have 2 children: Receive (row 0) and Transmit (row 1)
    EXPECT_EQ(model->rowCount(), 2);

    const QModelIndex rxGroup = model->index(0, 0);
    const QModelIndex txGroup = model->index(1, 0);

    EXPECT_EQ(model->data(rxGroup, Qt::DisplayRole).toString(), QStringLiteral("Receive"));
    EXPECT_EQ(model->data(txGroup, Qt::DisplayRole).toString(), QStringLiteral("Transmit"));

    // Receive group has 1 message (0x100)
    EXPECT_EQ(model->rowCount(rxGroup), 1);
    const QModelIndex rxMsg = model->index(0, 0, rxGroup);
    EXPECT_EQ(model->data(model->index(0, ColumnSymbol, rxGroup), Qt::DisplayRole).toString(),
              QStringLiteral("EngineData"));

    // Check decoded DBC signals under 0x100
    EXPECT_EQ(model->rowCount(rxMsg), 2);
    const QModelIndex sig0 = model->index(0, ColumnSymbol, rxMsg);
    const QModelIndex sig0Data = model->index(0, ColumnData, rxMsg);
    const QModelIndex sig1 = model->index(1, ColumnSymbol, rxMsg);

    EXPECT_TRUE(model->data(sig0, Qt::DisplayRole).toString().contains("VehicleSpeed"));
    EXPECT_TRUE(model->data(sig0Data, Qt::DisplayRole).toString().contains("km/h"));
    EXPECT_TRUE(model->data(sig1, Qt::DisplayRole).toString().contains("EngineRPM"));

    // Transmit group has 1 message (0x200)
    EXPECT_EQ(model->rowCount(txGroup), 1);
}

TEST(TracePanelTests, FilterReducesVisibleTreeItems)
{
    TraceStore store{100};
    TracePanel panel;
    panel.setStore(&store);

    auto db = makeTestDatabase();
    panel.setDatabases({db});

    panel.resize(1000, 600);
    panel.show();
    settle();

    const CanFrame rxFrame1 = makeFrame(0x100, CanDirection::Rx);
    const CanFrame rxFrame2 = makeFrame(0x300, CanDirection::Rx);
    store.append(std::span<const CanFrame>{&rxFrame1, 1});
    store.append(std::span<const CanFrame>{&rxFrame2, 1});

    panel.poll();
    settle();

    auto* filterEdit = panel.findChild<QLineEdit*>();
    ASSERT_NE(filterEdit, nullptr);

    auto* tree = panel.findChild<QTreeView*>("torquebusTraceTree");
    ASSERT_NE(tree, nullptr);
    auto* model = tree->model();

    const QModelIndex rxGroup = model->index(0, 0);
    EXPECT_EQ(model->rowCount(rxGroup), 2);

    // Filter by "VehicleSpeed"
    filterEdit->setText(QStringLiteral("VehicleSpeed"));
    settle();

    // Only message 0x100 (which contains VehicleSpeed) should be visible under Receive
    EXPECT_EQ(model->rowCount(rxGroup), 1);

    // Clear filter
    filterEdit->clear();
    settle();
    EXPECT_EQ(model->rowCount(rxGroup), 2);
}
