// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Dashboard, built from the databases: signals and pictures dropped on it, what it says about a
// widget whose signal is not there, and what it does when a signal goes quiet.
//
// The panel is driven the way a person drives it - a drop event sent to the widget, a clock the
// panel reads for itself - and not through the methods the drop calls, because a drop that is
// accepted and does nothing is the failure this has to catch.

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/database/DecodedSignal.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardPanel.h"
#include "ui/dashboard/SignalMime.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QUrl>

#include <functional>
#include <memory>
#include <string>
#include <vector>

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

/// Runs the event loop until `done` or two seconds have passed. The panel works out what is stale
/// and what is wrong from a timer of its own, so there is no single call after which it has.
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

/// A panel with the database loaded and the room to put widgets in.
struct Bench final {
    DashboardDescription dashboard;
    DashboardPanel panel{dashboard};

    Bench()
    {
        panel.setDatabases({database()});
        panel.resize(900, 600);
        panel.setEditing(true);
    }
};

[[nodiscard]] std::unique_ptr<QMimeData>
signalsDrag(std::initializer_list<SignalReference> references)
{
    return std::unique_ptr<QMimeData>{makeSignalMimeData(QList<SignalReference>{references})};
}

/// Sends the events a drag makes - enter, then drop - and says whether the drop was taken.
[[nodiscard]] bool drop(QWidget& target, const QMimeData& mime, const QPoint& where)
{
    QDragEnterEvent enter{where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(&target, &enter);

    if (!enter.isAccepted()) {
        return false;
    }

    QDropEvent event{QPointF{where}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(&target, &event);

    return event.isAccepted();
}

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

[[nodiscard]] DashboardWidget
boundTo(DashboardWidgetKind kind, std::string id, std::string message, std::string signal)
{
    DashboardWidget widget = defaultWidgetOf(kind);
    widget.id = std::move(id);
    widget.binding.source = DashboardBinding::Source::Signal;
    widget.binding.message = std::move(message);
    widget.binding.signal = std::move(signal);
    return widget;
}

/// The theme the panel is drawn in: the manager's if there is one in this process, as the panel
/// itself asks.
[[nodiscard]] Theme panelTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }

    return Theme::dark();
}

} // namespace

// ---------------------------------------------------------------------------
// Signals dropped on it
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, ADroppedSignalBecomesTheWidgetItSuitsAlreadyBound)
{
    Bench bench;

    const auto mime = signalsDrag({{QStringLiteral("EEC1"), QStringLiteral("EngineSpeed")}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{40, 60}));
    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);

    const DashboardWidget& widget = bench.dashboard.widgets().front();

    EXPECT_EQ(widget.kind, DashboardWidgetKind::Gauge);
    EXPECT_EQ(widget.id, "engine_speed");
    EXPECT_EQ(widget.binding.source, DashboardBinding::Source::Signal);
    EXPECT_EQ(widget.binding.message, "EEC1");
    EXPECT_EQ(widget.binding.signal, "EngineSpeed");
    EXPECT_EQ(widget.unit, "rpm");
    EXPECT_DOUBLE_EQ(widget.maximum, 8031.875);
    EXPECT_EQ(widget.title, "Engine Speed");
    EXPECT_DOUBLE_EQ(widget.x, 40.0);
    EXPECT_DOUBLE_EQ(widget.y, 60.0);

    // Selected, so that the form shows it; and one that validates, so that it can be saved.
    EXPECT_EQ(bench.panel.selectedId(), widget.id);
    EXPECT_TRUE(bench.dashboard.validate().succeeded());
}

TEST(DashboardPanelTests, EachSignalGetsTheWidgetItSuitsAndTheyDoNotLandOnEachOther)
{
    Bench bench;

    const auto mime =
        signalsDrag({{QStringLiteral("EEC1"), QStringLiteral("EngineSpeed")},
                     {QStringLiteral("EFL_P1"), QStringLiteral("EngineOilLevel")},
                     {QStringLiteral("Model"), QStringLiteral("RegimeClass")},
                     {QStringLiteral("Doors"), QStringLiteral("DoorState")},
                     {QStringLiteral("EFL_P1"), QStringLiteral("EngineOilPressure")}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{20, 20}));

    const std::vector<DashboardWidget>& widgets = bench.dashboard.widgets();
    ASSERT_EQ(widgets.size(), 5U);

    EXPECT_EQ(widgets[0].kind, DashboardWidgetKind::Gauge);
    EXPECT_EQ(widgets[1].kind, DashboardWidgetKind::Bar);
    EXPECT_EQ(widgets[2].kind, DashboardWidgetKind::State);
    EXPECT_EQ(widgets[3].kind, DashboardWidgetKind::Lamp);
    EXPECT_EQ(widgets[4].kind, DashboardWidgetKind::Gauge);

    // The state came with the words of its value table, tinted.
    ASSERT_EQ(widgets[2].states.size(), 5U);
    EXPECT_EQ(widgets[2].states[2].label, "HighLoad");
    EXPECT_EQ(widgets[2].states[2].tone, DashboardTone::Warning);

    for (std::size_t a = 0; a < widgets.size(); ++a) {
        for (std::size_t b = a + 1; b < widgets.size(); ++b) {
            const QRectF first{widgets[a].x, widgets[a].y, widgets[a].width, widgets[a].height};
            const QRectF second{widgets[b].x, widgets[b].y, widgets[b].width, widgets[b].height};

            EXPECT_FALSE(first.intersects(second)) << widgets[a].id << " and " << widgets[b].id;
        }
    }

    EXPECT_TRUE(bench.dashboard.validate().succeeded());
}

TEST(DashboardPanelTests, AMessageDroppedBecomesATableOfItsSignals)
{
    Bench bench;

    const auto mime = signalsDrag({{QStringLiteral("EFL_P1"), QString{}}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{30, 30}));
    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);

    const DashboardWidget& table = bench.dashboard.widgets().front();

    EXPECT_EQ(table.kind, DashboardWidgetKind::Table);
    EXPECT_EQ(table.binding.source, DashboardBinding::Source::Message);
    EXPECT_EQ(table.binding.message, "EFL_P1");
    EXPECT_TRUE(bench.dashboard.validate().succeeded());

    // And it lists what the message holds, with nothing yet to say about their values.
    const std::vector<TableRow> rows = bench.panel.tableRowsOf(table.id);

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].name.toStdString(), "Engine Oil Level");
    EXPECT_EQ(rows[1].name.toStdString(), "Engine Oil Pressure");
    EXPECT_FALSE(rows[0].known);
    EXPECT_EQ(rows[0].value.toStdString(), "--");
}

TEST(DashboardPanelTests, ADroppedSignalOntoAWidgetThatReadsOneRebindsThatWidget)
{
    Bench bench;

    DashboardWidget numeric = defaultWidgetOf(DashboardWidgetKind::Numeric);
    numeric.id = "readout";
    numeric.binding.source = DashboardBinding::Source::Variable;
    numeric.binding.variable = "something";
    numeric.x = 100.0;
    numeric.y = 100.0;
    bench.dashboard.add(numeric);

    const auto mime =
        signalsDrag({{QStringLiteral("EFL_P1"), QStringLiteral("EngineOilPressure")}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{150, 150}));

    // The same widget, now reading the signal; none added.
    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);

    const DashboardWidget& widget = bench.dashboard.widgets().front();

    EXPECT_EQ(widget.id, "readout");
    EXPECT_EQ(widget.kind, DashboardWidgetKind::Numeric);
    EXPECT_EQ(widget.binding.source, DashboardBinding::Source::Signal);
    EXPECT_EQ(widget.binding.signal, "EngineOilPressure");
    EXPECT_EQ(widget.unit, "kPa");
    EXPECT_TRUE(widget.binding.variable.empty());
    EXPECT_DOUBLE_EQ(widget.x, 100.0);
}

TEST(DashboardPanelTests, ASignalDroppedOnAControlMakesANewWidgetBecauseAControlCannotReadIt)
{
    Bench bench;

    DashboardWidget slider = defaultWidgetOf(DashboardWidgetKind::Slider);
    slider.id = "pedal";
    slider.binding.source = DashboardBinding::Source::Variable;
    slider.binding.variable = "pedal";
    slider.x = 100.0;
    slider.y = 100.0;
    bench.dashboard.add(slider);

    const auto mime = signalsDrag({{QStringLiteral("EEC1"), QStringLiteral("EngineSpeed")}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{150, 120}));

    // The slider is as it was: a signal cannot be written, so it is not bound to one.
    ASSERT_EQ(bench.dashboard.widgets().size(), 2U);
    EXPECT_EQ(bench.dashboard.find("pedal")->binding.source, DashboardBinding::Source::Variable);
    EXPECT_EQ(bench.dashboard.widgets().back().kind, DashboardWidgetKind::Gauge);
}

TEST(DashboardPanelTests, AnythingTheDatabasesDoNotHaveIsReportedAndNothingIsMadeOfIt)
{
    Bench bench;

    QStringList reports;
    int errors = 0;

    QObject::connect(
        &bench.panel, &DashboardPanel::reported, [&](const QString& text, bool isError) {
            reports << text;
            errors += isError ? 1 : 0;
        });

    const auto mime = signalsDrag({{QStringLiteral("EEC1"), QStringLiteral("Bogus")},
                                   {QStringLiteral("Nowhere"), QString{}}});

    ASSERT_TRUE(drop(bench.panel, *mime, QPoint{10, 10}));

    EXPECT_TRUE(bench.dashboard.empty());
    EXPECT_EQ(errors, 1);
    ASSERT_EQ(reports.size(), 1);
    EXPECT_TRUE(reports.front().contains(QStringLiteral("EEC1.Bogus")));
    EXPECT_TRUE(reports.front().contains(QStringLiteral("Nowhere")));
}

TEST(DashboardPanelTests, ADropIsRefusedUnlessTheDashboardIsBeingEdited)
{
    Bench bench;
    bench.panel.setEditing(false);

    const auto mime = signalsDrag({{QStringLiteral("EEC1"), QStringLiteral("EngineSpeed")}});

    EXPECT_FALSE(drop(bench.panel, *mime, QPoint{10, 10}));
    EXPECT_TRUE(bench.dashboard.empty());

    // And what is neither a signal nor a picture is refused in Edit mode too.
    bench.panel.setEditing(true);

    QMimeData text;
    text.setText(QStringLiteral("EEC1.EngineSpeed"));

    EXPECT_FALSE(drop(bench.panel, text, QPoint{10, 10}));

    QMimeData notes;
    notes.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/notes/readme.txt"))});

    EXPECT_FALSE(drop(bench.panel, notes, QPoint{10, 10}));
    EXPECT_TRUE(bench.dashboard.empty());
}

// ---------------------------------------------------------------------------
// Pictures dropped on it
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, APictureFileDroppedBecomesAnImageSizedToItsProportions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    QImage wide{200, 100, QImage::Format_ARGB32};
    wide.fill(QColor{10, 200, 30});
    ASSERT_TRUE(wide.save(directory.filePath(QStringLiteral("logo.png"))));

    Bench bench;
    bench.panel.setAssetBase(directory.path());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(directory.filePath(QStringLiteral("logo.png")))});

    ASSERT_TRUE(drop(bench.panel, mime, QPoint{25, 35}));
    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);

    const DashboardWidget& image = bench.dashboard.widgets().front();

    EXPECT_EQ(image.kind, DashboardWidgetKind::Image);
    EXPECT_EQ(image.id, "logo");

    // Stored the way a project stores it: relative to its folder.
    EXPECT_EQ(image.image, "logo.png");

    // Twice as wide as it is tall, as the picture is.
    EXPECT_NEAR(image.width / image.height, 2.0, 0.05);
    EXPECT_DOUBLE_EQ(image.x, 25.0);
    EXPECT_DOUBLE_EQ(image.y, 35.0);
    EXPECT_TRUE(bench.dashboard.validate().succeeded());
}

TEST(DashboardPanelTests, APictureDroppedOnAWidgetThatCanWearOneBecomesItsPicture)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    QImage picture{32, 32, QImage::Format_ARGB32};
    picture.fill(Qt::red);
    ASSERT_TRUE(picture.save(directory.filePath(QStringLiteral("door.png"))));

    Bench bench;
    bench.panel.setAssetBase(directory.path());

    DashboardWidget lamp = boundTo(DashboardWidgetKind::Lamp, "door", "Doors", "DoorState");
    lamp.x = 200.0;
    lamp.y = 200.0;
    bench.dashboard.add(lamp);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(directory.filePath(QStringLiteral("door.png")))});

    ASSERT_TRUE(drop(bench.panel, mime, QPoint{230, 230}));

    // Not an Image beside it: the lamp's own picture.
    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);
    EXPECT_EQ(bench.dashboard.find("door")->image, "door.png");

    // But a gauge has no use for one, and a drop on it makes an Image of its own.
    DashboardWidget gauge = boundTo(DashboardWidgetKind::Gauge, "speed", "EEC1", "EngineSpeed");
    gauge.x = 400.0;
    gauge.y = 100.0;
    bench.dashboard.add(gauge);

    ASSERT_TRUE(drop(bench.panel, mime, QPoint{430, 130}));

    EXPECT_EQ(bench.dashboard.widgets().size(), 3U);
    EXPECT_TRUE(bench.dashboard.find("speed")->image.empty());
}

TEST(DashboardPanelTests, SeveralPicturesMakeSeveralImagesSideBySide)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    QMimeData mime;
    QList<QUrl> urls;

    for (const char* name : {"a.png", "b.png", "c.png"}) {
        QImage picture{20, 20, QImage::Format_ARGB32};
        picture.fill(Qt::blue);
        ASSERT_TRUE(picture.save(directory.filePath(QString::fromLatin1(name))));

        urls << QUrl::fromLocalFile(directory.filePath(QString::fromLatin1(name)));
    }

    mime.setUrls(urls);

    Bench bench;
    bench.panel.setAssetBase(directory.path());

    ASSERT_TRUE(drop(bench.panel, mime, QPoint{10, 10}));
    ASSERT_EQ(bench.dashboard.widgets().size(), 3U);

    EXPECT_LT(bench.dashboard.widgets()[0].x, bench.dashboard.widgets()[1].x);
    EXPECT_LT(bench.dashboard.widgets()[1].x, bench.dashboard.widgets()[2].x);
}

TEST(DashboardPanelTests, APictureTheBuildCannotShowIsReportedNotAdded)
{
    Bench bench;

    int errors = 0;
    QObject::connect(&bench.panel, &DashboardPanel::reported, [&](const QString&, bool isError) {
        errors += isError ? 1 : 0;
    });

    EXPECT_TRUE(bench.panel.addImage(QStringLiteral("C:/notes/readme.txt"), QPoint{0, 0}).empty());
    EXPECT_TRUE(bench.dashboard.empty());
    EXPECT_EQ(errors, 1);

    // One that is a picture by its name and is not there is added: the widget says what is wrong
    // with it, which is more use than refusing it.
    EXPECT_FALSE(bench.panel.addImage(QStringLiteral("C:/nowhere/gone.png"), QPoint{0, 0}).empty());
    EXPECT_EQ(bench.dashboard.widgets().size(), 1U);
}

// ---------------------------------------------------------------------------
// Making and arranging widgets
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, AWidgetCanBeDuplicatedAndLayeredWithTheKeyboard)
{
    Bench bench;

    ASSERT_EQ(bench.panel
                  .addSignals({*bench.panel.dictionary().find("EEC1", "EngineSpeed")},
                              std::nullopt,
                              QPoint{50, 50})
                  .size(),
              1U);

    ASSERT_EQ(bench.panel.selectedId(), "engine_speed");

    // Control+D.
    QKeyEvent press{QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier};
    QApplication::sendEvent(&bench.panel, &press);

    ASSERT_EQ(bench.dashboard.widgets().size(), 2U);
    EXPECT_EQ(bench.panel.selectedId(), "engine_speed_2");
    EXPECT_DOUBLE_EQ(bench.dashboard.find("engine_speed_2")->x, 70.0);
    EXPECT_EQ(bench.dashboard.find("engine_speed_2")->binding,
              bench.dashboard.find("engine_speed")->binding);

    // Delete takes the selected one.
    QKeyEvent erase{QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier};
    QApplication::sendEvent(&bench.panel, &erase);

    ASSERT_EQ(bench.dashboard.widgets().size(), 1U);
    EXPECT_EQ(bench.dashboard.widgets().front().id, "engine_speed");

    // And in Run mode neither does anything.
    bench.panel.setEditing(false);
    bench.panel.select("engine_speed");
    QApplication::sendEvent(&bench.panel, &press);
    EXPECT_EQ(bench.dashboard.widgets().size(), 1U);
}

TEST(DashboardPanelTests, BindingAWidgetFillsInWhatTheDatabaseKnowsAndRefusesWhatCannotRead)
{
    Bench bench;

    DashboardWidget bar = defaultWidgetOf(DashboardWidgetKind::Bar);
    bar.id = "level";
    bench.dashboard.add(bar);

    DashboardWidget slider = defaultWidgetOf(DashboardWidgetKind::Slider);
    slider.id = "pedal";
    slider.binding.source = DashboardBinding::Source::Variable;
    slider.binding.variable = "pedal";
    bench.dashboard.add(slider);

    const DictionaryEntry* oil = bench.panel.dictionary().find("EFL_P1", "EngineOilLevel");
    ASSERT_NE(oil, nullptr);

    int edits = 0;
    QObject::connect(&bench.panel, &DashboardPanel::dashboardEdited, [&] { ++edits; });

    EXPECT_TRUE(bench.panel.bindWidget("level", *oil));
    EXPECT_EQ(edits, 1);

    EXPECT_EQ(bench.dashboard.find("level")->binding.signal, "EngineOilLevel");
    EXPECT_EQ(bench.dashboard.find("level")->unit, "%");
    EXPECT_EQ(bench.dashboard.find("level")->title, "Engine Oil Level");

    // A control cannot read a signal; and what is not there cannot be bound.
    EXPECT_FALSE(bench.panel.bindWidget("pedal", *oil));
    EXPECT_FALSE(bench.panel.bindWidget("nothing", *oil));
    EXPECT_EQ(edits, 1);
}

// ---------------------------------------------------------------------------
// Saying what is wrong
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, AWidgetWhoseSignalIsInNoDatabaseIsMarkedAndSaysWhatItProbablyMeant)
{
    Bench bench;

    DashboardWidget typo = boundTo(DashboardWidgetKind::Gauge, "speed", "EEC1", "EngineSped");
    typo.x = 40.0;
    typo.y = 40.0;
    bench.dashboard.add(typo);

    DashboardWidget good =
        boundTo(DashboardWidgetKind::Gauge, "oil", "EFL_P1", "EngineOilPressure");
    good.x = 300.0;
    good.y = 40.0;
    bench.dashboard.add(good);

    bench.panel.show();
    settle();

    ASSERT_TRUE(waitFor([&] { return !bench.panel.findingFor("speed").isEmpty(); }));

    const QString finding = bench.panel.findingFor("speed");
    EXPECT_TRUE(finding.contains(QStringLiteral("EngineSped")));
    EXPECT_TRUE(finding.contains(QStringLiteral("EngineSpeed")));

    // The one that is fine says nothing.
    EXPECT_TRUE(bench.panel.findingFor("oil").isEmpty());

    // Hovering says it; so does a mark on the widget in Edit mode.
    EXPECT_TRUE(bench.panel.detailsAt(QPoint{60, 60}).contains(QStringLiteral("Did you mean")));

    const Theme theme = panelTheme();
    settle();
    const QImage image = bench.panel.grab().toImage();

    // The mark is a filled amber circle in the top left corner of the widget.
    const QColor mark = image.pixelColor(40 + 5, 40 + 11);
    EXPECT_NEAR(mark.red(), theme.lampAmber.red(), 8);
    EXPECT_NEAR(mark.green(), theme.lampAmber.green(), 8);

    const QColor none = image.pixelColor(300 + 5, 40 + 11);
    EXPECT_FALSE(std::abs(none.red() - theme.lampAmber.red()) < 8
                 && std::abs(none.green() - theme.lampAmber.green()) < 8);
}

TEST(DashboardPanelTests, TheMarkGoesWhenTheDatabaseThatHasTheSignalIsLoaded)
{
    DashboardDescription dashboard;
    dashboard.add(boundTo(DashboardWidgetKind::Gauge, "speed", "EEC1", "EngineSpeed"));

    DashboardPanel panel{dashboard};
    panel.resize(600, 400);
    panel.setEditing(true);
    panel.show();

    // No database: nothing is checked, so nothing can be wrong.
    settle();
    EXPECT_TRUE(panel.findingFor("speed").isEmpty());

    // A database that does not have it.
    constexpr const char* other =
        "BO_ 100 Other: 8 ECU\n SG_ Boost : 0|8@1+ (1,0) [0|255] \"kPa\" Vector__XXX\n";
    auto db = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parse(other, *db).succeeded());

    panel.setDatabases({db});
    ASSERT_TRUE(waitFor([&] { return !panel.findingFor("speed").isEmpty(); }));

    // And the one that does.
    panel.setDatabases({database()});
    ASSERT_TRUE(waitFor([&] { return panel.findingFor("speed").isEmpty(); }));
}

TEST(DashboardPanelTests, HoveringSaysWhatAWidgetIsAndWhatItReadsNow)
{
    Bench bench;

    DashboardWidget gauge = boundTo(DashboardWidgetKind::Gauge, "speed", "EEC1", "EngineSpeed");
    gauge.title = "Engine speed";
    gauge.unit = "rpm";
    gauge.x = 40.0;
    gauge.y = 40.0;
    bench.dashboard.add(gauge);

    SignalSeriesStore store{64};
    bench.panel.setPlotStore(&store);

    const QPoint over{80, 80};

    // Nothing over empty canvas.
    EXPECT_TRUE(bench.panel.detailsAt(QPoint{700, 500}).isEmpty());

    const QString before = bench.panel.detailsAt(over);
    EXPECT_TRUE(before.contains(QStringLiteral("Gauge")));
    EXPECT_TRUE(before.contains(QStringLiteral("EEC1.EngineSpeed")));
    EXPECT_TRUE(before.contains(QStringLiteral("No value yet")));

    // What the database says of it is there too: the unit, the range, how often it is sent.
    EXPECT_TRUE(before.contains(QStringLiteral("every 20 ms")));

    append(store, "EEC1", "EngineSpeed", 1'000'000ULL, 1500.0);

    EXPECT_TRUE(bench.panel.detailsAt(over).contains(QStringLiteral("Reads 1500.0 rpm")));
}

// ---------------------------------------------------------------------------
// A signal that goes quiet
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, AWidgetWithATimeoutStopsShowingASignalThatHasGoneQuietAndOnlyThen)
{
    DashboardDescription dashboard;

    DashboardWidget timed = boundTo(DashboardWidgetKind::Numeric, "timed", "EEC1", "EngineSpeed");
    timed.staleMs = 150;
    dashboard.add(timed);

    // The same signal on a widget that was never given a time-out: it holds its last value, as
    // every widget did before there was one.
    DashboardWidget patient =
        boundTo(DashboardWidgetKind::Numeric, "patient", "EEC1", "EngineSpeed");
    patient.x = 200.0;
    dashboard.add(patient);

    SignalSeriesStore store{64};
    append(store, "EEC1", "EngineSpeed", 1'000'000ULL, 1500.0);

    DashboardPanel panel{dashboard};
    panel.setPlotStore(&store);
    panel.resize(500, 200);
    panel.show();
    settle();

    EXPECT_FALSE(panel.isStale("timed"));

    // Nothing new arrives.
    ASSERT_TRUE(waitFor([&] { return panel.isStale("timed"); }));
    EXPECT_FALSE(panel.isStale("patient"));

    // A new sample, and it is current again.
    append(store, "EEC1", "EngineSpeed", 2'000'000ULL, 1600.0);
    ASSERT_TRUE(waitFor([&] { return !panel.isStale("timed"); }));

    // And a signal that never spoke is not stale; it is unknown.
    DashboardDescription silent;
    DashboardWidget never =
        boundTo(DashboardWidgetKind::Numeric, "never", "EEC1", "DriverDemandTorque");
    never.staleMs = 50;
    silent.add(never);

    DashboardPanel other{silent};
    other.setPlotStore(&store);
    other.resize(300, 200);
    other.show();
    settle();

    EXPECT_FALSE(waitFor([&] { return other.isStale("never"); }, 400));
}

TEST(DashboardPanelTests, AStaleWidgetShowsDashesAndTheBadgeAndNotItsLastValue)
{
    DashboardDescription dashboard;

    DashboardWidget bar = boundTo(DashboardWidgetKind::Bar, "level", "EFL_P1", "EngineOilLevel");
    bar.staleMs = 100;
    bar.x = 10.0;
    bar.y = 10.0;
    bar.width = 220.0;
    bar.height = 64.0;
    dashboard.add(bar);

    SignalSeriesStore store{64};
    append(store, "EFL_P1", "EngineOilLevel", 1'000'000ULL, 100.0);

    DashboardPanel panel{dashboard};
    panel.setPlotStore(&store);
    panel.resize(300, 120);
    panel.show();
    settle();

    const Theme theme = panelTheme();
    const auto filledAtTheMiddleOfTheTrack = [&] {
        settle();
        const QImage image = panel.grab().toImage();

        // Well inside the groove on the left, where a full bar is the accent colour.
        const QColor pixel = image.pixelColor(10 + 40, 10 + static_cast<int>(64 * 0.5 + 6));
        return std::abs(pixel.red() - theme.accent.red()) < 6
               && std::abs(pixel.green() - theme.accent.green()) < 6
               && std::abs(pixel.blue() - theme.accent.blue()) < 6;
    };

    ASSERT_TRUE(waitFor([&] { return filledAtTheMiddleOfTheTrack(); }));
    EXPECT_FALSE(panel.isStale("level"));

    ASSERT_TRUE(waitFor([&] { return panel.isStale("level"); }));
    ASSERT_TRUE(waitFor([&] { return !filledAtTheMiddleOfTheTrack(); }));
}

// ---------------------------------------------------------------------------
// A table
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, ATableShowsWhatEachSignalOfItsMessageReadsInTheWordsTheDatabaseHas)
{
    DashboardDescription dashboard;

    DashboardWidget model = defaultWidgetOf(DashboardWidgetKind::Table);
    model.id = "regime";
    model.binding.source = DashboardBinding::Source::Message;
    model.binding.message = "Model";
    dashboard.add(model);

    DashboardWidget engine = defaultWidgetOf(DashboardWidgetKind::Table);
    engine.id = "oil";
    engine.binding.source = DashboardBinding::Source::Message;
    engine.binding.message = "EFL_P1";
    dashboard.add(engine);

    SignalSeriesStore store{64};
    append(store, "Model", "RegimeClass", 1'000'000ULL, 4.0);
    append(store, "EFL_P1", "EngineOilLevel", 1'000'000ULL, 62.0);
    append(store, "EFL_P1", "EngineOilPressure", 1'000'000ULL, 340.0);

    DashboardPanel panel{dashboard};
    panel.setDatabases({database()});
    panel.setPlotStore(&store);

    // A value with a name in the database is shown as the name, tinted by what it means.
    const std::vector<TableRow> regime = panel.tableRowsOf("regime");

    ASSERT_EQ(regime.size(), 1U);
    EXPECT_EQ(regime[0].name.toStdString(), "Regime Class");
    EXPECT_EQ(regime[0].value.toStdString(), "Anomaly");
    EXPECT_EQ(regime[0].tone, DashboardTone::Warning);
    EXPECT_TRUE(regime[0].known);

    // One without is a number to as many places as the signal's resolution deserves, with its unit.
    const std::vector<TableRow> oil = panel.tableRowsOf("oil");

    ASSERT_EQ(oil.size(), 2U);
    EXPECT_EQ(oil[0].value.toStdString(), "62.0 %");
    EXPECT_EQ(oil[1].value.toStdString(), "340 kPa");

    // Not a table, or not there: no rows.
    EXPECT_TRUE(panel.tableRowsOf("nothing").empty());
}

// ---------------------------------------------------------------------------
// Trends of variables
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, ATrendOfAVariableDrawsTheLineItHasSeenSinceTheDashboardWasShown)
{
    DashboardDescription dashboard;

    DashboardWidget trend = defaultWidgetOf(DashboardWidgetKind::Trend);
    trend.id = "pedal";
    trend.binding.source = DashboardBinding::Source::Variable;
    trend.binding.variable = "pedal";
    trend.minimum = 0.0;
    trend.maximum = 1.0;
    trend.windowSeconds = 5.0;
    trend.x = 10.0;
    trend.y = 10.0;
    trend.width = 280.0;
    trend.height = 140.0;
    dashboard.add(trend);

    SystemVariables variables;
    variables.set("pedal", 0.5);

    DashboardPanel panel{dashboard};
    panel.setVariables(&variables);
    panel.resize(320, 180);
    panel.show();
    settle();

    const Theme theme = panelTheme();

    const auto accentPixels = [&] {
        settle();
        const QImage image = panel.grab().toImage();

        int count = 0;
        for (int y = 30; y < 140; ++y) {
            for (int x = 20; x < 280; ++x) {
                const QColor pixel = image.pixelColor(x, y);

                // Close to the line's colour rather than exactly it: a line a pixel and a half
                // wide is only partly over most of the pixels it touches.
                if (std::abs(pixel.red() - theme.accent.red()) < 50
                    && std::abs(pixel.green() - theme.accent.green()) < 50
                    && std::abs(pixel.blue() - theme.accent.blue()) < 50) {
                    ++count;
                }
            }
        }

        return count;
    };

    // A flat line at half height builds up as the panel watches it.
    ASSERT_TRUE(waitFor([&] { return accentPixels() > 12; }));
}

// ---------------------------------------------------------------------------
// Every kind, both modes
// ---------------------------------------------------------------------------

TEST(DashboardPanelTests, EveryKindOfWidgetPaintsInBothModesWithAndWithoutAValue)
{
    DashboardDescription dashboard;
    int row = 0;

    for (const DashboardWidgetKind kind : {DashboardWidgetKind::Gauge,
                                           DashboardWidgetKind::Numeric,
                                           DashboardWidgetKind::Lamp,
                                           DashboardWidgetKind::Button,
                                           DashboardWidgetKind::Switch,
                                           DashboardWidgetKind::Slider,
                                           DashboardWidgetKind::Knob,
                                           DashboardWidgetKind::Label,
                                           DashboardWidgetKind::Bar,
                                           DashboardWidgetKind::State,
                                           DashboardWidgetKind::Image,
                                           DashboardWidgetKind::Trend,
                                           DashboardWidgetKind::Table,
                                           DashboardWidgetKind::Group}) {
        DashboardWidget widget = defaultWidgetOf(kind);
        widget.id = std::string{nameOf(kind)};
        widget.x = 10.0 + 130.0 * (row % 6);
        widget.y = 10.0 + 150.0 * (row / 6);
        widget.width = 120.0;
        widget.height = 130.0;
        ++row;

        const DashboardKindTraits& traits = traitsOf(kind);

        if (traits.binding == BindingNeed::Required || traits.binding == BindingNeed::Optional) {
            widget.binding.source = DashboardBinding::Source::Signal;
            widget.binding.message = "EEC1";
            widget.binding.signal = "EngineSpeed";

            if (writesItsBinding(kind)) {
                widget.binding.source = DashboardBinding::Source::Variable;
                widget.binding.variable = "v";
            }
        } else if (traits.binding == BindingNeed::Message) {
            widget.binding.source = DashboardBinding::Source::Message;
            widget.binding.message = "EEC1";
        }

        DashboardState state;
        state.value = 1500.0;
        state.label = "Cruising";
        state.tone = DashboardTone::Ok;
        widget.states.push_back(state);

        widget.limits.warningAbove = 50.0;
        widget.limits.criticalAbove = 80.0;

        dashboard.add(std::move(widget));
    }

    ASSERT_TRUE(dashboard.validate().succeeded()) << std::string{dashboard.validate().message()};

    SignalSeriesStore store{64};
    SystemVariables variables;

    DashboardPanel panel{dashboard};
    panel.setDatabases({database()});
    panel.setVariables(&variables);
    panel.resize(900, 360);

    const std::vector<double> values{1500.0, 20.0, 60.0, 95.0};

    for (const bool editing : {false, true}) {
        panel.setEditing(editing);

        // Nothing known.
        panel.setPlotStore(nullptr);
        EXPECT_FALSE(panel.grab().toImage().isNull());

        panel.setPlotStore(&store);

        // And a value on each side of each limit.
        std::uint64_t at = 1'000'000ULL;

        for (const double value : values) {
            append(store, "EEC1", "EngineSpeed", at, value);
            at += 1'000'000ULL;

            variables.set("v", value / 100.0);
            panel.select("gauge");

            const QImage image = panel.grab().toImage();
            EXPECT_FALSE(image.isNull());
        }
    }
}

TEST(DashboardPanelTests, AnEmptyDashboardSaysHowToStartInEachMode)
{
    DashboardDescription dashboard;
    DashboardPanel panel{dashboard};
    panel.resize(400, 200);

    const QImage run = panel.grab().toImage();

    panel.setEditing(true);
    const QImage edit = panel.grab().toImage();

    EXPECT_FALSE(run.isNull());
    EXPECT_NE(run, edit);
}
