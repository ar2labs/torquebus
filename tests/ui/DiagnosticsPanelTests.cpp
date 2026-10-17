// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/DiagnosticSession.h"
#include "core/diagnostics/UdsClient.h"
#include "ui/diagnostics/DiagnosticsPanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void pumpEvents(int ms = 120)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

} // namespace

TEST(DiagnosticsPanelTests, DtcLookupDecodesStandardAndCatalogCodesOffline)
{
    DiagnosticsPanel panel;
    panel.resize(900, 500);
    panel.show();

    auto* lookup = panel.findChild<QLineEdit*>(QStringLiteral("dtcLookupEdit"));
    auto* preset = panel.findChild<QComboBox*>(QStringLiteral("dtcPresetCombo"));
    auto* guide = panel.findChild<QLabel*>(QStringLiteral("dtcGuideLabel"));

    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(preset, nullptr);
    ASSERT_NE(guide, nullptr);

    // Offline by default, yet DTC lookup is always enabled
    EXPECT_TRUE(lookup->isEnabled());
    EXPECT_FALSE(guide->isVisible());

    // Enter a curated code (P0300)
    lookup->setText(QStringLiteral("P0300"));
    EXPECT_TRUE(guide->isVisible());
    EXPECT_TRUE(guide->text().contains(QStringLiteral("P0300")));
    EXPECT_TRUE(guide->text().contains(QStringLiteral("Powertrain")));
    EXPECT_TRUE(guide->text().contains(QStringLiteral("SPN 1322")));
    EXPECT_GT(preset->currentIndex(), 0);

    // Enter a non-catalog OEM code (B1234) - still decodes L D D D D structure
    lookup->setText(QStringLiteral("B1234"));
    EXPECT_TRUE(guide->isVisible());
    EXPECT_TRUE(guide->text().contains(QStringLiteral("B1234")));
    EXPECT_TRUE(guide->text().contains(QStringLiteral("Body")));
    EXPECT_TRUE(guide->text().contains(QStringLiteral("Manufacturer-specific")));
    EXPECT_EQ(preset->currentIndex(), 0);

    // Clearing hides the guide
    lookup->clear();
    EXPECT_FALSE(guide->isVisible());
}

TEST(DiagnosticsPanelTests, ReadDtcResponseAutomaticallyExpandsDtcRowsAndPopulatesGuide)
{
    DiagnosticSession session;
    session.setActive(true);

    DiagnosticsPanel panel;
    panel.setSession(&session);
    panel.resize(900, 500);
    panel.show();

    auto* table = panel.findChild<QTableWidget*>();
    auto* lookup = panel.findChild<QLineEdit*>(QStringLiteral("dtcLookupEdit"));
    auto* guide = panel.findChild<QLabel*>(QStringLiteral("dtcGuideLabel"));

    ASSERT_NE(table, nullptr);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(guide, nullptr);

    // Publish a 0x59 0x02 response with two DTCs from the shipped Lua ECU example:
    // P0128 (0x012800, status 0x2F) and U0035 (0xC03500, status 0x08)
    UdsExchange exchange;
    exchange.outcome = UdsExchange::Outcome::Positive;
    exchange.request = {0x19U, 0x02U, 0xFFU};
    exchange.response = {
        0x59U, 0x02U, 0xFFU, 0x01U, 0x28U, 0x00U, 0x2FU, 0xC0U, 0x35U, 0x00U, 0x08U};
    exchange.elapsedNs = 4'200'000ULL;

    session.publishExchange(exchange);
    pumpEvents(150);

    // 1 main RX row + 2 expanded DTC rows = 3 rows
    ASSERT_EQ(table->rowCount(), 3);
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("RX"));
    EXPECT_TRUE(table->item(0, 3)->text().contains(QStringLiteral("2 DTC(s)")));

    EXPECT_EQ(table->item(1, 1)->text(), QStringLiteral("DTC"));
    EXPECT_TRUE(table->item(1, 2)->text().contains(QStringLiteral("P0128")));
    EXPECT_TRUE(table->item(1, 3)->text().contains(QStringLiteral("Coolant Thermostat")));
    EXPECT_TRUE(table->item(1, 3)->text().contains(QStringLiteral("SPN 110")));

    EXPECT_EQ(table->item(2, 1)->text(), QStringLiteral("DTC"));
    EXPECT_TRUE(table->item(2, 2)->text().contains(QStringLiteral("U0035")));

    // First decoded DTC automatically populates the lookup & diagnostic guide
    EXPECT_EQ(lookup->text(), QStringLiteral("P0128"));
    EXPECT_TRUE(guide->isVisible());
    EXPECT_TRUE(guide->text().contains(QStringLiteral("P0128")));
}
