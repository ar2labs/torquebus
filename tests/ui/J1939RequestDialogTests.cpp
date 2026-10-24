// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Asking for a parameter group: what a typed number is, what is said when it is not one, what the
// list of names fills in, and the question that comes before an ECU is told to forget its fault
// codes.
//
// The number is parsed by hand and the cases below are the ones a hand gets wrong - a sign, a
// second "0x", a number too large for a PGN - because a request is a frame on a bus, and one for a
// group nobody meant is a frame that something answers.
//
// The message box that is asked before DM3 and DM11 runs an event loop of its own, so it is found
// and answered from inside it - the way a hand would - and is closed after a while whatever
// happens, so that a box that opens when it should not fails a test and does not hold it.

#include "core/j1939/J1939Id.h"
#include "ui/j1939/J1939RequestDialog.h"
#include "ui/theme/ThemeManager.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

constexpr std::uint32_t kDm3 = 0x0'FECCU;
constexpr std::uint32_t kDm11 = 0x0'FED3U;

/// Answers the message box that opens, with `button`, and counts the boxes it saw.
class BoxAnswer final {
public:
    explicit BoxAnswer(QMessageBox::StandardButton button)
        : m_button{button}
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { look(); });
        m_timer.start();
    }

    [[nodiscard]] int opened() const { return m_opened; }
    [[nodiscard]] const QString& text() const { return m_text; }
    [[nodiscard]] const QString& title() const { return m_title; }

private:
    void look()
    {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());

        if (box == nullptr) {
            return;
        }

        ++m_opened;
        m_text = box->text();
        m_title = box->windowTitle();

        if (QAbstractButton* answer = box->button(m_button); answer != nullptr) {
            answer->click();
        } else {
            box->reject();
        }
    }

    QMessageBox::StandardButton m_button;
    QTimer m_timer;
    int m_opened{0};
    QString m_text;
    QString m_title;
};

/// A dialog that knows of two ECUs on two buses.
struct Bench final {
    J1939RequestDialog dialog{
        {{0x21, QStringLiteral("ECU at 33")}, {0x00, QStringLiteral("ECU at 0")}},
        {{0, QStringLiteral("CAN 1")}, {1, QStringLiteral("CAN 2")}}};

    [[nodiscard]] QComboBox* box(const char* name) const
    {
        return dialog.findChild<QComboBox*>(QLatin1String{name});
    }

    [[nodiscard]] QLineEdit* field() const
    {
        return dialog.findChild<QLineEdit*>(QStringLiteral("requestPgn"));
    }

    [[nodiscard]] QPushButton* ask() const
    {
        return dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    }

    /// The index of the entry of the group list that says `text`, or -1.
    [[nodiscard]] int groupNamed(const QString& text) const
    {
        return box("requestGroup")->findText(text, Qt::MatchContains);
    }
};

} // namespace

TEST(J1939RequestDialogTests, AGroupIsTypedInDecimalOrInHexAndNothingElseIsOne)
{
    const auto parse = [](const char* text) {
        return J1939RequestDialog::parsePgn(QString::fromLatin1(text));
    };

    using Number = std::optional<std::uint32_t>;

    // The same group, the way a database writes it and the way a trace does.
    EXPECT_EQ(parse("65227"), Number{65227U});
    EXPECT_EQ(parse("0xFECB"), Number{65227U});
    EXPECT_EQ(parse("0XfEcB"), Number{65227U});
    EXPECT_EQ(parse("  65227  "), Number{65227U});

    // A "0" in front is a number, and not a mark that the rest is octal.
    EXPECT_EQ(parse("010"), Number{10U});
    EXPECT_EQ(parse("0"), Number{0U});
    EXPECT_EQ(parse("0x0"), Number{0U});

    // The largest the type holds, and one more.
    EXPECT_EQ(parse("4294967295"), Number{4294967295U});
    EXPECT_EQ(parse("0xFFFFFFFF"), Number{4294967295U});
    EXPECT_EQ(parse("4294967296"), std::nullopt);
    EXPECT_EQ(parse("0x100000000"), std::nullopt);

    // Not numbers, and numbers that would be read as one: a sign, a second "0x", a space inside, a
    // letter that is not a digit of the base, another way of writing a number.
    for (const char* text : {"",
                             "   ",
                             "0x",
                             "abc",
                             "0xZZ",
                             "12 34",
                             "-1",
                             "+1",
                             "-0",
                             "1.5",
                             "1e3",
                             "0x0xFF",
                             "0b11",
                             "0o7",
                             "65227h",
                             "FECB",
                             "0x-1",
                             "0x+1",
                             "6 5"}) {
        EXPECT_EQ(parse(text), std::nullopt) << text;
    }

    // A decimal number has no hex digits: "FECB" is not one, and neither is "65A27".
    EXPECT_EQ(parse("65A27"), std::nullopt);

    // And digits of another alphabet are not digits here, whatever Qt would make of them.
    EXPECT_EQ(J1939RequestDialog::parsePgn(QString::fromUtf8("\xD9\xA3")), std::nullopt);
}

TEST(J1939RequestDialogTests, AGroupIsWrittenInDecimalAndHexTogether)
{
    EXPECT_EQ(J1939RequestDialog::pgnText(65227U).toStdString(), "65227 (0xFECB)");

    // Padded to four digits, and the digits upper case and the "x" not.
    EXPECT_EQ(J1939RequestDialog::pgnText(0U).toStdString(), "0 (0x0000)");
    EXPECT_EQ(J1939RequestDialog::pgnText(0xEE00U).toStdString(), "60928 (0xEE00)");
    EXPECT_EQ(J1939RequestDialog::pgnText(0x3FFFFU).toStdString(), "262143 (0x3FFFF)");
}

TEST(J1939RequestDialogTests, TheListIsTheGroupsWorthAskingForByNameAndEveryOneOfThemCanBeAsked)
{
    const std::vector<J1939RequestPreset>& presets = J1939RequestDialog::presets();

    // The claim first, because it is the one asked for most, then the faults, then who the ECU is.
    ASSERT_GE(presets.size(), 7U);
    EXPECT_EQ(presets.front().pgn, kPgnAddressClaimed);

    std::set<std::uint32_t> groups;
    std::set<QString> labels;

    for (const J1939RequestPreset& preset : presets) {
        EXPECT_FALSE(preset.label.isEmpty());

        // Asked for as it is written on the list - which is what the field is filled with.
        EXPECT_TRUE(J1939RequestDialog::problemWith(QString::number(preset.pgn)).isEmpty())
            << preset.pgn;
        EXPECT_FALSE(J1939RequestDialog::clearsFaultCodes(preset.pgn)) << preset.pgn;

        groups.insert(preset.pgn);
        labels.insert(preset.label);
    }

    EXPECT_EQ(groups.size(), presets.size());
    EXPECT_EQ(labels.size(), presets.size());

    // The ones the panel's help text promises.
    for (const std::uint32_t pgn :
         {kPgnAddressClaimed, kPgnDm1, kPgnDm2, 0xFDC5U, 0xFEDAU, 0xFEEBU, 0xFEECU}) {
        EXPECT_TRUE(groups.contains(pgn)) << pgn;
    }
}

TEST(J1939RequestDialogTests, WhatIsWrongWithAGroupIsSaidInWordsBeforeItCanBeAsked)
{
    using D = J1939RequestDialog;

    EXPECT_TRUE(D::problemWith(QStringLiteral("65227")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0xFECB")).isEmpty());

    // Nothing typed yet is not an error, and says what to type.
    EXPECT_TRUE(D::problemWith(QStringLiteral("")).contains(QStringLiteral("Type the PGN")));
    EXPECT_TRUE(D::problemWith(QStringLiteral("   ")).contains(QStringLiteral("Type the PGN")));

    // Not a number: how it is written.
    EXPECT_TRUE(D::problemWith(QStringLiteral("DM1")).contains(QStringLiteral("not a PGN")));
    EXPECT_TRUE(D::problemWith(QStringLiteral("-5")).contains(QStringLiteral("not a PGN")));
    EXPECT_TRUE(D::problemWith(QStringLiteral("4294967296")).contains(QStringLiteral("not a PGN")));

    // A PGN has eighteen bits: the largest is asked for, and one more is not.
    EXPECT_TRUE(D::problemWith(QStringLiteral("262143")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0x3FFFF")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("262144")).contains(QStringLiteral("262143")));
    EXPECT_TRUE(D::problemWith(QStringLiteral("0x40000")).contains(QStringLiteral("18 bits")));
    EXPECT_TRUE(D::problemWith(QStringLiteral("4294967295")).contains(QStringLiteral("18 bits")));
}

TEST(J1939RequestDialogTests, AGroupForOneEcuEndsInZeroBecauseItsLastByteIsTheAddress)
{
    using D = J1939RequestDialog;

    // Below 0xF000 the last byte is the ECU it goes to, and a request names it as zero.
    EXPECT_TRUE(D::problemWith(QStringLiteral("60928")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0xEE00")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0x1EA00")).isEmpty()); // on the second data page

    const QString words = D::problemWith(QStringLiteral("60929"));
    EXPECT_TRUE(words.contains(QStringLiteral("ends in 00"))) << words.toStdString();

    // And what to type instead.
    EXPECT_TRUE(words.contains(QStringLiteral("60928 (0xEE00)"))) << words.toStdString();
    EXPECT_TRUE(
        D::problemWith(QStringLiteral("0xEEFF")).contains(QStringLiteral("60928 (0xEE00)")));
    EXPECT_TRUE(
        D::problemWith(QStringLiteral("0x1EB05")).contains(QStringLiteral("125696 (0x1EB00)")));

    // The last group that is for one ECU, and the first that is not: from 0xF000 up the last byte
    // is part of the number.
    EXPECT_FALSE(D::problemWith(QStringLiteral("0xEFFF")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0xF000")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0xF0FF")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0xFECA")).isEmpty());
    EXPECT_TRUE(D::problemWith(QStringLiteral("0x3FFFF")).isEmpty());
}

TEST(J1939RequestDialogTests, ItStartsOnTheClaimAskedOfEveryEcuOnEveryBus)
{
    Bench bench;

    ASSERT_TRUE(bench.dialog.pgn().has_value());
    EXPECT_EQ(*bench.dialog.pgn(), kPgnAddressClaimed);
    EXPECT_EQ(bench.dialog.destination(), 255);
    EXPECT_EQ(bench.dialog.channel(), -1);
    EXPECT_TRUE(bench.dialog.canAsk());
    EXPECT_TRUE(bench.dialog.problem().isEmpty());
    EXPECT_TRUE(bench.ask()->isEnabled());
    EXPECT_EQ(bench.ask()->text().toStdString(), "Ask");
    EXPECT_EQ(bench.field()->text().toStdString(), "60928");
    EXPECT_TRUE(bench.dialog.isModal());
}

TEST(J1939RequestDialogTests, TheEcusAndTheBusesGivenAreWhatCanBeChosen)
{
    Bench bench;

    // Named for what each one chooses: the button is the one that says Ask.
    QStringList rows;
    for (const QLabel* label : bench.dialog.findChildren<QLabel*>()) {
        rows.push_back(label->text());
    }

    EXPECT_TRUE(rows.contains(QStringLiteral("ECU")));
    EXPECT_TRUE(rows.contains(QStringLiteral("Channel")));
    EXPECT_TRUE(rows.contains(QStringLiteral("Group")));
    EXPECT_TRUE(rows.contains(QStringLiteral("PGN")));

    // Everyone first, and then each ECU the panel has heard, in the order it gave them.
    const QComboBox* ecus = bench.box("requestDestination");
    ASSERT_EQ(ecus->count(), 3);
    EXPECT_EQ(ecus->itemText(0).toStdString(), "Every ECU");
    EXPECT_EQ(ecus->itemText(1).toStdString(), "ECU at 33");
    EXPECT_EQ(ecus->itemData(1).toInt(), 33);
    EXPECT_EQ(ecus->itemData(2).toInt(), 0);

    const QComboBox* buses = bench.box("requestChannel");
    ASSERT_EQ(buses->count(), 3);
    EXPECT_EQ(buses->itemText(0).toStdString(), "Every channel");
    EXPECT_EQ(buses->itemData(0).toInt(), -1);
    EXPECT_EQ(buses->itemData(1).toInt(), 0);
    EXPECT_EQ(buses->itemData(2).toInt(), 1);

    bench.dialog.setDestination(33);
    bench.dialog.setChannel(1);
    EXPECT_EQ(bench.dialog.destination(), 33);
    EXPECT_EQ(bench.dialog.channel(), 1);

    // The ECU at 0 is address zero, which is not the same as nobody in particular.
    bench.dialog.setDestination(0);
    EXPECT_EQ(bench.dialog.destination(), 0);

    // Back to everyone, and to every channel.
    bench.dialog.setDestination(255);
    bench.dialog.setChannel(-1);
    EXPECT_EQ(bench.dialog.destination(), 255);
    EXPECT_EQ(bench.dialog.channel(), -1);
    EXPECT_EQ(ecus->count(), 3);
    EXPECT_EQ(buses->count(), 3);
}

TEST(J1939RequestDialogTests, AnEcuOrABusThePanelHasNotHeardIsStillOneToAsk)
{
    Bench bench;

    // Opened from a row that has gone by the time the dialog is built: the choice is kept, and
    // added, and not quietly turned into "every ECU".
    bench.dialog.setDestination(77);
    EXPECT_EQ(bench.dialog.destination(), 77);

    const QComboBox* ecus = bench.box("requestDestination");
    EXPECT_EQ(ecus->count(), 4);
    EXPECT_TRUE(ecus->currentText().contains(QStringLiteral("77")));
    EXPECT_TRUE(ecus->currentText().contains(QStringLiteral("0x4D")))
        << ecus->currentText().toStdString();

    bench.dialog.setChannel(5);
    EXPECT_EQ(bench.dialog.channel(), 5);
    EXPECT_EQ(bench.box("requestChannel")->currentText().toStdString(), "CAN 6");

    // Once, and not again the next time.
    bench.dialog.setDestination(77);
    bench.dialog.setChannel(5);
    EXPECT_EQ(ecus->count(), 4);
    EXPECT_EQ(bench.box("requestChannel")->count(), 4);
}

TEST(J1939RequestDialogTests, ChoosingAGroupFromTheListWritesItsNumberInTheField)
{
    Bench bench;

    QComboBox* groups = bench.box("requestGroup");

    const int dm2 = bench.groupNamed(QStringLiteral("Previously active faults (DM2)"));
    ASSERT_GE(dm2, 0);

    // What a click on the list does: the combo moves, and says so.
    groups->setCurrentIndex(dm2);
    Q_EMIT groups->activated(dm2);

    EXPECT_EQ(bench.field()->text().toStdString(), "65227");
    ASSERT_TRUE(bench.dialog.pgn().has_value());
    EXPECT_EQ(*bench.dialog.pgn(), kPgnDm2);

    // The one at the end is a group that is not on the list: an empty field to type it in, and
    // nothing to ask until it is typed.
    const int other = groups->count() - 1;
    groups->setCurrentIndex(other);
    Q_EMIT groups->activated(other);

    EXPECT_TRUE(bench.field()->text().isEmpty());
    EXPECT_FALSE(bench.dialog.pgn().has_value());
    EXPECT_FALSE(bench.dialog.canAsk());
    EXPECT_TRUE(bench.dialog.problem().contains(QStringLiteral("Type the PGN")));
    EXPECT_EQ(groups->currentIndex(), other);
}

TEST(J1939RequestDialogTests, WhatIsTypedSelectsTheGroupTheListKnowsItBy)
{
    Bench bench;
    const QComboBox* groups = bench.box("requestGroup");

    bench.dialog.setPgnText(QStringLiteral("65227"));
    EXPECT_TRUE(groups->currentText().contains(QStringLiteral("(DM2)")))
        << groups->currentText().toStdString();

    // The same group in hex, and the one next to it.
    bench.dialog.setPgnText(QStringLiteral("0xFECA"));
    EXPECT_TRUE(groups->currentText().contains(QStringLiteral("(DM1)")))
        << groups->currentText().toStdString();

    // The first and the last of the list, as they are the places a search by index goes wrong.
    bench.dialog.setPgnText(QStringLiteral("60928"));
    EXPECT_EQ(groups->currentIndex(), 0);

    const std::uint32_t lastGroup = J1939RequestDialog::presets().back().pgn;
    bench.dialog.setPgnText(QString::number(lastGroup));
    EXPECT_EQ(groups->currentIndex(), static_cast<int>(J1939RequestDialog::presets().size()) - 1);

    // A group the list does not know is "another group", and so is nothing typed, and so is a
    // number that is not a group.
    for (const char* text : {"12345", "", "abc", "0x"}) {
        bench.dialog.setPgnText(QString::fromLatin1(text));
        EXPECT_EQ(groups->currentIndex(), groups->count() - 1) << text;
        EXPECT_EQ(groups->currentText().toStdString(), "Another group...") << text;
    }
}

TEST(J1939RequestDialogTests, AskCanBePressedOnlyWhileWhatIsTypedCanBeAskedFor)
{
    Bench bench;

    bench.dialog.setPgnText(QStringLiteral("abc"));
    EXPECT_FALSE(bench.dialog.canAsk());
    EXPECT_FALSE(bench.ask()->isEnabled());
    EXPECT_FALSE(bench.dialog.problem().isEmpty());
    EXPECT_FALSE(bench.dialog.pgn().has_value());

    bench.dialog.setPgnText(QStringLiteral("65227"));
    EXPECT_TRUE(bench.dialog.canAsk());
    EXPECT_TRUE(bench.ask()->isEnabled());
    EXPECT_TRUE(bench.dialog.problem().isEmpty());
    EXPECT_EQ(bench.dialog.pgn(), std::optional<std::uint32_t>{65227U});

    // A number that is one but cannot be asked for is not asked for: the group is not given out.
    bench.dialog.setPgnText(QStringLiteral("60929"));
    EXPECT_FALSE(bench.dialog.canAsk());
    EXPECT_FALSE(bench.dialog.pgn().has_value());

    bench.dialog.setPgnText(QStringLiteral("262144"));
    EXPECT_FALSE(bench.dialog.canAsk());
    EXPECT_FALSE(bench.dialog.pgn().has_value());
}

TEST(J1939RequestDialogTests, WhatIsWrongIsSaidInTheErrorColourAndWhatIsNotTypedYetInTheQuietOne)
{
    // The colours of the theme in force, and not any two that differ.
    ThemeManager themes;
    themes.applyVariant(ThemeVariant::Dark);
    const Theme theme = themes.theme();
    ASSERT_NE(theme.textMuted, theme.error);

    Bench bench;
    const auto* problem = bench.dialog.findChild<QLabel*>(QStringLiteral("requestProblem"));
    ASSERT_NE(problem, nullptr);

    bench.dialog.setPgnText(QStringLiteral(""));
    const QString quiet = problem->styleSheet();

    bench.dialog.setPgnText(QStringLiteral("abc"));
    const QString loud = problem->styleSheet();

    // The quiet one for a field nobody has typed in yet, and the error one for a mistake.
    EXPECT_EQ(quiet, QStringLiteral("color: %1;").arg(theme.textMuted.name()));
    EXPECT_EQ(loud, QStringLiteral("color: %1;").arg(theme.error.name()));

    // And a field that is right says nothing, in whichever colour.
    bench.dialog.setPgnText(QStringLiteral("65227"));
    EXPECT_TRUE(problem->text().isEmpty());
}

TEST(J1939RequestDialogTests, TheGroupsThatClearFaultCodesAreTheTwoThatAreNotQuestions)
{
    EXPECT_TRUE(J1939RequestDialog::clearsFaultCodes(kDm3));
    EXPECT_TRUE(J1939RequestDialog::clearsFaultCodes(kDm11));

    // The ones beside them, which only ask: DM2 is the one before DM3, and DM1 before that.
    for (const std::uint32_t pgn :
         {kPgnDm1, kPgnDm2, kDm3 + 1U, kDm11 - 1U, kDm11 + 1U, 0U, kPgnAddressClaimed}) {
        EXPECT_FALSE(J1939RequestDialog::clearsFaultCodes(pgn)) << pgn;
    }
}

TEST(J1939RequestDialogTests, AskingEveryoneToClearItsFaultCodesAsksForConfirmationFirst)
{
    for (const std::uint32_t group : {kDm3, kDm11}) {
        // Said no to: nothing is asked.
        {
            Bench bench;
            bench.dialog.setPgnText(QString::number(group));
            ASSERT_TRUE(bench.dialog.canAsk());

            BoxAnswer answer{QMessageBox::Cancel};
            bench.dialog.accept();

            EXPECT_EQ(answer.opened(), 1) << group;
            EXPECT_NE(bench.dialog.result(), static_cast<int>(QDialog::Accepted)) << group;

            // In words: what is asked, and that what is cleared is gone.
            EXPECT_TRUE(answer.text().contains(QStringLiteral("clear")))
                << answer.text().toStdString();
            EXPECT_TRUE(answer.text().contains(QStringLiteral("is gone")))
                << answer.text().toStdString();
            EXPECT_TRUE(answer.title().contains(QStringLiteral("Clear fault codes")))
                << answer.title().toStdString();

            // And what was typed is still there to be changed.
            EXPECT_EQ(bench.dialog.pgn(), std::optional<std::uint32_t>{group});
        }

        // Closed with the cross, which is no as well: no button is pressed, and the box is
        // rejected.
        {
            Bench bench;
            bench.dialog.setPgnText(QString::number(group));

            BoxAnswer answer{QMessageBox::NoButton};
            bench.dialog.accept();

            EXPECT_EQ(answer.opened(), 1) << group;
            EXPECT_NE(bench.dialog.result(), static_cast<int>(QDialog::Accepted)) << group;
        }

        // Said yes to: asked.
        {
            Bench bench;
            bench.dialog.setPgnText(QString::number(group));

            BoxAnswer answer{QMessageBox::Yes};
            bench.dialog.accept();

            EXPECT_EQ(answer.opened(), 1) << group;
            EXPECT_EQ(bench.dialog.result(), static_cast<int>(QDialog::Accepted)) << group;
        }
    }
}

TEST(J1939RequestDialogTests, TheConfirmationIsAskedByPressingAskAndNotOnlyByCallingAccept)
{
    Bench bench;
    bench.dialog.setPgnText(QString::number(kDm11));

    BoxAnswer answer{QMessageBox::Cancel};
    bench.ask()->click();

    EXPECT_EQ(answer.opened(), 1);
    EXPECT_NE(bench.dialog.result(), static_cast<int>(QDialog::Accepted));
}

TEST(J1939RequestDialogTests, AQuestionIsAskedWithoutAnyConfirmation)
{
    for (const std::uint32_t group : {kPgnDm1, kPgnDm2, kPgnAddressClaimed}) {
        Bench bench;
        bench.dialog.setPgnText(QString::number(group));

        BoxAnswer answer{QMessageBox::Cancel};
        bench.dialog.accept();

        EXPECT_EQ(answer.opened(), 0) << group;
        EXPECT_EQ(bench.dialog.result(), static_cast<int>(QDialog::Accepted)) << group;
    }
}

TEST(J1939RequestDialogTests, WhatCannotBeAskedForIsNotAskedForEvenIfAcceptIsCalled)
{
    Bench bench;
    bench.dialog.setPgnText(QStringLiteral("not a group"));

    BoxAnswer answer{QMessageBox::Yes};
    bench.dialog.accept();

    EXPECT_EQ(answer.opened(), 0);
    EXPECT_NE(bench.dialog.result(), static_cast<int>(QDialog::Accepted));
}
