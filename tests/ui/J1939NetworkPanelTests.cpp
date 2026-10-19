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

#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939Network.h"
#include "ui/j1939/J1939NetworkPanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <functional>
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

/// The codes of `messages` that are being reported now, as the block hands them over.
[[nodiscard]] std::vector<J1939ActiveFault>
reportedNow(const std::vector<J1939Diagnostic>& messages)
{
    return j1939ActiveFaults(messages, 0U);
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
                               std::vector<J1939ActiveFault> reporting)
    {
        network.publish(std::move(nodes), {}, std::move(messages), std::move(reporting));

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
