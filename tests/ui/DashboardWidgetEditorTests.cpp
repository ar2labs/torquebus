// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The form that configures one Dashboard widget, with the databases behind it.
//
// The form is a set of controls that write into a description, so these tests do what a person
// does - set a control, press a button, choose from a list - and look at the description, which is
// the thing that is saved. What the controls are is found by name: each has an object name, which
// is part of what the form promises, so that a control cannot be renamed or lost without a test
// saying so.

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/database/DecodedSignal.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardWidgetEditor.h"

#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>

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

BO_ 2566848640 Model: 8 BCM
 SG_ RegimeClass : 29|3@1+ (1,0) [0|4] "class" Vector__XXX

BO_ 300 Doors: 8 BCM
 SG_ DoorState : 0|1@1+ (1,0) [0|1] "" Vector__XXX

BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535;
BA_ "GenMsgCycleTime" BO_ 2364539904 20;

CM_ SG_ 2364539904 EngineSpeed "Crankshaft speed";
VAL_ 2566848640 RegimeClass 0 "Idle" 1 "Cruise" 2 "HighLoad" 3 "ThermalStress" 4 "Anomaly" ;
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

/// The control called `name`, which has to be there.
template<typename Control>
[[nodiscard]] Control* control(QWidget& editor, const char* name)
{
    auto* found = editor.findChild<Control*>(QString::fromLatin1(name));
    EXPECT_NE(found, nullptr) << name;
    return found;
}

/// What typing `text` into a line edit does: the text, and the signal a keystroke sends.
void typeInto(QLineEdit* edit, const QString& text)
{
    edit->setText(text);
    QMetaObject::invokeMethod(edit, "textEdited", Qt::DirectConnection, Q_ARG(QString, text));
}

/// Whether the control would be seen if the form were on screen.
[[nodiscard]] bool shown(QWidget& editor, const char* name)
{
    const QWidget* found = editor.findChild<QWidget*>(QString::fromLatin1(name));
    return found != nullptr && found->isVisibleTo(&editor);
}

[[nodiscard]] DashboardWidget
signalWidget(DashboardWidgetKind kind, std::string id, std::string message, std::string signal)
{
    DashboardWidget widget = defaultWidgetOf(kind);
    widget.id = std::move(id);
    widget.binding.source = DashboardBinding::Source::Signal;
    widget.binding.message = std::move(message);
    widget.binding.signal = std::move(signal);
    return widget;
}

struct Form final {
    DashboardDescription dashboard;
    DashboardWidgetEditor editor{dashboard};
    int edits{0};

    Form()
    {
        editor.setDatabases({database()});

        QObject::connect(
            &editor, &DashboardWidgetEditor::widgetEdited, [this](const QString&) { ++edits; });
    }

    /// Adds `widget` and puts it on the form.
    DashboardWidget& show(DashboardWidget widget)
    {
        const std::string id = widget.id;
        dashboard.add(std::move(widget));
        editor.showWidget(QString::fromStdString(id));

        return const_cast<DashboardWidget&>(*dashboard.find(id));
    }
};

void append(SignalSeriesStore& store,
            const std::string& message,
            const std::string& signal,
            std::uint64_t at,
            double value)
{
    CanMessage canMessage;
    canMessage.name = message;

    CanSignal canSignal;
    canSignal.name = signal;

    DecodedSignal decoded;
    decoded.message = &canMessage;
    decoded.signal = &canSignal;
    decoded.timestampNs = at;
    decoded.value = value;

    store.append(std::span<const DecodedSignal>{&decoded, 1});
}

} // namespace

// ---------------------------------------------------------------------------
// What each kind shows
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, EachKindShowsTheFieldsItUsesAndNoOthers)
{
    Form form;

    // A gauge: a range, a unit, digits, limits, and a time-out for its signal; no names, no
    // picture.
    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed"));

    for (const char* name : {"showAs",
                             "source",
                             "message",
                             "signal",
                             "unit",
                             "minimum",
                             "maximum",
                             "decimals",
                             "stale.on"}) {
        EXPECT_TRUE(shown(form.editor, name)) << "gauge " << name;
    }

    for (const char* name : {"threshold", "picture", "states", "window", "variable", "profile"}) {
        EXPECT_FALSE(shown(form.editor, name)) << "gauge " << name;
    }

    EXPECT_TRUE(form.editor.findChild<QWidget*>(QStringLiteral("limit.warningAbove"))
                    ->isVisibleTo(&form.editor));

    // A lamp: a threshold, names and a picture; no range, unit or digits.
    (void)form.show(signalWidget(DashboardWidgetKind::Lamp, "l", "Doors", "DoorState"));

    for (const char* name : {"threshold", "picture", "states"}) {
        EXPECT_TRUE(shown(form.editor, name)) << "lamp " << name;
    }

    for (const char* name : {"unit", "minimum", "maximum", "decimals", "window"}) {
        EXPECT_FALSE(shown(form.editor, name)) << "lamp " << name;
    }

    EXPECT_FALSE(form.editor.findChild<QWidget*>(QStringLiteral("limit.warningAbove"))
                     ->isVisibleTo(&form.editor));

    // A trend adds the window.
    (void)form.show(signalWidget(DashboardWidgetKind::Trend, "t", "EEC1", "EngineSpeed"));
    EXPECT_TRUE(shown(form.editor, "window"));
    EXPECT_TRUE(shown(form.editor, "minimum"));

    // A state has names and digits but no range and no unit.
    (void)form.show(signalWidget(DashboardWidgetKind::State, "s", "Model", "RegimeClass"));
    EXPECT_TRUE(shown(form.editor, "states"));
    EXPECT_TRUE(shown(form.editor, "decimals"));
    EXPECT_FALSE(shown(form.editor, "minimum"));
    EXPECT_FALSE(shown(form.editor, "unit"));

    // An image has a picture, and its signal is optional.
    DashboardWidget image = defaultWidgetOf(DashboardWidgetKind::Image);
    image.id = "i";
    (void)form.show(image);
    EXPECT_TRUE(shown(form.editor, "picture"));
    EXPECT_TRUE(shown(form.editor, "source"));
    EXPECT_FALSE(shown(form.editor, "message"));
    EXPECT_FALSE(shown(form.editor, "stale.on"));

    // A table has a message and nothing to choose between.
    DashboardWidget table = defaultWidgetOf(DashboardWidgetKind::Table);
    table.id = "tab";
    table.binding.source = DashboardBinding::Source::Message;
    table.binding.message = "EEC1";
    (void)form.show(table);

    EXPECT_TRUE(shown(form.editor, "message"));
    EXPECT_TRUE(shown(form.editor, "browse"));
    EXPECT_FALSE(shown(form.editor, "source"));
    EXPECT_FALSE(shown(form.editor, "signal"));
    EXPECT_FALSE(shown(form.editor, "unit"));
    EXPECT_FALSE(shown(form.editor, "stale.on"));

    // A label and a group have a title and nothing else.
    DashboardWidget label = defaultWidgetOf(DashboardWidgetKind::Label);
    label.id = "heading";
    (void)form.show(label);

    EXPECT_TRUE(shown(form.editor, "title"));
    EXPECT_FALSE(shown(form.editor, "source"));
    EXPECT_FALSE(shown(form.editor, "minimum"));
    EXPECT_FALSE(shown(form.editor, "showAs"));
}

TEST(DashboardWidgetEditorTests, AFaultListShowsATitleAndASentenceAboutWhereItsCodesComeFrom)
{
    Form form;

    DashboardWidget faults = defaultWidgetOf(DashboardWidgetKind::Faults);
    faults.id = "faults";
    (void)form.show(faults);

    // Nothing to bind and nothing to sweep: it has a title, and a line that says what it lists and
    // what it needs - because a form with one field leaves the question that matters unanswered.
    EXPECT_TRUE(shown(form.editor, "title"));
    EXPECT_TRUE(shown(form.editor, "hint"));

    for (const char* name : {"showAs",
                             "source",
                             "message",
                             "signal",
                             "variable",
                             "unit",
                             "minimum",
                             "maximum",
                             "decimals",
                             "threshold",
                             "window",
                             "picture",
                             "states",
                             "stale.on",
                             "profile"}) {
        EXPECT_FALSE(shown(form.editor, name)) << name;
    }

    EXPECT_FALSE(form.editor.findChild<QWidget*>(QStringLiteral("limit.warningAbove"))
                     ->isVisibleTo(&form.editor));

    const auto* hint = control<QLabel>(form.editor, "hint");
    ASSERT_NE(hint, nullptr);
    EXPECT_TRUE(hint->text().contains(QStringLiteral("J1939 block"))) << hint->text().toStdString();

    // The title is the one setting, and it is written back like any other.
    auto* title = control<QLineEdit>(form.editor, "title");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Faults"));

    typeInto(title, QStringLiteral("Active faults"));
    EXPECT_EQ(form.dashboard.find("faults")->title, "Active faults");
    EXPECT_GT(form.edits, 0);

    // And the sentence goes with the kind: another widget on the form does not keep it.
    DashboardWidget label = defaultWidgetOf(DashboardWidgetKind::Label);
    label.id = "heading";
    (void)form.show(label);

    EXPECT_FALSE(shown(form.editor, "hint"));
}

TEST(DashboardWidgetEditorTests, NothingOnTheFormIsWrittenBackWhileItIsBeingFilledIn)
{
    Form form;

    DashboardWidget gauge = signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed");
    gauge.minimum = 100.0;
    gauge.maximum = 7000.0;
    gauge.unit = "RPM";
    gauge.limits.warningAbove = 6000.0;
    gauge.staleMs = 750;

    const DashboardWidget before = gauge;
    (void)form.show(gauge);

    EXPECT_EQ(form.edits, 0);
    EXPECT_EQ(*form.dashboard.find("g"), before);

    // And what is on the form is the widget.
    EXPECT_EQ(control<QLineEdit>(form.editor, "unit")->text().toStdString(), "RPM");
    EXPECT_DOUBLE_EQ(control<QDoubleSpinBox>(form.editor, "minimum")->value(), 100.0);
    EXPECT_TRUE(control<QCheckBox>(form.editor, "limit.warningAbove.on")->isChecked());
    EXPECT_FALSE(control<QCheckBox>(form.editor, "limit.criticalAbove.on")->isChecked());
    EXPECT_EQ(control<QSpinBox>(form.editor, "stale")->value(), 750);
}

// ---------------------------------------------------------------------------
// Completing and browsing
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests,
     TheMessageFieldCompletesFromTheDatabasesAndTheSignalFieldFromTheMessage)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "", ""));

    QLineEdit* message = control<QLineEdit>(form.editor, "message");
    QLineEdit* signal = control<QLineEdit>(form.editor, "signal");
    ASSERT_NE(message->completer(), nullptr);
    ASSERT_NE(signal->completer(), nullptr);

    EXPECT_EQ(message->completer()->model()->rowCount(), 4);

    // With no message yet, the signal field offers every signal, qualified, so that a signal can be
    // found without knowing where it is.
    EXPECT_EQ(signal->completer()->model()->rowCount(), 6);
    EXPECT_EQ(signal->completer()->model()->index(0, 0).data().toString().toStdString(),
              "EEC1.EngineSpeed");

    // With one, the signals of it.
    typeInto(message, QStringLiteral("EFL_P1"));

    EXPECT_EQ(signal->completer()->model()->rowCount(), 2);
    EXPECT_EQ(signal->completer()->model()->index(0, 0).data().toString().toStdString(),
              "EngineOilLevel");

    // A message that is not one is not completed from.
    typeInto(message, QStringLiteral("EFL_P"));
    EXPECT_EQ(signal->completer()->model()->rowCount(), 6);

    EXPECT_EQ(form.dashboard.find("g")->binding.message, "EFL_P");
}

TEST(DashboardWidgetEditorTests, ChoosingASignalFromTheListBindsItAndFillsInWhatTheDatabaseKnows)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "", ""));

    QLineEdit* signal = control<QLineEdit>(form.editor, "signal");

    // The qualified name, as the list offers it when the message was not typed.
    QMetaObject::invokeMethod(signal->completer(),
                              "activated",
                              Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("EEC1.EngineSpeed")));

    const DashboardWidget& widget = *form.dashboard.find("g");

    EXPECT_EQ(widget.binding.message, "EEC1");
    EXPECT_EQ(widget.binding.signal, "EngineSpeed");
    EXPECT_EQ(widget.unit, "rpm");
    EXPECT_DOUBLE_EQ(widget.maximum, 8031.875);
    EXPECT_EQ(widget.title, "Engine Speed");
    EXPECT_EQ(widget.staleMs, 500U);

    // And the form shows it.
    EXPECT_EQ(control<QLineEdit>(form.editor, "message")->text().toStdString(), "EEC1");
    EXPECT_EQ(control<QLineEdit>(form.editor, "signal")->text().toStdString(), "EngineSpeed");
    EXPECT_EQ(control<QLineEdit>(form.editor, "unit")->text().toStdString(), "rpm");
    EXPECT_GE(form.edits, 1);
}

TEST(DashboardWidgetEditorTests, ASignalChosenWithTheMessageAlreadyTypedIsAPlainName)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Numeric, "n", "EFL_P1", ""));

    QLineEdit* signal = control<QLineEdit>(form.editor, "signal");

    QMetaObject::invokeMethod(signal->completer(),
                              "activated",
                              Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("EngineOilPressure")));

    EXPECT_EQ(form.dashboard.find("n")->binding.signal, "EngineOilPressure");
    EXPECT_EQ(form.dashboard.find("n")->unit, "kPa");
}

TEST(DashboardWidgetEditorTests, WhatIsTypedIsKeptAsTypedAndNeverReplacedByAGuess)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed"));

    // Typed by hand, wrongly. The title and the unit are left alone: only a choice from the list
    // fills things in.
    control<QLineEdit>(form.editor, "title")->setText(QStringLiteral("RPM"));
    typeInto(control<QLineEdit>(form.editor, "title"), QStringLiteral("RPM"));
    typeInto(control<QLineEdit>(form.editor, "signal"), QStringLiteral("EngineSped"));

    const DashboardWidget& widget = *form.dashboard.find("g");

    EXPECT_EQ(widget.binding.signal, "EngineSped");
    EXPECT_EQ(widget.title, "RPM");
    EXPECT_TRUE(widget.unit.empty());

    // What is wrong is said under the field, with the nearest thing that is right.
    const QString info = form.editor.bindingText();
    EXPECT_TRUE(info.contains(QStringLiteral("EngineSped")));
    EXPECT_TRUE(info.contains(QStringLiteral("Did you mean EngineSpeed?")));
    EXPECT_TRUE(info.contains(QStringLiteral("color")));
}

// ---------------------------------------------------------------------------
// What the database says of the binding
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, UnderTheFieldsTheBindingIsConfirmedOrCalledWrong)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed"));

    // Found: what it is.
    QString info = form.editor.bindingText();
    EXPECT_TRUE(info.contains(QStringLiteral("rpm")));
    EXPECT_TRUE(info.contains(QStringLiteral("8031.875")));
    EXPECT_TRUE(info.contains(QStringLiteral("every 20 ms")));
    EXPECT_TRUE(info.contains(QStringLiteral("Crankshaft speed")));
    EXPECT_FALSE(info.contains(QStringLiteral("color")));
    EXPECT_TRUE(shown(form.editor, "fill"));

    // The right signal under the wrong message name.
    typeInto(control<QLineEdit>(form.editor, "message"), QStringLiteral("Engine"));
    info = form.editor.bindingText();
    EXPECT_TRUE(info.contains(QStringLiteral("No message")));
    EXPECT_TRUE(info.contains(QStringLiteral("EEC1.EngineSpeed")));
    EXPECT_FALSE(shown(form.editor, "fill"));

    // A variable is not in a database and is not checked.
    control<QComboBox>(form.editor, "source")->setCurrentIndex(2);
    EXPECT_TRUE(form.editor.bindingText().isEmpty());
}

TEST(DashboardWidgetEditorTests, WithNoDatabaseTheNamesAreNotCheckedAndTheFormSaysSo)
{
    DashboardDescription dashboard;
    DashboardWidgetEditor editor{dashboard};

    dashboard.add(signalWidget(DashboardWidgetKind::Gauge, "g", "Whatever", "Whatever"));
    editor.showWidget(QStringLiteral("g"));

    EXPECT_TRUE(editor.bindingText().contains(QStringLiteral("No database is loaded")));
    EXPECT_FALSE(shown(editor, "fill"));

    // Nothing to complete from, and nothing prevents typing.
    auto* message = control<QLineEdit>(editor, "message");
    EXPECT_EQ(message->completer()->model()->rowCount(), 0);
}

TEST(DashboardWidgetEditorTests, FillFromDatabaseRestoresWhatTheDatabaseSaysAfterAHandHasChangedIt)
{
    Form form;

    DashboardWidget& gauge =
        form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EFL_P1", "EngineOilPressure"));
    gauge.unit = "bar";
    gauge.maximum = 10.0;
    gauge.title = "Oil";
    form.editor.showWidget(QStringLiteral("g"));

    control<QPushButton>(form.editor, "fill")->click();

    const DashboardWidget& widget = *form.dashboard.find("g");

    EXPECT_EQ(widget.unit, "kPa");
    EXPECT_DOUBLE_EQ(widget.maximum, 1000.0);

    // A title somebody typed is theirs.
    EXPECT_EQ(widget.title, "Oil");
    EXPECT_GE(form.edits, 1);
}

TEST(DashboardWidgetEditorTests, AnyWidgetThatReadsOneValueCanBeShownAsAnother)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EFL_P1", "EngineOilLevel"));

    auto* showAs = control<QComboBox>(form.editor, "showAs");
    const int bar = showAs->findText(QStringLiteral("Bar"));
    ASSERT_GE(bar, 0);

    showAs->setCurrentIndex(bar);
    QMetaObject::invokeMethod(showAs, "activated", Qt::DirectConnection, Q_ARG(int, bar));

    const DashboardWidget& widget = *form.dashboard.find("g");

    EXPECT_EQ(widget.kind, DashboardWidgetKind::Bar);
    EXPECT_EQ(widget.binding.signal, "EngineOilLevel");
    EXPECT_EQ(widget.unit, "%");
    EXPECT_GT(widget.maximum, widget.minimum);
    EXPECT_EQ(form.dashboard.validate().succeeded(), true);

    // As a state it brings the names the signal has - and a lamp does not need to be told.
    (void)form.show(signalWidget(DashboardWidgetKind::Numeric, "n", "Model", "RegimeClass"));

    auto* again = control<QComboBox>(form.editor, "showAs");
    const int state = again->findText(QStringLiteral("State"));
    ASSERT_GE(state, 0);
    again->setCurrentIndex(state);
    QMetaObject::invokeMethod(again, "activated", Qt::DirectConnection, Q_ARG(int, state));

    EXPECT_EQ(form.dashboard.find("n")->kind, DashboardWidgetKind::State);
    EXPECT_EQ(form.dashboard.find("n")->states.size(), 5U);
}

// ---------------------------------------------------------------------------
// Limits, window, time-out
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, ATickedLimitIsSetAndAnUntickedOneIsNotThere)
{
    Form form;

    DashboardWidget gauge = signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed");
    gauge.minimum = 0.0;
    gauge.maximum = 8000.0;
    (void)form.show(gauge);

    EXPECT_FALSE(form.dashboard.find("g")->limits.any());

    // Ticking one starts from a sensible number, not zero.
    control<QCheckBox>(form.editor, "limit.warningAbove.on")->setChecked(true);

    ASSERT_TRUE(form.dashboard.find("g")->limits.warningAbove.has_value());
    EXPECT_NEAR(*form.dashboard.find("g")->limits.warningAbove, 6400.0, 1.0);

    control<QDoubleSpinBox>(form.editor, "limit.warningAbove")->setValue(5500.0);
    EXPECT_DOUBLE_EQ(*form.dashboard.find("g")->limits.warningAbove, 5500.0);

    control<QCheckBox>(form.editor, "limit.criticalAbove.on")->setChecked(true);
    control<QDoubleSpinBox>(form.editor, "limit.criticalAbove")->setValue(7000.0);

    EXPECT_TRUE(form.dashboard.validate().succeeded());
    EXPECT_EQ(form.dashboard.find("g")->limits.toneOf(6000.0), DashboardTone::Warning);
    EXPECT_EQ(form.dashboard.find("g")->limits.toneOf(7500.0), DashboardTone::Danger);

    control<QCheckBox>(form.editor, "limit.warningAbove.on")->setChecked(false);
    EXPECT_FALSE(form.dashboard.find("g")->limits.warningAbove.has_value());
    EXPECT_TRUE(form.dashboard.find("g")->limits.criticalAbove.has_value());
}

TEST(DashboardWidgetEditorTests, TheWindowAndTheTimeoutAreWrittenAndATimeoutNotTickedIsNone)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Trend, "t", "EEC1", "EngineSpeed"));

    control<QDoubleSpinBox>(form.editor, "window")->setValue(120.0);
    EXPECT_DOUBLE_EQ(form.dashboard.find("t")->windowSeconds, 120.0);

    EXPECT_EQ(form.dashboard.find("t")->staleMs, 0U);

    control<QCheckBox>(form.editor, "stale.on")->setChecked(true);
    EXPECT_GT(form.dashboard.find("t")->staleMs, 0U);

    control<QSpinBox>(form.editor, "stale")->setValue(2500);
    EXPECT_EQ(form.dashboard.find("t")->staleMs, 2500U);

    control<QCheckBox>(form.editor, "stale.on")->setChecked(false);
    EXPECT_EQ(form.dashboard.find("t")->staleMs, 0U);
    EXPECT_FALSE(control<QSpinBox>(form.editor, "stale")->isEnabled());
}

// ---------------------------------------------------------------------------
// Named values
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, NamedValuesAreAddedEditedAndRemovedInATable)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::State, "s", "Model", "RegimeClass"));

    auto* table = control<QTableWidget>(form.editor, "states");
    ASSERT_NE(table, nullptr);

    // Nothing yet: the widget was made by hand and has none.
    EXPECT_EQ(table->rowCount(), 0);

    control<QPushButton>(form.editor, "states.add")->click();
    control<QPushButton>(form.editor, "states.add")->click();

    ASSERT_EQ(form.dashboard.find("s")->states.size(), 2U);
    EXPECT_DOUBLE_EQ(form.dashboard.find("s")->states[0].value, 0.0);
    EXPECT_DOUBLE_EQ(form.dashboard.find("s")->states[1].value, 1.0);

    // Renamed, retinted.
    table->item(1, 1)->setText(QStringLiteral("Cruising"));
    EXPECT_EQ(form.dashboard.find("s")->states[1].label, "Cruising");

    auto* tone = qobject_cast<QComboBox*>(table->cellWidget(1, 2));
    ASSERT_NE(tone, nullptr);
    tone->setCurrentIndex(tone->findData(static_cast<int>(DashboardTone::Ok)));
    EXPECT_EQ(form.dashboard.find("s")->states[1].tone, DashboardTone::Ok);

    auto* value = qobject_cast<QDoubleSpinBox*>(table->cellWidget(1, 0));
    ASSERT_NE(value, nullptr);
    value->setValue(4.0);
    EXPECT_DOUBLE_EQ(form.dashboard.find("s")->states[1].value, 4.0);

    // Removed.
    table->selectRow(0);
    control<QPushButton>(form.editor, "states.remove")->click();

    ASSERT_EQ(form.dashboard.find("s")->states.size(), 1U);
    EXPECT_EQ(form.dashboard.find("s")->states[0].label, "Cruising");
    EXPECT_EQ(table->rowCount(), 1);
}

TEST(DashboardWidgetEditorTests, TwoNamesForOneValueAreSaidInsteadOfSilentlyKept)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::State, "s", "Model", "RegimeClass"));

    control<QPushButton>(form.editor, "states.add")->click();
    control<QPushButton>(form.editor, "states.add")->click();

    auto* table = control<QTableWidget>(form.editor, "states");
    qobject_cast<QDoubleSpinBox*>(table->cellWidget(1, 0))->setValue(0.0);

    EXPECT_TRUE(control<QLabel>(form.editor, "hint")->text().contains(QStringLiteral("only one")));
    EXPECT_TRUE(shown(form.editor, "hint"));
    EXPECT_TRUE(form.dashboard.validate().failed());
}

TEST(DashboardWidgetEditorTests, TheNamesCanBeTakenFromTheDatabaseAgainAndKeepTheirPictures)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    Form form;
    form.editor.setAssetBase(directory.path());

    DashboardWidget image =
        signalWidget(DashboardWidgetKind::Image, "regime", "Model", "RegimeClass");

    DashboardState custom;
    custom.value = 2.0;
    custom.label = "Mine";
    custom.image = "high.png";
    image.states.push_back(custom);

    (void)form.show(image);

    control<QPushButton>(form.editor, "states.fromDatabase")->click();

    const DashboardWidget& widget = *form.dashboard.find("regime");

    ASSERT_EQ(widget.states.size(), 5U);
    EXPECT_EQ(widget.states[2].label, "HighLoad");
    EXPECT_EQ(widget.states[2].image, "high.png");
    EXPECT_TRUE(widget.states[0].image.empty());

    // The picture column is for the kinds that can show one.
    EXPECT_FALSE(control<QTableWidget>(form.editor, "states")->isColumnHidden(3));

    (void)form.show(signalWidget(DashboardWidgetKind::State, "plain", "Model", "RegimeClass"));
    EXPECT_TRUE(control<QTableWidget>(form.editor, "states")->isColumnHidden(3));
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, ThePictureFieldSaysWhatSizeItIsOrWhyItCannotBeShown)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    QImage picture{48, 24, QImage::Format_ARGB32};
    picture.fill(Qt::green);
    ASSERT_TRUE(picture.save(directory.filePath(QStringLiteral("logo.png"))));

    Form form;
    form.editor.setAssetBase(directory.path());

    DashboardWidget image = defaultWidgetOf(DashboardWidgetKind::Image);
    image.id = "logo";
    image.image = "logo.png";
    (void)form.show(image);

    auto* info = control<QLabel>(form.editor, "pictureInfo");
    EXPECT_TRUE(info->text().contains(QStringLiteral("48")));
    EXPECT_TRUE(info->text().contains(QStringLiteral("24")));
    EXPECT_EQ(control<QLineEdit>(form.editor, "picture")->text().toStdString(), "logo.png");

    // Gone from where the project says it is.
    form.dashboard.widgets().front().image = "missing/logo.png";
    form.editor.showWidget(QStringLiteral("logo"));

    EXPECT_TRUE(info->text().contains(QStringLiteral("No such file")));
    EXPECT_TRUE(info->text().contains(QStringLiteral("color")));

    // Cleared.
    control<QPushButton>(form.editor, "clearPicture")->click();
    EXPECT_TRUE(form.dashboard.find("logo")->image.empty());
    EXPECT_TRUE(control<QLineEdit>(form.editor, "picture")->text().isEmpty());
}

// ---------------------------------------------------------------------------
// Whether the signal is arriving
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, TheFormSaysWhetherTheSignalIsArrivingAndWhatToWireIfItIsNot)
{
    Form form;
    SignalSeriesStore store{64};
    form.editor.setPlotStore(&store);

    (void)form.show(signalWidget(DashboardWidgetKind::Gauge, "g", "EEC1", "EngineSpeed"));

    // No series at all: nothing has reached the plot store, which is the commonest reason a gauge
    // shows dashes, and the one nobody can see from the gauge.
    EXPECT_TRUE(form.editor.liveText().contains(QStringLiteral("Signal Plot")));

    append(store, "EEC1", "EngineSpeed", 1'000'000'000ULL, 1500.0);
    form.editor.showWidget(QStringLiteral("g"));
    EXPECT_TRUE(form.editor.liveText().contains(QStringLiteral("Arriving")));
    EXPECT_TRUE(form.editor.liveText().contains(QStringLiteral("1500")));

    // The rest of the bus went on without it.
    append(store, "EFL_P1", "EngineOilLevel", 9'000'000'000ULL, 50.0);
    form.editor.showWidget(QStringLiteral("g"));
    EXPECT_TRUE(form.editor.liveText().contains(QStringLiteral("Silent")));

    // A variable has no such question.
    control<QComboBox>(form.editor, "source")->setCurrentIndex(2);
    EXPECT_TRUE(form.editor.liveText().isEmpty());
}

// ---------------------------------------------------------------------------
// The tables
// ---------------------------------------------------------------------------

TEST(DashboardWidgetEditorTests, ATablesMessageIsCheckedLikeASignalIs)
{
    Form form;

    DashboardWidget table = defaultWidgetOf(DashboardWidgetKind::Table);
    table.id = "tab";
    table.binding.source = DashboardBinding::Source::Message;
    table.binding.message = "EEC1";
    (void)form.show(table);

    EXPECT_TRUE(form.editor.bindingText().contains(QStringLiteral("2 signal")));

    typeInto(control<QLineEdit>(form.editor, "message"), QStringLiteral("EEC"));
    EXPECT_TRUE(form.editor.bindingText().contains(QStringLiteral("Did you mean EEC1")));

    // Still a table bound to a message, whatever was typed.
    EXPECT_EQ(form.dashboard.find("tab")->binding.source, DashboardBinding::Source::Message);
    EXPECT_EQ(form.dashboard.find("tab")->binding.message, "EEC");
}

TEST(DashboardWidgetEditorTests, AControlBoundToASignalIsToldWhyItWillNotWork)
{
    Form form;

    (void)form.show(signalWidget(DashboardWidgetKind::Slider, "pedal", "EEC1", "EngineSpeed"));

    EXPECT_TRUE(
        control<QLabel>(form.editor, "hint")->text().contains(QStringLiteral("cannot write")));
    EXPECT_TRUE(shown(form.editor, "hint"));
}
