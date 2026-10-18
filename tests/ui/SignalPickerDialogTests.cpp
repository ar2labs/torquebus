// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Choosing signals instead of typing them: what travels when one is dragged out of the DBC
// Explorer, and the dialog that searches every loaded database.
//
// The drag is tested at the model, where Qt asks the tree for its data when a drag starts, and not
// by calling a function the tree happens to have - a drag that works when called directly and
// carries nothing when started is the failure.

#include "core/dashboard/SignalDictionary.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "ui/dashboard/SignalMime.h"
#include "ui/dashboard/SignalPickerDialog.h"
#include "ui/database/DatabasePanel.h"

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <memory>
#include <string>
#include <vector>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

constexpr const char* kDatabase = R"(VERSION "test"
BU_: EMS BCM

BO_ 2364539904 EEC1: 8 EMS
 SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] "rpm" Vector__XXX
 SG_ DriverDemandTorque : 8|8@1+ (1,-125) [-125|125] "%" Vector__XXX

BO_ 2566844160 EFL_P1: 8 EMS
 SG_ EngineOilLevel : 16|8@1+ (0.4,0) [0|100] "%" Vector__XXX
 SG_ EngineOilPressure : 24|8@1+ (4,0) [0|1000] "kPa" Vector__XXX

BO_ 300 Doors: 8 BCM
 SG_ DoorState : 0|1@1+ (1,0) [0|1] "" Vector__XXX

BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 2364539904 20;

CM_ BO_ 2364539904 "Electronic engine controller 1";
VAL_ 300 DoorState 0 "Closed" 1 "Open" ;
)";

[[nodiscard]] std::shared_ptr<const CanDatabase> database()
{
    auto db = std::make_shared<CanDatabase>();

    const Result result = DbcParser::parse(kDatabase, *db);
    EXPECT_TRUE(result.succeeded()) << std::string{result.message()};

    db->sourcePath = "C:/dbc/test.dbc";
    return db;
}

[[nodiscard]] SignalDictionary dictionary()
{
    const std::vector<std::shared_ptr<const CanDatabase>> databases{database()};
    return SignalDictionary{databases};
}

[[nodiscard]] std::vector<std::string> qualifiedNames(const std::vector<DictionaryEntry>& entries)
{
    std::vector<std::string> names;

    for (const DictionaryEntry& entry : entries) {
        names.push_back(entry.qualified());
    }

    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// What travels
// ---------------------------------------------------------------------------

TEST(SignalMimeTests, TheReferencesComeOutAsTheyWentIn)
{
    const QList<SignalReference> sent{{QStringLiteral("EEC1"), QStringLiteral("EngineSpeed")},
                                      {QStringLiteral("Doors"), QString{}},
                                      {QStringLiteral("EFL_P1"), QStringLiteral("EngineOilLevel")}};

    EXPECT_EQ(decodeSignalReferences(encodeSignalReferences(sent)), sent);

    const std::unique_ptr<QMimeData> mime{makeSignalMimeData(sent)};

    EXPECT_TRUE(hasSignalReferences(mime.get()));
    EXPECT_EQ(signalReferencesOf(mime.get()), sent);

    // And as text, for anything that takes a drop and does not know the format: one a line, the
    // message alone for a message.
    EXPECT_EQ(mime->text().toStdString(), "EEC1.EngineSpeed\nDoors\nEFL_P1.EngineOilLevel");
}

TEST(SignalMimeTests, WhatIsNotAReferenceIsSkippedAndNothingFailsOnIt)
{
    EXPECT_TRUE(decodeSignalReferences(QByteArray{}).isEmpty());
    EXPECT_TRUE(decodeSignalReferences(QByteArray{"not json at all"}).isEmpty());
    EXPECT_TRUE(decodeSignalReferences(QByteArray{"{\"message\":\"A\"}"}).isEmpty());

    // An entry that is not an object, one with no message, and one that is fine.
    const QList<SignalReference> kept = decodeSignalReferences(QByteArray{
        R"([1, "two", {"signal":"Orphan"}, {"message":"EEC1","signal":"EngineSpeed"}])"});

    ASSERT_EQ(kept.size(), 1);
    EXPECT_EQ(kept.front().message, QStringLiteral("EEC1"));
    EXPECT_EQ(kept.front().signal, QStringLiteral("EngineSpeed"));

    QMimeData other;
    other.setText(QStringLiteral("EEC1.EngineSpeed"));

    EXPECT_FALSE(hasSignalReferences(&other));
    EXPECT_TRUE(signalReferencesOf(&other).isEmpty());
    EXPECT_FALSE(hasSignalReferences(nullptr));
    EXPECT_TRUE(signalReferencesOf(nullptr).isEmpty());
}

// ---------------------------------------------------------------------------
// The DBC Explorer as a source
// ---------------------------------------------------------------------------

TEST(DatabasePanelDragTests, SignalsAndMessagesAreDraggedOutOfTheTreeAndAFileIsNot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("test.dbc"));

    {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(kDatabase);
    }

    DatabasePanel panel;
    ASSERT_TRUE(panel.loadDatabase(path));

    auto* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);

    EXPECT_TRUE(tree->dragEnabled());
    EXPECT_TRUE(tree->model()->mimeTypes().contains(QString::fromLatin1(kSignalMimeType)));

    QTreeWidgetItem* file = tree->topLevelItem(0);
    ASSERT_NE(file, nullptr);
    ASSERT_GE(file->childCount(), 2);

    QTreeWidgetItem* eec1 = file->child(0);
    ASSERT_EQ(eec1->text(0).toStdString(), "EEC1");
    ASSERT_GE(eec1->childCount(), 2);

    const auto dragOf = [&](std::initializer_list<QTreeWidgetItem*> items) {
        tree->clearSelection();

        for (QTreeWidgetItem* item : items) {
            item->setSelected(true);
        }

        return std::unique_ptr<QMimeData>{
            tree->model()->mimeData(tree->selectionModel()->selectedIndexes())};
    };

    // Two signals of a message, and a message.
    const auto mime = dragOf({eec1->child(0), eec1->child(1), file->child(2)});
    ASSERT_NE(mime, nullptr);

    const QList<SignalReference> references = signalReferencesOf(mime.get());

    // In the order the tree lists them, which is the order the database does and not the order the
    // test happens to know them in.
    ASSERT_EQ(references.size(), 3);
    EXPECT_EQ(references[0], (SignalReference{eec1->text(0), eec1->child(0)->text(0)}));
    EXPECT_EQ(references[1], (SignalReference{eec1->text(0), eec1->child(1)->text(0)}));
    EXPECT_NE(references[0].signal, references[1].signal);
    EXPECT_EQ(references[2].message, file->child(2)->text(0));
    EXPECT_TRUE(references[2].signal.isEmpty());

    // A message and a signal of it are two things to drop, and each is carried once.
    const auto both = dragOf({eec1, eec1->child(0), eec1->child(0)});
    ASSERT_NE(both, nullptr);
    EXPECT_EQ(signalReferencesOf(both.get()).size(), 2);

    // The file itself is not something a widget can be made of.
    const auto nothing = dragOf({file});
    EXPECT_EQ(nothing, nullptr);
}

// ---------------------------------------------------------------------------
// The picker
// ---------------------------------------------------------------------------

TEST(SignalPickerDialogTests, ItListsEverySignalGroupedByMessageAndNarrowsAsYouType)
{
    const SignalDictionary loaded = dictionary();
    SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::AddWidgets};

    EXPECT_EQ(dialog.shownSignalCount(), 5);

    // Ranked and flat once something is typed.
    dialog.setSearchText(QStringLiteral("oil"));
    EXPECT_EQ(dialog.shownSignalCount(), 2);

    dialog.setSearchText(QStringLiteral("kpa"));
    EXPECT_EQ(dialog.shownSignalCount(), 1);

    dialog.setSearchText(QStringLiteral("zzz"));
    EXPECT_EQ(dialog.shownSignalCount(), 0);

    dialog.setSearchText(QString{});
    EXPECT_EQ(dialog.shownSignalCount(), 5);
}

TEST(SignalPickerDialogTests, SeveralSignalsAndAMessageCanBeChosenAndComeBackInTheDatabasesOrder)
{
    const SignalDictionary loaded = dictionary();
    SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::AddWidgets};

    // The order of the clicks is not the order of the database.
    ASSERT_TRUE(dialog.select("EFL_P1", "EngineOilPressure"));
    ASSERT_TRUE(dialog.select("EEC1", "DriverDemandTorque"));

    // And a message, for a table.
    ASSERT_TRUE(dialog.select("Doors"));

    EXPECT_EQ(qualifiedNames(dialog.chosenSignals()),
              (std::vector<std::string>{"EEC1.DriverDemandTorque", "EFL_P1.EngineOilPressure"}));
    EXPECT_EQ(dialog.chosenMessages(), (std::vector<std::string>{"Doors"}));

    // Automatic until a kind is asked for.
    EXPECT_FALSE(dialog.chosenKind().has_value());

    auto* kind = dialog.findChild<QComboBox*>();
    ASSERT_NE(kind, nullptr);

    const int bar = kind->findText(QStringLiteral("Bar"));
    ASSERT_GE(bar, 0);
    kind->setCurrentIndex(bar);

    ASSERT_TRUE(dialog.chosenKind().has_value());
    EXPECT_EQ(*dialog.chosenKind(), DashboardWidgetKind::Bar);
}

TEST(SignalPickerDialogTests, TheOkButtonWaitsForSomethingToBeChosenAndTheDetailsDescribeIt)
{
    const SignalDictionary loaded = dictionary();
    SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::AddWidgets};

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(buttons, nullptr);

    EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());

    ASSERT_TRUE(dialog.select("EEC1", "EngineSpeed"));
    EXPECT_TRUE(buttons->button(QDialogButtonBox::Ok)->isEnabled());

    const QString details = dialog.detailsText();
    EXPECT_TRUE(details.contains(QStringLiteral("EEC1.EngineSpeed")));
    EXPECT_TRUE(details.contains(QStringLiteral("rpm")));
    EXPECT_TRUE(details.contains(QStringLiteral("8031.875")));
    EXPECT_TRUE(details.contains(QStringLiteral("20 ms")));

    // What it would make by default is said, because it is the thing the person will be surprised
    // by.
    EXPECT_TRUE(details.contains(QStringLiteral("Gauge")));

    // A value table is listed.
    ASSERT_TRUE(dialog.select("Doors", "DoorState"));
    EXPECT_TRUE(dialog.detailsText().contains(QStringLiteral("1 = Open")))
        << dialog.detailsText().toStdString();

    // A message says how many signals a table of it would list.
    ASSERT_TRUE(dialog.select("EEC1"));
    EXPECT_TRUE(dialog.detailsText().contains(QStringLiteral("Electronic engine controller 1")))
        << dialog.detailsText().toStdString();
}

TEST(SignalPickerDialogTests, AskedForOneSignalItOffersOnlySignalsAndAskedForAMessageOnlyMessages)
{
    const SignalDictionary loaded = dictionary();

    {
        SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::ChooseSignal};

        // A message is a folder here: it cannot be selected, and nothing is chosen by trying.
        ASSERT_TRUE(dialog.select("EEC1"));
        EXPECT_TRUE(dialog.chosenSignals().empty());
        EXPECT_TRUE(dialog.chosenMessages().empty());

        ASSERT_TRUE(dialog.select("EEC1", "EngineSpeed"));
        EXPECT_EQ(qualifiedNames(dialog.chosenSignals()),
                  (std::vector<std::string>{"EEC1.EngineSpeed"}));

        // And no kind to choose: the widget is already one.
        EXPECT_EQ(dialog.findChild<QComboBox*>(), nullptr);
    }

    {
        SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::ChooseMessage};

        // Messages and nothing under them.
        EXPECT_EQ(dialog.shownSignalCount(), 0);
        EXPECT_FALSE(dialog.select("EEC1", "EngineSpeed"));

        ASSERT_TRUE(dialog.select("EFL_P1"));
        EXPECT_TRUE(dialog.chosenSignals().empty());

        // Narrowed by what is typed.
        dialog.setSearchText(QStringLiteral("door"));
        EXPECT_FALSE(dialog.select("EEC1"));
        EXPECT_TRUE(dialog.select("Doors"));
    }
}

TEST(SignalPickerDialogTests, DoubleClickingASignalChoosesItAndDoubleClickingAFolderDoesNot)
{
    const SignalDictionary loaded = dictionary();
    SignalPickerDialog dialog{loaded, SignalPickerDialog::Mode::ChooseSignal};

    auto* tree = dialog.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);

    QTreeWidgetItem* message = tree->topLevelItem(0);
    ASSERT_NE(message, nullptr);
    ASSERT_GT(message->childCount(), 0);

    tree->itemDoubleClicked(message, 0);
    EXPECT_NE(dialog.result(), QDialog::Accepted);

    tree->itemDoubleClicked(message->child(0), 0);
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST(SignalPickerDialogTests, WithNoDatabaseItSaysSoAndNothingCanBeChosen)
{
    const SignalDictionary nothing;
    SignalPickerDialog dialog{nothing, SignalPickerDialog::Mode::AddWidgets};

    EXPECT_EQ(dialog.shownSignalCount(), 0);
    EXPECT_TRUE(dialog.detailsText().contains(QStringLiteral("No database")));
    EXPECT_TRUE(dialog.chosenSignals().empty());

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
}
