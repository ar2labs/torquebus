// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the J1939 Network panel says about faults.
//
// The panel keeps what each ECU last said, which is what a panel is for after a measurement has
// ended. The block also says what the bus is reporting *now*, and the two are different things: an
// ECU that has gone quiet, or a measurement that has stopped, leaves its last DM1 on screen - and a
// panel that went on calling it "active faults", and counting it, would be telling somebody who
// stopped the measurement that the machine is on fire.
//
// The panel is given a network of its own and what the block would publish is published into it,
// by the one door the engine's thread uses.
//
// "Said now" is two things, and the panel needs both: the codes the bus is reporting, and the ECUs
// that are repeating a DM1. The first cannot say anything of a healthy ECU, which has no code - so
// an ECU that said it was fine and then went quiet is only seen through the second.

#include "core/can/CanEngine.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939Id.h"
#include "core/j1939/J1939Network.h"
#include "core/j1939/J1939Request.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "services/ProjectFile.h"
#include "ui/j1939/J1939NetworkPanel.h"
#include "ui/j1939/J1939RequestDialog.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <vector>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

/// Runs the event loop until `done` or two seconds have passed. The panel looks at the network from
/// a timer of its own, so there is no single call after which it has.
bool waitFor(const std::function<bool()>& done, int milliseconds = 2000)
{
    QElapsedTimer clock;
    clock.start();

    while (!done()) {
        if (clock.elapsed() > milliseconds) {
            return false;
        }

        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    return true;
}

[[nodiscard]] J1939NetworkNode nodeAt(std::uint8_t address)
{
    J1939NetworkNode node;
    node.address = address;
    node.trafficSeen = true;
    node.framesSeen = 10U;
    return node;
}

[[nodiscard]] J1939Dtc code(std::uint32_t spn, std::uint8_t fmi)
{
    J1939Dtc dtc;
    dtc.spn = spn;
    dtc.spnAssembled = true;
    dtc.fmi = fmi;
    dtc.occurrenceCount = 1U;
    return dtc;
}

/// What an ECU last said: a DM1 under the red stop lamp that carries `codes`, or the placeholder of
/// a healthy one when there are none.
[[nodiscard]] J1939Diagnostic dm1From(std::uint8_t address, std::vector<J1939Dtc> codes)
{
    J1939Diagnostic message;
    message.active = true;
    message.sourceAddress = address;
    message.lamps.redStop = codes.empty() ? J1939LampState::Off : J1939LampState::On;
    message.faults = std::move(codes);
    return message;
}

[[nodiscard]] J1939Diagnostic dm2From(std::uint8_t address, std::vector<J1939Dtc> codes)
{
    J1939Diagnostic message = dm1From(address, std::move(codes));
    message.active = false;
    return message;
}

/// What the block hands over of a bus that is being reported now.
struct Reported final {
    /// The codes, whichever ECU they came from.
    std::vector<J1939ActiveFault> faults;

    /// The ECUs that are repeating a DM1, the healthy ones included.
    std::vector<std::uint8_t> sources;
};

/// Everything `messages` say, as if every one of them were still being repeated.
[[nodiscard]] Reported reportedNow(const std::vector<J1939Diagnostic>& messages)
{
    return {j1939ActiveFaults(messages, 0U), j1939Reporting(messages, 0U)};
}

struct Bench final {
    J1939Network network;
    J1939NetworkPanel panel;

    Bench()
    {
        panel.setNetwork(&network);
        panel.resize(900, 400);
        panel.show();
    }

    /// Publishes what the block would, and waits for the panel to have drawn it.
    [[nodiscard]] bool publish(std::vector<J1939NetworkNode> nodes,
                               std::vector<J1939Diagnostic> messages,
                               Reported reported)
    {
        network.publish(std::move(nodes),
                        {},
                        std::move(messages),
                        std::move(reported.faults),
                        std::move(reported.sources));

        return waitFor([this] { return tree()->topLevelItemCount() > 0; });
    }

    [[nodiscard]] QTreeWidget* tree() const { return panel.findChild<QTreeWidget*>(); }

    [[nodiscard]] QString summary() const { return panel.findChild<QLabel*>()->text(); }

    /// The row of the ECU at `address`, or null.
    [[nodiscard]] QTreeWidgetItem* ecu(std::uint8_t address) const
    {
        for (int i = 0; i < tree()->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree()->topLevelItem(i);

            if (item->text(0).startsWith(QString::number(address) + QLatin1Char(' '))) {
                return item;
            }
        }

        return nullptr;
    }

    /// What the group under an ECU's row is called: "active faults", "last reported", ...
    [[nodiscard]] QString groupOf(std::uint8_t address, int index = 0) const
    {
        const QTreeWidgetItem* row = ecu(address);

        return row != nullptr && index < row->childCount() ? row->child(index)->text(0) : QString{};
    }

    /// Waits until the panel says `text` in its summary, which is how it shows that it has seen the
    /// publication that was just made.
    [[nodiscard]] bool waitForSummary(const QString& text) const
    {
        return waitFor([this, &text] { return summary().contains(text); });
    }
};

} // namespace

TEST(J1939NetworkPanelTests, AnEcuThatIsReportingFaultsNowIsLabelledActiveAndCounted)
{
    Bench bench;

    const std::vector<J1939Diagnostic> messages{dm1From(0U, {code(100U, 1U), code(110U, 0U)})};
    ASSERT_TRUE(bench.publish({nodeAt(0U)}, messages, reportedNow(messages)));

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("2 active fault(s) on 1")))
        << bench.summary().toStdString();

    EXPECT_EQ(bench.groupOf(0U).toStdString(), "active faults");

    const QTreeWidgetItem* group = bench.ecu(0U)->child(0);
    EXPECT_EQ(group->text(2).toStdString(), "2");
    EXPECT_EQ(group->childCount(), 2);

    // Nothing to say about the past: there is none.
    EXPECT_FALSE(bench.summary().contains(QStringLiteral("earlier")));
    EXPECT_TRUE(group->toolTip(0).isEmpty());
}

TEST(J1939NetworkPanelTests, AnEcuThatHasGoneQuietLeavesItsLastDm1OnScreenAndCountsNothing)
{
    Bench bench;

    // The DM1 is still what the ECU last said, and the block says nothing is being reported: the
    // ECU stopped repeating it.
    const std::vector<J1939Diagnostic> messages{dm1From(0U, {code(100U, 1U), code(110U, 0U)})};
    ASSERT_TRUE(bench.publish({nodeAt(0U)}, messages, {}));

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("no active faults")))
        << bench.summary().toStdString();

    // Said as what it is: the last thing the ECU said, kept because that is what the panel is for.
    EXPECT_EQ(bench.groupOf(0U).toStdString(), "last reported");

    const QTreeWidgetItem* group = bench.ecu(0U)->child(0);
    EXPECT_TRUE(group->toolTip(0).contains(QStringLiteral("not being reported now")))
        << group->toolTip(0).toStdString();

    // Its faults are still there to read - that is why it is kept - and none of them is counted as
    // active: the summary says what it would say of a healthy bus, and that two were reported
    // before.
    EXPECT_EQ(group->childCount(), 2);
    EXPECT_EQ(group->text(2).toStdString(), "2");
    EXPECT_TRUE(bench.summary().contains(QStringLiteral("2 reported earlier")))
        << bench.summary().toStdString();
    EXPECT_FALSE(bench.summary().contains(QStringLiteral("active fault(s)")));
}

TEST(J1939NetworkPanelTests, AMeasurementThatStopsTakesTheWordActiveOffTheFaultsItLeavesBehind)
{
    Bench bench;

    const std::vector<J1939Diagnostic> messages{dm1From(0U, {code(100U, 1U)})};

    // Running: the engine reports a fault.
    ASSERT_TRUE(bench.publish({nodeAt(0U)}, messages, reportedNow(messages)));
    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("1 active fault(s) on 1")));
    EXPECT_EQ(bench.groupOf(0U).toStdString(), "active faults");

    // Stopped: the block publishes the same view of the bus with an empty list of what is being
    // reported. The panel follows, and keeps the fault where it was.
    bench.network.publish({nodeAt(0U)}, {}, messages, {});

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("no active faults")))
        << bench.summary().toStdString();
    EXPECT_EQ(bench.groupOf(0U).toStdString(), "last reported");
    EXPECT_EQ(bench.ecu(0U)->child(0)->childCount(), 1);
    EXPECT_TRUE(bench.summary().contains(QStringLiteral("1 reported earlier")));
}

TEST(J1939NetworkPanelTests, EachEcuIsLabelledByItsOwnDm1AndOnlyWhatIsReportedNowIsCounted)
{
    Bench bench;

    // The engine has gone quiet with two faults on its last DM1; the brakes are reporting one.
    const std::vector<J1939Diagnostic> messages{dm1From(0U, {code(100U, 1U), code(110U, 0U)}),
                                                dm1From(11U, {code(1043U, 12U)})};
    const std::vector<J1939Diagnostic> brakes{messages[1]};

    ASSERT_TRUE(bench.publish({nodeAt(0U), nodeAt(11U)}, messages, reportedNow(brakes)));

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("1 active fault(s) on 1")))
        << bench.summary().toStdString();

    EXPECT_EQ(bench.groupOf(0U).toStdString(), "last reported");
    EXPECT_EQ(bench.groupOf(11U).toStdString(), "active faults");
    EXPECT_TRUE(bench.summary().contains(QStringLiteral("2 reported earlier")))
        << bench.summary().toStdString();
}

TEST(J1939NetworkPanelTests, AHealthyEcuStillSaysNoneAndAnEarlierListIsStillSaidToBeEarlier)
{
    Bench bench;

    // Unchanged by any of this: an ECU that says it is fine says so, and a DM2 is what *was* wrong.
    const std::vector<J1939Diagnostic> messages{dm1From(0U, {}), dm2From(0U, {code(100U, 1U)})};
    ASSERT_TRUE(bench.publish({nodeAt(0U)}, messages, reportedNow(messages)));

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("no active faults")))
        << bench.summary().toStdString();

    EXPECT_EQ(bench.groupOf(0U, 0).toStdString(), "active faults");
    EXPECT_EQ(bench.ecu(0U)->child(0)->text(2).toStdString(), "none");

    EXPECT_EQ(bench.groupOf(0U, 1).toStdString(), "previously active");

    // A previous list is not a fault being reported, and not one reported earlier by a DM1 either.
    EXPECT_FALSE(bench.summary().contains(QStringLiteral("earlier")));
}

TEST(J1939NetworkPanelTests, BeforeAnythingIsPublishedItSaysSoAndNothingAboutFaults)
{
    Bench bench;

    EXPECT_TRUE(bench.summary().contains(QStringLiteral("Nothing seen yet")))
        << bench.summary().toStdString();
    EXPECT_FALSE(bench.summary().contains(QStringLiteral("fault")));
}

TEST(J1939NetworkPanelTests, AHealthyEcuThatHasGoneQuietIsNotSaidToBeSayingItHasNoFaults)
{
    Bench bench;

    // Both said that nothing was wrong, and only the brakes have said it since. The engine was
    // healthy when last heard, and that is all the panel knows of it.
    const std::vector<J1939Diagnostic> messages{dm1From(0U, {}), dm1From(11U, {})};
    const std::vector<J1939Diagnostic> brakes{messages[1]};

    ASSERT_TRUE(bench.publish({nodeAt(0U), nodeAt(11U)}, messages, reportedNow(brakes)));
    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("no active faults")))
        << bench.summary().toStdString();

    // The one that goes on saying it is a diagnosis, as before.
    EXPECT_EQ(bench.groupOf(11U).toStdString(), "active faults");

    const QTreeWidgetItem* speaking = bench.ecu(11U)->child(0);
    EXPECT_EQ(speaking->text(2).toStdString(), "none");
    EXPECT_TRUE(speaking->toolTip(0).isEmpty());

    // The other is the last thing its ECU said - kept, because that is what the panel is for - and
    // not in the colour of an ECU that is saying it now.
    EXPECT_EQ(bench.groupOf(0U).toStdString(), "last reported");

    const QTreeWidgetItem* quiet = bench.ecu(0U)->child(0);
    EXPECT_EQ(quiet->text(2).toStdString(), "none");
    EXPECT_NE(quiet->foreground(2).color().name().toStdString(),
              speaking->foreground(2).color().name().toStdString());

    // Said in words: what it said is not something it is saying now.
    EXPECT_TRUE(quiet->toolTip(0).contains(QStringLiteral("no faults")))
        << quiet->toolTip(0).toStdString();
    EXPECT_TRUE(quiet->toolTip(0).contains(QStringLiteral("not sent another")))
        << quiet->toolTip(0).toStdString();

    // Nothing to count, and nothing "reported earlier" either: there were no faults to report.
    EXPECT_FALSE(bench.summary().contains(QStringLiteral("earlier")));
}

TEST(J1939NetworkPanelTests, AMeasurementThatStopsMakesAHealthyEcusNoneTheLastThingItSaid)
{
    Bench bench;

    const std::vector<J1939Diagnostic> messages{dm1From(0U, {})};

    ASSERT_TRUE(bench.publish({nodeAt(0U)}, messages, reportedNow(messages)));
    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("no active faults")));
    EXPECT_EQ(bench.groupOf(0U).toStdString(), "active faults");

    // Stopped: the same view of the bus, and no ECU repeating anything. The list of codes was empty
    // before and is now - it is the other list that says this.
    bench.network.publish({nodeAt(0U)}, {}, messages, {}, {});

    ASSERT_TRUE(waitFor([&bench] { return bench.groupOf(0U) == QStringLiteral("last reported"); }));
    EXPECT_EQ(bench.ecu(0U)->child(0)->text(2).toStdString(), "none");
}

TEST(J1939NetworkPanelTests, TheJ1939ExampleIsReportingWhileItRunsAndLastReportedOnceItIsStopped)
{
    // All the way: the example's own graph on the engine and the engine's own network on the panel.
    // The ECUs of the example repeat a DM1 a second, every one of them saying that nothing is
    // wrong, and "active faults: none" is true of them only while they are saying it.
    const QString path = QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/j1939-vehicle.tbsproj");

    GraphDescription pipeline;
    TransmitList transmit;
    DashboardDescription dashboard;

    const Result opened = services::ProjectFile::load(path, pipeline, transmit, dashboard);
    ASSERT_TRUE(opened.succeeded()) << std::string{opened.message()};

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    engine.setGraphDescription(
        pipeline, NodeCatalog::withBuiltinTypes(), QFileInfo{path}.absolutePath().toStdString());
    ASSERT_TRUE(engine.start().succeeded());

    J1939NetworkPanel panel;
    panel.setNetwork(&engine.j1939Network());
    panel.resize(900, 400);
    panel.show();

    const QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);

    // How many ECUs have a DM1 under their row that the panel calls `label`.
    const auto groups = [tree](const QString& label) {
        int count = 0;

        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            const QTreeWidgetItem* ecu = tree->topLevelItem(row);

            for (int child = 0; child < ecu->childCount(); ++child) {
                count += ecu->child(child)->text(0) == label ? 1 : 0;
            }
        }

        return count;
    };

    const QString active = QStringLiteral("active faults");
    const QString last = QStringLiteral("last reported");

    EXPECT_TRUE(waitFor([&] { return groups(active) >= 5 && groups(last) == 0; }, 8000))
        << groups(active) << " reporting, " << groups(last) << " last reported";

    // The measurement stops: nobody is repeating anything, and what each ECU said is all that is
    // left, and is said to be that.
    engine.stop();

    EXPECT_TRUE(waitFor([&] { return groups(last) >= 5 && groups(active) == 0; }))
        << groups(active) << " reporting, " << groups(last) << " last reported";
}

TEST(J1939NetworkPanelTests, AnEcuOfTheJ1939ExampleAskedForWhatItDoesNotHaveSaysSoOnItsRow)
{
    // The whole path, as a person meets it: the example's own graph on the engine, a request for a
    // group the engine ECU does not send - from the tool, which has no address - and the refusal
    // J1939-21 has an ECU send, read by the example's J1939 block and shown on the ECU's row.
    const QString path = QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/j1939-vehicle.tbsproj");

    GraphDescription pipeline;
    TransmitList transmit;
    DashboardDescription dashboard;

    const Result opened = services::ProjectFile::load(path, pipeline, transmit, dashboard);
    ASSERT_TRUE(opened.succeeded()) << std::string{opened.message()};

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    engine.setGraphDescription(
        pipeline, NodeCatalog::withBuiltinTypes(), QFileInfo{path}.absolutePath().toStdString());
    ASSERT_TRUE(engine.start().succeeded());

    J1939NetworkPanel panel;
    panel.setNetwork(&engine.j1939Network());
    panel.resize(900, 400);
    panel.show();

    const QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);

    // The row of the ECU at `address`, and what its detail says.
    const auto detailOf = [tree](int address) {
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            const QTreeWidgetItem* ecu = tree->topLevelItem(row);

            if (ecu->text(0).startsWith(QString::number(address) + QLatin1Char(' '))) {
                return ecu->text(4);
            }
        }

        return QString{};
    };

    // Everyone has to be on the panel before anybody is asked - 0 is the engine, 0x21 the body.
    ASSERT_TRUE(waitFor([&] { return !detailOf(0).isEmpty() && !detailOf(0x21).isEmpty(); }, 8000));

    // The engine ECU is asked for its previously active faults, and has never sent any.
    const J1939RequestOutcome outcome =
        j1939RequestPgn(engine, kPgnDm2, 0U, static_cast<std::uint8_t>(0U));
    EXPECT_EQ(outcome.sent, (std::vector<std::uint8_t>{0U}));
    EXPECT_TRUE(outcome.refused.empty());

    const QString refusal = QStringLiteral("last answer: PGN 65227 (0xFECB) not supported");

    EXPECT_TRUE(waitFor([&] { return detailOf(0).contains(refusal); }, 8000))
        << detailOf(0).toStdString();

    // Only the one that was asked has it: the body controller heard the same wire and was not
    // addressed.
    EXPECT_FALSE(detailOf(0x21).contains(QStringLiteral("last answer")))
        << detailOf(0x21).toStdString();

    engine.stop();
}

namespace {

/// Two buses as two J1939 blocks hand them over: on CAN 1 the engine at address 0, reporting a
/// fault; on CAN 2 another ECU at address 0 - another ECU, at the same address - and one at 33,
/// both healthy.
void publishTwoBuses(J1939Network& network, std::vector<J1939NetworkNode> body = {})
{
    const std::vector<J1939Diagnostic> powertrain{dm1From(0U, {code(100U, 1U)})};
    const std::vector<J1939Diagnostic> bodyDm1{dm1From(0U, {}), dm1From(33U, {})};

    if (body.empty()) {
        body = {nodeAt(0U), nodeAt(33U)};
    }

    network.publish({nodeAt(0U)},
                    {},
                    powertrain,
                    j1939ActiveFaults(powertrain, 0U),
                    j1939Reporting(powertrain, 0U),
                    0U);
    network.publish(std::move(body), {}, bodyDm1, {}, j1939Reporting(bodyDm1, 0U), 1U);
}

} // namespace

TEST(J1939NetworkPanelTests, WithTwoBusesEachEcuIsListedUnderTheBusItIsOn)
{
    Bench bench;
    publishTwoBuses(bench.network);

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("on 2 buses")))
        << bench.summary().toStdString();

    // One heading per bus, in channel order, open, and the ECUs under the bus they are on.
    QTreeWidget* tree = bench.tree();
    ASSERT_EQ(tree->topLevelItemCount(), 2);

    const QTreeWidgetItem* can1 = tree->topLevelItem(0);
    const QTreeWidgetItem* can2 = tree->topLevelItem(1);
    EXPECT_EQ(can1->text(0).toStdString(), "CAN 1");
    EXPECT_EQ(can2->text(0).toStdString(), "CAN 2");
    EXPECT_TRUE(can1->isExpanded());
    EXPECT_TRUE(can2->isExpanded());

    ASSERT_EQ(can1->childCount(), 1);
    ASSERT_EQ(can2->childCount(), 2);
    EXPECT_TRUE(can1->child(0)->text(0).startsWith(QStringLiteral("0 ")));
    EXPECT_TRUE(can2->child(0)->text(0).startsWith(QStringLiteral("0 ")));
    EXPECT_TRUE(can2->child(1)->text(0).startsWith(QStringLiteral("33 ")));

    // What each bus is reporting, on its heading.
    EXPECT_EQ(can1->text(1).toStdString(), "1 ECU(s)");
    EXPECT_EQ(can1->text(2).toStdString(), "1 active fault(s)");
    EXPECT_EQ(can2->text(1).toStdString(), "2 ECU(s)");
    EXPECT_EQ(can2->text(2).toStdString(), "no active faults");

    // Each DM1 is what its own bus says of its ECU: address 0 is reporting a fault on CAN 1, and
    // address 0 on CAN 2 says it is fine.
    ASSERT_EQ(can1->child(0)->childCount(), 1);
    EXPECT_EQ(can1->child(0)->child(0)->text(0).toStdString(), "active faults");
    EXPECT_EQ(can1->child(0)->child(0)->text(2).toStdString(), "1");
    ASSERT_EQ(can2->child(0)->childCount(), 1);
    EXPECT_EQ(can2->child(0)->child(0)->text(0).toStdString(), "active faults");
    EXPECT_EQ(can2->child(0)->child(0)->text(2).toStdString(), "none");

    // And the summary counts both.
    EXPECT_TRUE(bench.summary().startsWith(QStringLiteral("3 ECU(s) on 2 buses")))
        << bench.summary().toStdString();
    EXPECT_TRUE(bench.summary().contains(QStringLiteral("1 active fault(s) on 1")))
        << bench.summary().toStdString();
}

TEST(J1939NetworkPanelTests, WhatWasOpenedAndClosedUnderTwoBusesStaysSoWhenOneOfThemChanges)
{
    Bench bench;
    publishTwoBuses(bench.network);
    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("on 2 buses")));

    QTreeWidget* tree = bench.tree();
    ASSERT_EQ(tree->topLevelItemCount(), 2);

    // CAN 1 closed by hand, and on CAN 2 the healthy ECU at 33 opened by hand.
    tree->topLevelItem(0)->setExpanded(false);
    ASSERT_EQ(tree->topLevelItem(1)->childCount(), 2);
    tree->topLevelItem(1)->child(1)->setExpanded(true);

    // CAN 2 changes - an ECU is heard at 40 - and the tree is built again.
    publishTwoBuses(bench.network, {nodeAt(0U), nodeAt(33U), nodeAt(40U)});
    ASSERT_TRUE(waitFor([tree] {
        return tree->topLevelItemCount() == 2 && tree->topLevelItem(1)->childCount() == 3;
    }));

    EXPECT_FALSE(tree->topLevelItem(0)->isExpanded());
    EXPECT_TRUE(tree->topLevelItem(1)->isExpanded());
    EXPECT_TRUE(tree->topLevelItem(1)->child(1)->isExpanded());
    EXPECT_FALSE(tree->topLevelItem(1)->child(2)->isExpanded());
}

TEST(J1939NetworkPanelTests, WithOneBusThereIsNoHeadingAndTheEcusAreAtTheTopAsBefore)
{
    // The usual case is shown as it always was.
    Bench bench;

    const std::vector<J1939Diagnostic> messages{dm1From(0U, {})};
    bench.network.publish(
        {nodeAt(0U)}, {}, messages, {}, j1939Reporting(messages, 0U), 1U); // on CAN 2, alone

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("1 ECU(s)"))) << bench.summary().toStdString();

    EXPECT_FALSE(bench.summary().contains(QStringLiteral("buses")));
    ASSERT_EQ(bench.tree()->topLevelItemCount(), 1);
    EXPECT_TRUE(bench.tree()->topLevelItem(0)->text(0).startsWith(QStringLiteral("0 ")));
}

TEST(J1939NetworkPanelTests, RequestClaimsCanBePressedOnlyWhileAMeasurementRuns)
{
    Bench bench;

    int asked = 0;
    QObject::connect(&bench.panel, &J1939NetworkPanel::claimsRequested, [&asked] { ++asked; });

    auto* button = bench.panel.findChild<QPushButton*>(QStringLiteral("requestClaimsButton"));
    ASSERT_NE(button, nullptr);

    // Off until whoever owns the engine says a measurement runs: before it every channel refuses.
    EXPECT_FALSE(button->isEnabled());
    button->click();
    EXPECT_EQ(asked, 0);
    EXPECT_TRUE(
        button->toolTip().contains(QStringLiteral("Available while a measurement is running")));

    bench.panel.setRequestAvailable(true);
    EXPECT_TRUE(button->isEnabled());
    EXPECT_FALSE(button->toolTip().contains(QStringLiteral("Available while")));

    button->click();
    EXPECT_EQ(asked, 1);

    // Once per press, and not again by itself.
    QApplication::processEvents();
    EXPECT_EQ(asked, 1);

    bench.panel.setRequestAvailable(false);
    EXPECT_FALSE(button->isEnabled());
    button->click();
    EXPECT_EQ(asked, 1);
}

TEST(J1939NetworkPanelTests, TheButtonSaysWhatItSendsAndWhenToUseIt)
{
    Bench bench;

    const auto* button = bench.panel.findChild<QPushButton*>(QStringLiteral("requestClaimsButton"));
    ASSERT_NE(button, nullptr);

    EXPECT_EQ(button->text().toStdString(), "Request claims");

    // What it puts on the bus, from where, and the situation it is for: a person pressing a button
    // that transmits has to be able to find out what it transmits.
    const QString tip = button->toolTip();
    EXPECT_TRUE(tip.contains(QStringLiteral("Request for Address Claimed")));
    EXPECT_TRUE(tip.contains(QStringLiteral("null address")));
    EXPECT_TRUE(tip.contains(QStringLiteral("already running")));
}

namespace {

/// What a panel was asked for: the group, of whom, and on which channel.
struct Asked final {
    std::uint32_t pgn{0};
    int destination{0};
    int channel{0};

    [[nodiscard]] friend bool operator==(const Asked&, const Asked&) = default;

    friend void PrintTo(const Asked& asked, std::ostream* out)
    {
        *out << "{PGN " << asked.pgn << ", to " << asked.destination << ", on " << asked.channel
             << "}";
    }
};

/// Collects what a panel asks for, in the order it asks.
struct Listener final {
    std::vector<Asked> asked;

    explicit Listener(const J1939NetworkPanel& panel)
    {
        QObject::connect(&panel,
                         &J1939NetworkPanel::pgnRequested,
                         [this](quint32 pgn, int destination, int channel) {
                             asked.push_back({pgn, destination, channel});
                         });
    }
};

/// Runs `act` on the request dialog when it opens, from inside the event loop it runs - the way a
/// hand would. A dialog that `act` leaves open is rejected, and so is one that opens too late for
/// `act` to find it, so that a failure fails and does not hang.
class DialogDriver final {
public:
    explicit DialogDriver(std::function<void(J1939RequestDialog&)> act)
        : m_act{std::move(act)}
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { look(); });
        m_timer.start();
    }

    [[nodiscard]] int opened() const { return m_opened; }

private:
    void look()
    {
        QWidget* modal = QApplication::activeModalWidget();
        auto* dialog = qobject_cast<J1939RequestDialog*>(modal);

        if (dialog == nullptr) {
            // Something else is holding the loop: a modal that is not the one to answer is closed
            // after a few seconds.
            if (modal != nullptr && ++m_waited > 400) {
                modal->close();
            }

            return;
        }

        if (dialog == m_last) {
            return;
        }

        m_last = dialog;
        ++m_opened;
        m_act(*dialog);

        if (dialog->isVisible()) {
            dialog->reject();
        }
    }

    std::function<void(J1939RequestDialog&)> m_act;
    QTimer m_timer;
    const J1939RequestDialog* m_last{nullptr};
    int m_opened{0};
    int m_waited{0};
};

/// What can be chosen in a menu of the panel: its entries, and neither the line that says whom
/// nor the lines between them.
[[nodiscard]] QList<QAction*> choicesOf(const QMenu& menu)
{
    QList<QAction*> choices;

    for (QAction* action : menu.actions()) {
        if (!action->isSeparator() && action->objectName() != QLatin1String{"requestMenuHeading"}) {
            choices.push_back(action);
        }
    }

    return choices;
}

/// Finds the menu a right click opens - it runs an event loop of its own - reads its title,
/// triggers the action at `choice` (none when it is -1) and closes it. A menu left open is closed
/// after a few seconds, so that one that is never answered fails and does not hang.
class MenuDriver final {
public:
    explicit MenuDriver(int choice)
        : m_choice{choice}
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { look(); });
        m_timer.start();
    }

    [[nodiscard]] int opened() const { return m_opened; }
    [[nodiscard]] const QString& title() const { return m_title; }

private:
    void look()
    {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());

        if (menu == nullptr || menu == m_last) {
            return;
        }

        m_last = menu;
        ++m_opened;
        m_title = menu->title();

        const QList<QAction*> choices = choicesOf(*menu);
        if (m_choice >= 0 && m_choice < choices.size()) {
            choices[m_choice]->trigger();
        }

        menu->close();
    }

    int m_choice;
    QTimer m_timer;
    const QMenu* m_last{nullptr};
    int m_opened{0};
    QString m_title;
};

/// A right click on `item`, or on the empty part of the tree when it is null.
void rightClick(const Bench& bench, const QTreeWidgetItem* item)
{
    QTreeWidget* tree = bench.tree();

    const QPoint where = item != nullptr ? tree->visualItemRect(item).center()
                                         : QPoint{20, tree->viewport()->height() - 5};

    Q_EMIT tree->customContextMenuRequested(where);
}

/// A bench with two buses on it - at 0 and at 33 - and requests turned on.
struct TwoBusBench final {
    Bench bench;
    Listener listener{bench.panel};

    TwoBusBench()
    {
        publishTwoBuses(bench.network);
        EXPECT_TRUE(bench.waitForSummary(QStringLiteral("on 2 buses")));
        bench.panel.setRequestAvailable(true);
    }

    /// The row of the ECU at `address` on the bus at `bus` (0 or 1), or null.
    [[nodiscard]] QTreeWidgetItem* ecuOn(int bus, std::uint8_t address) const
    {
        QTreeWidgetItem* heading = bench.tree()->topLevelItem(bus);

        for (int child = 0; heading != nullptr && child < heading->childCount(); ++child) {
            if (heading->child(child)->text(0).startsWith(QString::number(address)
                                                          + QLatin1Char(' '))) {
                return heading->child(child);
            }
        }

        return nullptr;
    }
};

} // namespace

TEST(J1939NetworkPanelTests, RequestPgnIsOffUntilAMeasurementRunsAndSaysWhatItIsFor)
{
    Bench bench;
    Listener listener{bench.panel};

    auto* button = bench.panel.findChild<QPushButton*>(QStringLiteral("requestPgnButton"));
    ASSERT_NE(button, nullptr);

    EXPECT_EQ(button->text().toStdString(), "Request PGN...");

    // Off until whoever owns the engine says a measurement runs - and a click on it opens nothing.
    EXPECT_FALSE(button->isEnabled());
    EXPECT_TRUE(
        button->toolTip().contains(QStringLiteral("Available while a measurement is running")));

    {
        DialogDriver driver{[](J1939RequestDialog& dialog) { dialog.accept(); }};
        button->click();
        QApplication::processEvents();

        EXPECT_EQ(driver.opened(), 0);
        EXPECT_TRUE(listener.asked.empty());
    }

    bench.panel.setRequestAvailable(true);
    EXPECT_TRUE(button->isEnabled());
    EXPECT_FALSE(button->toolTip().contains(QStringLiteral("Available while")));

    // What it is for, and where else the same is: a person pressing a button that transmits has to
    // be able to find out what it transmits.
    const QString tip = button->toolTip();
    EXPECT_TRUE(tip.contains(QStringLiteral("parameter group")));
    EXPECT_TRUE(tip.contains(QStringLiteral("DM2")));
    EXPECT_TRUE(tip.contains(QStringLiteral("right click")));
    EXPECT_TRUE(tip.contains(QStringLiteral("last answer")));
    EXPECT_TRUE(tip.contains(QStringLiteral("null address, 254")));

    // Off again with the measurement, and both buttons go together.
    bench.panel.setRequestAvailable(false);
    EXPECT_FALSE(button->isEnabled());
    EXPECT_FALSE(
        bench.panel.findChild<QPushButton*>(QStringLiteral("requestClaimsButton"))->isEnabled());
    EXPECT_TRUE(
        button->toolTip().contains(QStringLiteral("Available while a measurement is running")));
}

TEST(J1939NetworkPanelTests, RequestPgnOpensTheDialogOnEveryEcuAndEveryBusAndAsksWhatWasChosen)
{
    TwoBusBench two;

    auto* button = two.bench.panel.findChild<QPushButton*>(QStringLiteral("requestPgnButton"));
    ASSERT_NE(button, nullptr);

    int ecus = 0;
    int buses = 0;
    int destination = -9;
    int channel = -9;

    DialogDriver driver{[&](J1939RequestDialog& dialog) {
        ecus = dialog.findChild<QComboBox*>(QStringLiteral("requestDestination"))->count();
        buses = dialog.findChild<QComboBox*>(QStringLiteral("requestChannel"))->count();
        destination = dialog.destination();
        channel = dialog.channel();

        dialog.setPgnText(QStringLiteral("0xFECB"));
        dialog.accept();
    }};

    button->click();

    ASSERT_EQ(driver.opened(), 1);

    // Every ECU and every channel to start with - the button is the question for the whole bus -
    // and what was heard to choose from: the ECUs at 0 and at 33, once each though 0 is on both
    // buses, and the two buses.
    EXPECT_EQ(destination, 255);
    EXPECT_EQ(channel, -1);
    EXPECT_EQ(ecus, 3);
    EXPECT_EQ(buses, 3);

    EXPECT_EQ(two.listener.asked, (std::vector<Asked>{{65227U, 255, -1}}));
}

TEST(J1939NetworkPanelTests, WhatTheDialogIsAcceptedWithIsWhatIsAsked)
{
    TwoBusBench two;
    auto* button = two.bench.panel.findChild<QPushButton*>(QStringLiteral("requestPgnButton"));
    ASSERT_NE(button, nullptr);

    DialogDriver driver{[](J1939RequestDialog& dialog) {
        dialog.setPgnText(QStringLiteral("65226")); // DM1
        dialog.setDestination(33);
        dialog.setChannel(1);
        dialog.accept();
    }};

    button->click();

    ASSERT_EQ(driver.opened(), 1);
    EXPECT_EQ(two.listener.asked, (std::vector<Asked>{{65226U, 33, 1}}));
}

TEST(J1939NetworkPanelTests, ADialogThatIsCancelledAsksNothing)
{
    TwoBusBench two;
    auto* button = two.bench.panel.findChild<QPushButton*>(QStringLiteral("requestPgnButton"));
    ASSERT_NE(button, nullptr);

    DialogDriver driver{[](J1939RequestDialog& dialog) {
        dialog.setPgnText(QStringLiteral("0xFECB"));
        dialog.reject();
    }};

    button->click();

    EXPECT_EQ(driver.opened(), 1);
    EXPECT_TRUE(two.listener.asked.empty());
}

TEST(J1939NetworkPanelTests, TheMenuOfAnEcuAsksThatEcuOnItsBusForTheGroupsWorthAskingFor)
{
    TwoBusBench two;

    const std::unique_ptr<QMenu> menu{two.bench.panel.requestMenu(1, 33)};

    EXPECT_EQ(menu->title().toStdString(), "Ask the ECU at 33  (0x21)");

    // A line that says whom - a popup does not show its title - and cannot be chosen, then one
    // entry for each group on the list, in the order of the list, a line, and the dialog for the
    // groups that are not.
    const QList<QAction*> all = menu->actions();
    ASSERT_GE(all.size(), 2);
    EXPECT_EQ(all[0]->text().toStdString(), "Ask the ECU at 33  (0x21)");
    EXPECT_FALSE(all[0]->isEnabled());
    EXPECT_TRUE(all[1]->isSeparator());

    const std::vector<J1939RequestPreset>& presets = J1939RequestDialog::presets();
    const QList<QAction*> actions = choicesOf(*menu);
    ASSERT_EQ(static_cast<std::size_t>(actions.size()), presets.size() + 1U);

    for (std::size_t index = 0; index < presets.size(); ++index) {
        EXPECT_EQ(actions[static_cast<qsizetype>(index)]->text().toStdString(),
                  presets[index].label.toStdString());
        EXPECT_TRUE(actions[static_cast<qsizetype>(index)]->isEnabled());
    }

    EXPECT_EQ(actions.back()->text().toStdString(), "Another group...");
    EXPECT_TRUE(actions.back()->isEnabled());
    EXPECT_TRUE(all[all.size() - 2]->isSeparator()); // the line before the dialog's entry

    // Each asks its own group, of that ECU, on that bus - CAN 2 is channel 1.
    for (std::size_t index = 0; index < presets.size(); ++index) {
        actions[static_cast<qsizetype>(index)]->trigger();
    }

    ASSERT_EQ(two.listener.asked.size(), presets.size());

    for (std::size_t index = 0; index < presets.size(); ++index) {
        EXPECT_EQ(two.listener.asked[index], (Asked{presets[index].pgn, 33, 1})) << index;
    }
}

TEST(J1939NetworkPanelTests, TheMenuIsOffWhileNoMeasurementRuns)
{
    Bench bench;
    Listener listener{bench.panel};

    const std::unique_ptr<QMenu> menu{bench.panel.requestMenu(0, 33)};

    // Every entry is there to read and none can be chosen: before a measurement every channel
    // would refuse. The line between them is not an entry.
    EXPECT_GT(choicesOf(*menu).size(), 1);

    for (const QAction* action : choicesOf(*menu)) {
        EXPECT_FALSE(action->isEnabled()) << action->text().toStdString();
    }

    bench.panel.setRequestAvailable(true);
    const std::unique_ptr<QMenu> later{bench.panel.requestMenu(0, 33)};

    for (const QAction* action : choicesOf(*later)) {
        EXPECT_TRUE(action->isEnabled()) << action->text().toStdString();
    }

    EXPECT_TRUE(listener.asked.empty());
}

TEST(J1939NetworkPanelTests, TheMenuOfABusAsksEveryEcuOnThatBus)
{
    TwoBusBench two;

    const std::unique_ptr<QMenu> menu{two.bench.panel.requestMenu(1, 255)};
    EXPECT_EQ(menu->title().toStdString(), "Ask every ECU on CAN 2");
    EXPECT_EQ(menu->actions().front()->text().toStdString(), "Ask every ECU on CAN 2");

    choicesOf(*menu).front()->trigger();
    EXPECT_EQ(two.listener.asked, (std::vector<Asked>{{kPgnAddressClaimed, 255, 1}}));

    // The first bus is a bus, and not "every bus": channel 0 is a channel.
    const std::unique_ptr<QMenu> first{two.bench.panel.requestMenu(0, 255)};
    EXPECT_EQ(first->title().toStdString(), "Ask every ECU on CAN 1");

    // And on every bus, for a menu that is not of one.
    const std::unique_ptr<QMenu> all{two.bench.panel.requestMenu(-1, 255)};
    EXPECT_EQ(all->title().toStdString(), "Ask every ECU on every bus");

    choicesOf(*all).front()->trigger();
    EXPECT_EQ(two.listener.asked.back(), (Asked{kPgnAddressClaimed, 255, -1}));
}

TEST(J1939NetworkPanelTests, AnotherGroupFromTheMenuOpensTheDialogOnTheEcuAndTheBusOfTheRow)
{
    TwoBusBench two;

    int destination = -9;
    int channel = -9;

    DialogDriver driver{[&](J1939RequestDialog& dialog) {
        destination = dialog.destination();
        channel = dialog.channel();
        dialog.accept(); // the claim, which it opens on
    }};

    const std::unique_ptr<QMenu> menu{two.bench.panel.requestMenu(1, 33)};
    menu->actions().back()->trigger();

    ASSERT_EQ(driver.opened(), 1);
    EXPECT_EQ(destination, 33);
    EXPECT_EQ(channel, 1);
    EXPECT_EQ(two.listener.asked, (std::vector<Asked>{{kPgnAddressClaimed, 33, 1}}));
}

TEST(J1939NetworkPanelTests, AnotherGroupFromTheMenuOfABusLeavesTheEcuToChoose)
{
    TwoBusBench two;

    int destination = -9;
    int channel = -9;

    DialogDriver driver{[&](J1939RequestDialog& dialog) {
        destination = dialog.destination();
        channel = dialog.channel();
        dialog.reject();
    }};

    const std::unique_ptr<QMenu> menu{two.bench.panel.requestMenu(0, 255)};
    menu->actions().back()->trigger();

    ASSERT_EQ(driver.opened(), 1);
    EXPECT_EQ(destination, 255);
    EXPECT_EQ(channel, 0);
    EXPECT_TRUE(two.listener.asked.empty());
}

TEST(J1939NetworkPanelTests, ARightClickOnARowOpensTheMenuOfTheEcuOrTheBusItBelongsTo)
{
    TwoBusBench two;

    // The ECU at 33, on CAN 2.
    {
        MenuDriver driver{0};
        rightClick(two.bench, two.ecuOn(1, 33U));

        EXPECT_EQ(driver.opened(), 1);
        EXPECT_EQ(driver.title().toStdString(), "Ask the ECU at 33  (0x21)");
    }

    // The ECU at 0 on CAN 1 and the one at 0 on CAN 2: the same number, and another ECU on another
    // bus, so another channel.
    {
        MenuDriver driver{0};
        rightClick(two.bench, two.ecuOn(0, 0U));
        EXPECT_EQ(driver.title().toStdString(), "Ask the ECU at 0  (0x00)");
    }

    {
        MenuDriver driver{0};
        rightClick(two.bench, two.ecuOn(1, 0U));
        EXPECT_EQ(driver.opened(), 1);
    }

    // The heading of CAN 2: everyone on it.
    {
        MenuDriver driver{0};
        rightClick(two.bench, two.bench.tree()->topLevelItem(1));

        EXPECT_EQ(driver.opened(), 1);
        EXPECT_EQ(driver.title().toStdString(), "Ask every ECU on CAN 2");
    }

    // A fault under an ECU is asked about by asking its ECU: the DM1 of the engine on CAN 1.
    {
        QTreeWidgetItem* engine = two.ecuOn(0, 0U);
        ASSERT_NE(engine, nullptr);
        ASSERT_GT(engine->childCount(), 0);
        engine->setExpanded(true);

        MenuDriver driver{0};
        rightClick(two.bench, engine->child(0));

        EXPECT_EQ(driver.opened(), 1);
        EXPECT_EQ(driver.title().toStdString(), "Ask the ECU at 0  (0x00)");
    }

    // And the fault itself, a row further down: the group it is in is no more an ECU than it is.
    {
        QTreeWidgetItem* engine = two.ecuOn(0, 0U);
        ASSERT_NE(engine, nullptr);
        QTreeWidgetItem* group = engine->child(0);
        ASSERT_NE(group, nullptr);
        ASSERT_GT(group->childCount(), 0);
        engine->setExpanded(true);
        group->setExpanded(true);

        MenuDriver driver{0};
        rightClick(two.bench, group->child(0));

        EXPECT_EQ(driver.opened(), 1);
        EXPECT_EQ(driver.title().toStdString(), "Ask the ECU at 0  (0x00)");
    }

    // What each of them asked: the first group on the list, of the ECU or the bus clicked, on the
    // channel of the bus it is under.
    EXPECT_EQ(two.listener.asked,
              (std::vector<Asked>{{kPgnAddressClaimed, 33, 1},
                                  {kPgnAddressClaimed, 0, 0},
                                  {kPgnAddressClaimed, 0, 1},
                                  {kPgnAddressClaimed, 255, 1},
                                  {kPgnAddressClaimed, 0, 0},
                                  {kPgnAddressClaimed, 0, 0}}));
}

TEST(J1939NetworkPanelTests, ARightClickOnAFaultAsksTheEcuThatReportsItOnItsOwnBus)
{
    Bench bench;
    Listener listener{bench.panel};

    // The ECU at 33 on CAN 2 reports a fault: its row, the group its faults are in, and the fault.
    // Not the ECU at 0 on CAN 1, which is what a click that lost its way would come back with.
    const std::vector<J1939Diagnostic> messages{dm1From(33U, {code(100U, 1U)})};
    bench.network.publish({nodeAt(33U)},
                          {},
                          messages,
                          j1939ActiveFaults(messages, 1U),
                          j1939Reporting(messages, 1U),
                          1U);

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("1 active fault(s)")))
        << bench.summary().toStdString();
    bench.panel.setRequestAvailable(true);

    QTreeWidgetItem* ecu = bench.ecu(33U);
    ASSERT_NE(ecu, nullptr);
    ASSERT_EQ(ecu->childCount(), 1);

    QTreeWidgetItem* group = ecu->child(0);
    ASSERT_EQ(group->childCount(), 1);
    ecu->setExpanded(true);
    group->setExpanded(true);

    MenuDriver driver{0};
    rightClick(bench, group->child(0));

    EXPECT_EQ(driver.opened(), 1);
    EXPECT_EQ(driver.title().toStdString(), "Ask the ECU at 33  (0x21)");
    EXPECT_EQ(listener.asked, (std::vector<Asked>{{kPgnAddressClaimed, 33, 1}}));
}

TEST(J1939NetworkPanelTests, ARightClickOnNothingOpensNoMenu)
{
    TwoBusBench two;

    MenuDriver driver{0};
    rightClick(two.bench, nullptr);
    QApplication::processEvents();

    EXPECT_EQ(driver.opened(), 0);
    EXPECT_TRUE(two.listener.asked.empty());
}

TEST(J1939NetworkPanelTests, WithOneBusARightClickOnAnEcuAsksItOnThatBusAndNotOnTheFirst)
{
    // The usual case has no heading, and the channel is on the row all the same: this bus is CAN 2.
    Bench bench;
    Listener listener{bench.panel};

    const std::vector<J1939Diagnostic> messages{dm1From(33U, {})};
    bench.network.publish({nodeAt(33U)}, {}, messages, {}, j1939Reporting(messages, 0U), 1U);

    ASSERT_TRUE(bench.waitForSummary(QStringLiteral("1 ECU(s)"))) << bench.summary().toStdString();
    bench.panel.setRequestAvailable(true);

    MenuDriver driver{0};
    rightClick(bench, bench.ecu(33U));

    EXPECT_EQ(driver.opened(), 1);
    EXPECT_EQ(listener.asked, (std::vector<Asked>{{kPgnAddressClaimed, 33, 1}}));
}

TEST(J1939NetworkPanelTests, WhatAnEcuAnsweredToARequestItCouldNotMeetIsOnItsRow)
{
    struct Answer final {
        std::uint8_t control;
        const char* words;
    };

    for (const Answer& answer : {Answer{kJ1939AckPositive, "acknowledged"},
                                 Answer{kJ1939AckNegative, "not supported"},
                                 Answer{kJ1939AckAccessDenied, "access denied"},
                                 Answer{kJ1939AckCannotRespond, "cannot respond"},
                                 Answer{9U, "answer 9"}}) {
        Bench bench;

        J1939NetworkNode node = nodeAt(33U);
        node.lastAnswer = J1939Acknowledgement{answer.control, 0xFFU, kJ1939NullAddress, kPgnDm2};

        const std::vector<J1939Diagnostic> messages{dm1From(33U, {})};
        ASSERT_TRUE(bench.publish({node}, messages, reportedNow(messages)));

        const QTreeWidgetItem* row = bench.ecu(33U);
        ASSERT_NE(row, nullptr);

        // In the detail of the row, with the group in both ways of writing it, and after what the
        // detail always says.
        const QString detail = row->text(4);
        EXPECT_TRUE(detail.startsWith(QStringLiteral("first seen")));
        EXPECT_TRUE(detail.contains(QStringLiteral("; last answer: PGN 65227 (0xFECB) %1")
                                        .arg(QLatin1String{answer.words})))
            << detail.toStdString();
    }
}

TEST(J1939NetworkPanelTests, AnEcuThatHasAnsweredNothingHasNoAnswerOnItsRow)
{
    Bench bench;

    const std::vector<J1939Diagnostic> messages{dm1From(33U, {})};
    ASSERT_TRUE(bench.publish({nodeAt(33U)}, messages, reportedNow(messages)));

    const QTreeWidgetItem* row = bench.ecu(33U);
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->text(4).contains(QStringLiteral("answer")));
}

TEST(J1939NetworkPanelTests, AnAddressIsWrittenInDecimalAndHexTogetherWithTheXLowerCase)
{
    Bench bench;

    const std::vector<J1939Diagnostic> messages{
        dm1From(33U, {}), dm1From(249U, {}), dm1From(5U, {})};
    ASSERT_TRUE(
        bench.publish({nodeAt(5U), nodeAt(33U), nodeAt(249U)}, messages, reportedNow(messages)));

    // The digits upper case, as a trace writes them, and the "x" of "0x" not: 0XF9 is no way to
    // write a number.
    ASSERT_NE(bench.ecu(33U), nullptr);
    ASSERT_NE(bench.ecu(249U), nullptr);
    ASSERT_NE(bench.ecu(5U), nullptr);
    EXPECT_EQ(bench.ecu(33U)->text(0).toStdString(), "33  (0x21)");
    EXPECT_EQ(bench.ecu(249U)->text(0).toStdString(), "249  (0xF9)");
    EXPECT_EQ(bench.ecu(5U)->text(0).toStdString(), "5  (0x05)");
}
