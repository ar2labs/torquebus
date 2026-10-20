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
#include "core/j1939/J1939Network.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "services/ProjectFile.h"
#include "ui/j1939/J1939NetworkPanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <functional>
#include <memory>
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
