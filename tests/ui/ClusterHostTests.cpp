// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The cluster on the Dashboard: from a widget in the description, through a profile and the panel's
// own reader, to the numbers the QML shows.
//
// Every link of that chain is one that fails quietly - a view at the wrong place, a profile that
// feeds nothing, a QML property that never hears of the change - so the tests follow one value from
// where the Dashboard reads it to where the QML cluster holds it, and treat a warning from Qt on
// the way as the failure it is.

#include "QmlWarningCatcher.h"

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SystemVariables.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/database/CanMessage.h"
#include "core/database/DecodedSignal.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardPanel.h"
#include "ui/dashboard/cluster/ClusterDataSource.h"
#include "ui/dashboard/cluster/ClusterHost.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSet>
#include <QWidget>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

/// Runs the event loop until `done` or a second has passed. The panel feeds its clusters from a
/// timer, so there is no single call after which the value is there.
bool waitFor(const std::function<bool()>& done)
{
    QElapsedTimer clock;
    clock.start();

    while (!done()) {
        if (clock.elapsed() > 1000) {
            return false;
        }

        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    return true;
}

/// Qt's own QML modules are not where the test executable's copy of Qt looks: see
/// tests/ui/CMakeLists.txt. The host's engine is its own, so the path goes in through the
/// environment, which every engine reads when it is made.
void pointQmlAtTheQtThatWasFound()
{
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(TORQUEBUS_QT_QML_DIR));
}

[[nodiscard]] DashboardWidget clusterWidget(const std::string& id = "cluster")
{
    DashboardWidget widget;
    widget.id = id;
    widget.kind = DashboardWidgetKind::Cluster;
    widget.profile = std::string{kDefaultClusterProfile};
    widget.x = 10.0;
    widget.y = 20.0;
    widget.width = 720.0;
    widget.height = 315.0;
    return widget;
}

[[nodiscard]] ClusterSource variableSource(ClusterRole role, std::string variable)
{
    ClusterSource source;
    source.role = role;
    source.binding.source = DashboardBinding::Source::Variable;
    source.binding.variable = std::move(variable);
    return source;
}

/// A CAN signal with the age its source allows, where variableSource has none.
[[nodiscard]] ClusterSource
signalSource(ClusterRole role, std::string message, std::string signal, std::uint32_t maxAgeMs)
{
    ClusterSource source;
    source.role = role;
    source.binding.source = DashboardBinding::Source::Signal;
    source.binding.message = std::move(message);
    source.binding.signal = std::move(signal);
    source.maxAgeMs = maxAgeMs;
    return source;
}

/// A profile of the test's own, so that what is read is the test's to choose and the registry's
/// built-in profile stays the one the application ships.
const ClusterProfile& testProfile(const std::string& id, std::vector<ClusterSource> sources)
{
    ClusterProfile profile;
    profile.id = id;
    profile.name = "Test " + id;
    profile.sources = std::move(sources);

    ClusterProfiles::instance().registerProfile(std::move(profile));
    return *ClusterProfiles::instance().find(id);
}

/// A ClusterView's property, which the QML declares and the test reads.
[[nodiscard]] double number(const QQuickWidget& view, const char* property)
{
    return view.rootObject()->property(property).toDouble();
}

} // namespace

// ---------------------------------------------------------------------------
// The data source
// ---------------------------------------------------------------------------

TEST(ClusterDataSourceTests, EveryRoleIsInTheVehicleFromTheStart)
{
    // A binding in QML follows only a property that exists when it is first evaluated. A role added
    // to the map later would never reach a cluster that had already read it.
    ClusterDataSource source;

    QSet<QString> keys;
    for (const QString& key : source.vehicle()->keys()) {
        keys.insert(key);
    }

    EXPECT_EQ(keys.size(), static_cast<qsizetype>(allClusterRoles().size()));

    for (const ClusterRole role : allClusterRoles()) {
        const std::string_view name = nameOf(role);

        EXPECT_TRUE(
            keys.contains(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))))
            << name;
    }
}

TEST(ClusterDataSourceTests, ANumberIsPublishedAndAFlagIsOnAboveHalf)
{
    const ClusterProfile& profile = testProfile("test-data-source",
                                                {variableSource(ClusterRole::Speed, "speed"),
                                                 variableSource(ClusterRole::LampHigh, "high"),
                                                 variableSource(ClusterRole::Ignition, "ign")});

    ClusterDataSource source;
    source.setProfile(&profile);

    double high = 0.6;
    source.update([&high](const DashboardBinding& binding) -> std::optional<double> {
        if (binding.variable == "speed") {
            return 72.5;
        }
        return binding.variable == "high" ? high : 0.5;
    });

    EXPECT_EQ(source.vehicle()->value(QStringLiteral("speed")).toDouble(), 72.5);
    EXPECT_TRUE(source.vehicle()->value(QStringLiteral("lampHigh")).toBool());

    // Above 0.5, not from it: the threshold a Lamp widget starts with, which is "on above".
    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("ignition")).toBool());
    EXPECT_TRUE(source.vehicle()->value(QStringLiteral("ignition")).isValid());

    high = 0.0;
    source.update([&high](const DashboardBinding& binding) -> std::optional<double> {
        return binding.variable == "high" ? high : 0.0;
    });
    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("lampHigh")).toBool());
}

TEST(ClusterDataSourceTests, ARoleThatLosesItsValueIsNoDataAgainAndNotZero)
{
    // A stopped measurement is not a vehicle at standstill. Zero would draw a needle at the bottom
    // of its scale; no data draws dashes.
    const ClusterProfile& profile =
        testProfile("test-data-lost", {variableSource(ClusterRole::Speed, "speed")});

    ClusterDataSource source;
    source.setProfile(&profile);

    source.update([](const DashboardBinding&) -> std::optional<double> { return 50.0; });
    ASSERT_TRUE(source.vehicle()->value(QStringLiteral("speed")).isValid());

    source.update([](const DashboardBinding&) -> std::optional<double> { return std::nullopt; });
    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("speed")).isValid());

    // And so is a value that is not a number: a decode of "not available" must not reach a needle.
    source.update([](const DashboardBinding&) -> std::optional<double> { return std::nan(""); });
    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("speed")).isValid());
}

TEST(ClusterDataSourceTests, ARefreshInWhichNothingMovedTellsQmlNothing)
{
    // A binding that reads the vehicle is evaluated again whenever the role it read is published.
    // Twenty refreshes a second of the same speed should not be twenty evaluations of everything
    // that depends on it.
    const ClusterProfile& profile =
        testProfile("test-quiet", {variableSource(ClusterRole::Speed, "speed")});

    ClusterDataSource source;
    source.setProfile(&profile);

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.setData(
        QByteArrayLiteral("import QtQml\n"
                          "QtObject {\n"
                          "    property var map\n"
                          "    property var counter: ({n: 0})\n"
                          "    readonly property real speed: { counter.n++; return map.speed }\n"
                          "}\n"),
        QUrl{});

    std::unique_ptr<QObject> probe{component.createWithInitialProperties(
        {{QStringLiteral("map"), QVariant::fromValue(source.vehicle())}})};
    ASSERT_TRUE(probe != nullptr) << component.errorString().toStdString();

    const auto evaluations = [&probe] {
        return probe->property("counter").value<QJSValue>().property("n").toInt();
    };

    const int atStart = evaluations();

    const auto reader = [](const DashboardBinding&) -> std::optional<double> { return 50.0; };
    source.update(reader);
    EXPECT_EQ(evaluations(), atStart + 1);

    source.update(reader);
    source.update(reader);
    source.update(reader);
    EXPECT_EQ(evaluations(), atStart + 1);
}

TEST(ClusterDataSourceTests, AnotherProfileDoesNotInheritTheLastOnesValues)
{
    const ClusterProfile& first =
        testProfile("test-first", {variableSource(ClusterRole::Speed, "speed")});
    const ClusterProfile& second =
        testProfile("test-second", {variableSource(ClusterRole::Rpm, "rpm")});

    ClusterDataSource source;
    source.setProfile(&first);
    source.update([](const DashboardBinding&) -> std::optional<double> { return 50.0; });
    ASSERT_TRUE(source.vehicle()->value(QStringLiteral("speed")).isValid());

    source.setProfile(&second);
    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("speed")).isValid());
}

TEST(ClusterDataSourceTests, ASignalThatStopsArrivingIsNoDataOnceItIsOlderThanItsAge)
{
    // The store keeps the last sample of a signal for ever, which is right for a plot and wrong for
    // a speedometer: a cluster that holds 90 km/h after the bus went quiet is telling a lie.
    using Clock = ClusterDataSource::Clock;
    using std::chrono::milliseconds;

    const ClusterProfile& profile =
        testProfile("test-age", {signalSource(ClusterRole::Speed, "Message", "Speed", 2000)});

    ClusterDataSource source;
    source.setProfile(&profile);

    std::uint64_t stamp = 1;
    const auto reader = [&stamp](const DashboardBinding&) -> std::optional<ClusterReading> {
        return ClusterReading{50.0, stamp};
    };
    const auto shown = [&source] {
        return source.vehicle()->value(QStringLiteral("speed")).isValid();
    };

    const Clock::time_point start = Clock::now();

    source.update(reader, start);
    EXPECT_TRUE(shown());

    // The same sample, as old as the source allows and not older: still there.
    source.update(reader, start + milliseconds{2000});
    EXPECT_TRUE(shown());

    source.update(reader, start + milliseconds{2001});
    EXPECT_FALSE(shown());

    // A new sample brings it back, and it has the whole age again from there.
    stamp = 2;
    source.update(reader, start + milliseconds{2500});
    EXPECT_TRUE(shown());

    source.update(reader, start + milliseconds{4500});
    EXPECT_TRUE(shown());
    source.update(reader, start + milliseconds{4501});
    EXPECT_FALSE(shown());
}

TEST(ClusterDataSourceTests, EveryRoleHasItsOwnAge)
{
    using Clock = ClusterDataSource::Clock;
    using std::chrono::milliseconds;

    const ClusterProfile& profile =
        testProfile("test-ages",
                    {signalSource(ClusterRole::Speed, "Message", "Speed", 1000),
                     signalSource(ClusterRole::Rpm, "Message", "Rpm", 5000)});

    ClusterDataSource source;
    source.setProfile(&profile);

    const auto reader = [](const DashboardBinding&) -> std::optional<ClusterReading> {
        return ClusterReading{10.0, std::uint64_t{7}};
    };

    const Clock::time_point start = Clock::now();
    source.update(reader, start);
    source.update(reader, start + milliseconds{3000});

    EXPECT_FALSE(source.vehicle()->value(QStringLiteral("speed")).isValid());
    EXPECT_TRUE(source.vehicle()->value(QStringLiteral("rpm")).isValid());
}

TEST(ClusterDataSourceTests, AnAgeOfZeroAndAValueWithNoStampAreNeverStale)
{
    // A message sent only when something changes: a lamp that is lit stays lit through the
    // silence. And a variable has no sample to be old - one written once is a steady value.
    using Clock = ClusterDataSource::Clock;
    using std::chrono::hours;

    const ClusterProfile& profile =
        testProfile("test-never",
                    {signalSource(ClusterRole::LampLeft, "Lamps", "Left", 0),
                     variableSource(ClusterRole::Speed, "speed")});

    ClusterDataSource source;
    source.setProfile(&profile);

    const auto reader = [](const DashboardBinding& binding) -> std::optional<ClusterReading> {
        if (binding.source == DashboardBinding::Source::Variable) {
            return ClusterReading{80.0};
        }
        return ClusterReading{1.0, std::uint64_t{3}};
    };

    const Clock::time_point start = Clock::now();
    source.update(reader, start);
    source.update(reader, start + hours{1});

    EXPECT_TRUE(source.vehicle()->value(QStringLiteral("lampLeft")).toBool());
    EXPECT_TRUE(source.vehicle()->value(QStringLiteral("speed")).isValid());
}

// ---------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------

TEST(ClusterHostTests, AClusterWidgetGetsAViewAtItsRectangleAndTheQmlLoads)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    QWidget canvas;
    canvas.resize(1000, 700);
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    dashboard.add(clusterWidget());

    host.sync(dashboard, false);
    settle();

    ASSERT_TRUE(host.problem().isEmpty()) << host.problem().toStdString();

    const QQuickWidget* view = host.viewOf("cluster");
    ASSERT_TRUE(view != nullptr);
    EXPECT_EQ(view->geometry(), QRect(10, 20, 720, 315));
    EXPECT_FALSE(view->isHidden());
    EXPECT_TRUE(view->status() == QQuickWidget::Ready);
    ASSERT_TRUE(view->rootObject() != nullptr);

    // The widget was not drawn the way the cluster draws: the panel's mouse still has to reach the
    // panel, which is the whole reason the view is a layer and not a window.
    EXPECT_TRUE(view->testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(view->testAttribute(Qt::WA_AlwaysStackOnTop));

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, TheClusterShowsWhatItsProfileReads)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    const ClusterProfile& profile = testProfile("test-shows",
                                                {variableSource(ClusterRole::Speed, "speed"),
                                                 variableSource(ClusterRole::Coolant, "coolant")});

    QWidget canvas;
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    DashboardWidget widget = clusterWidget();
    widget.profile = profile.id;
    dashboard.add(widget);

    host.sync(dashboard, false);

    host.feed([](const DashboardBinding& binding) -> std::optional<double> {
        if (binding.variable == "speed") {
            return 72.5;
        }
        return binding.variable == "coolant" ? std::optional<double>{91.0} : std::nullopt;
    });

    const QQuickWidget* view = host.viewOf("cluster");
    ASSERT_TRUE(view != nullptr);

    // From the data source through the QML binding to the property the cluster draws from. This is
    // the link a QQmlPropertyMap can break without a word: a key read before it existed.
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 72.5; }));
    EXPECT_EQ(number(*view, "coolant"), 91.0);

    // What the profile has no source for stays dashes.
    EXPECT_TRUE(std::isnan(number(*view, "rpm")));

    // And a value that goes away is dashes again.
    host.feed([](const DashboardBinding&) -> std::optional<double> { return std::nullopt; });
    ASSERT_TRUE(waitFor([view] { return std::isnan(number(*view, "speed")); }));

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, ASignalThatStoppedIsDashesOnTheClusterAndReturnsWithTheBus)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;
    using Clock = ClusterDataSource::Clock;
    using std::chrono::seconds;

    const ClusterProfile& profile =
        testProfile("test-host-age", {signalSource(ClusterRole::Speed, "Message", "Speed", 2000)});

    QWidget canvas;
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    DashboardWidget widget = clusterWidget();
    widget.profile = profile.id;
    dashboard.add(widget);
    host.sync(dashboard, false);

    const QQuickWidget* view = host.viewOf("cluster");
    ASSERT_TRUE(view != nullptr);

    std::uint64_t stamp = 1;
    const auto reader = [&stamp](const DashboardBinding&) -> std::optional<ClusterReading> {
        return ClusterReading{72.5, stamp};
    };

    const Clock::time_point start = Clock::now();

    host.feed(reader, start);
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 72.5; }));

    // Nothing new arrives for longer than the source allows.
    host.feed(reader, start + seconds{3});
    ASSERT_TRUE(waitFor([view] { return std::isnan(number(*view, "speed")); }));

    // The bus comes back.
    stamp = 2;
    host.feed(reader, start + seconds{4});
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 72.5; }));

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, ChangingTheProfileChangesWhatTheClusterReads)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    const ClusterProfile& onSpeed =
        testProfile("test-on-speed", {variableSource(ClusterRole::Speed, "value")});
    const ClusterProfile& onRpm =
        testProfile("test-on-rpm", {variableSource(ClusterRole::Rpm, "value")});

    QWidget canvas;
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    DashboardWidget widget = clusterWidget();
    widget.profile = onSpeed.id;
    dashboard.add(widget);

    const auto reader = [](const DashboardBinding&) -> std::optional<double> { return 1234.0; };

    host.sync(dashboard, false);
    host.feed(reader);

    const QQuickWidget* view = host.viewOf("cluster");
    ASSERT_TRUE(view != nullptr);
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 1234.0; }));

    // What the widget editor does: writes the profile into the description and leaves the rest to
    // the next tick.
    dashboard.widgets().front().profile = onRpm.id;
    host.sync(dashboard, false);
    host.feed(reader);

    ASSERT_TRUE(waitFor([view] { return number(*view, "rpm") == 1234.0; }));
    EXPECT_TRUE(std::isnan(number(*view, "speed")));

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, EditModeTakesTheViewOffTheScreenAndRunModeBringsItBack)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    QWidget canvas;
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    dashboard.add(clusterWidget());

    host.sync(dashboard, false);
    const QQuickWidget* view = host.viewOf("cluster");
    ASSERT_TRUE(view != nullptr);
    EXPECT_FALSE(view->isHidden());

    // The panel draws the selection and the resize handle, and a view over them would hide both.
    host.sync(dashboard, true);
    EXPECT_TRUE(view->isHidden());

    host.sync(dashboard, false);
    EXPECT_FALSE(view->isHidden());

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, AClusterThatIsMovedOrDeletedTakesItsViewWithIt)
{
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    QWidget canvas;
    canvas.show();

    ClusterHost host{canvas};

    DashboardDescription dashboard;
    dashboard.add(clusterWidget("one"));
    dashboard.add(clusterWidget("two"));

    host.sync(dashboard, false);
    EXPECT_EQ(canvas.findChildren<QQuickWidget*>().size(), 2);

    dashboard.widgets().front().x = 300.0;
    host.sync(dashboard, false);
    EXPECT_EQ(host.viewOf("one")->x(), 300);

    dashboard.remove("one");
    host.sync(dashboard, false);
    EXPECT_TRUE(host.viewOf("one") == nullptr);
    EXPECT_TRUE(host.viewOf("two") != nullptr);
    EXPECT_EQ(canvas.findChildren<QQuickWidget*>().size(), 1);

    // A widget that stopped being a cluster is not one any more.
    dashboard.widgets().front().kind = DashboardWidgetKind::Label;
    host.sync(dashboard, false);
    EXPECT_EQ(canvas.findChildren<QQuickWidget*>().size(), 0);

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(ClusterHostTests, TheHostGoesBeforeTheViewsItMadeWithoutAWarning)
{
    // The order the panel relies on: the host, a member, is destroyed while its views, children of
    // the canvas, are still there to be deleted before the engine they run on.
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    auto canvas = std::make_unique<QWidget>();
    canvas->show();

    auto host = std::make_unique<ClusterHost>(*canvas);

    DashboardDescription dashboard;
    dashboard.add(clusterWidget("one"));
    dashboard.add(clusterWidget("two"));
    host->sync(dashboard, false);
    settle();

    host.reset();
    EXPECT_EQ(canvas->findChildren<QQuickWidget*>().size(), 0);

    canvas.reset();
    settle();

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

TEST(DashboardPanelClusterTests, ASignalThatStopsOnTheStoreBecomesDashesOnTheCluster)
{
    // The whole way again, for the age: the panel reads the sample's stamp from the store and the
    // real clock does the rest. A short age keeps the wait short.
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    const ClusterProfile& profile = testProfile(
        "test-panel-age", {signalSource(ClusterRole::Speed, "VehicleSpeed", "SpeedKmh", 150)});

    SignalSeriesStore store{64};

    CanMessage message;
    message.name = "VehicleSpeed";
    CanSignal speed;
    speed.name = "SpeedKmh";

    const auto sample = [&](std::uint64_t at, double value) {
        DecodedSignal decoded;
        decoded.message = &message;
        decoded.signal = &speed;
        decoded.timestampNs = at;
        decoded.value = value;
        store.append(std::span<const DecodedSignal>{&decoded, 1});
    };
    sample(1'000'000ULL, 83.5);

    DashboardDescription dashboard;
    DashboardWidget widget = clusterWidget();
    widget.profile = profile.id;
    dashboard.add(widget);

    SystemVariables variables;

    DashboardPanel panel{dashboard};
    panel.setPlotStore(&store);
    panel.setVariables(&variables);
    panel.resize(900, 600);
    panel.show();
    panel.reload();

    const auto views = panel.findChildren<QQuickWidget*>();
    ASSERT_EQ(views.size(), 1);
    const QQuickWidget* view = views.front();

    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 83.5; }));

    // The store still holds 83.5. The cluster no longer shows it.
    ASSERT_TRUE(waitFor([view] { return std::isnan(number(*view, "speed")); }));

    // A later sample, and it is back.
    sample(2'000'000ULL, 90.0);
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 90.0; }));

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}

TEST(DashboardPanelClusterTests, ASignalOnTheStoreReachesTheCluster)
{
    // The whole way, with nothing in between replaced: a decoded signal in the store the Graph
    // reads, the panel's own timer and reader, the default profile, the QML.
    pointQmlAtTheQtThatWasFound();
    const QmlWarningCatcher catcher;

    SignalSeriesStore store{64};

    CanMessage message;
    message.name = "VehicleSpeed";
    CanSignal speed;
    speed.name = "SpeedKmh";

    DecodedSignal sample;
    sample.message = &message;
    sample.signal = &speed;
    sample.timestampNs = 1'000'000ULL;
    sample.value = 83.5;
    store.append(std::span<const DecodedSignal>{&sample, 1});

    DashboardDescription dashboard;
    dashboard.add(clusterWidget());

    SystemVariables variables;

    auto panel = std::make_unique<DashboardPanel>(dashboard);
    panel->setPlotStore(&store);
    panel->setVariables(&variables);
    panel->resize(900, 600);
    panel->show();
    panel->reload();

    const auto views = panel->findChildren<QQuickWidget*>();
    ASSERT_EQ(views.size(), 1);

    const QQuickWidget* view = views.front();
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 83.5; }));

    // Editing takes the cluster off the screen and Run puts it back; neither loses the value.
    panel->setEditing(true);
    EXPECT_TRUE(view->isHidden());

    panel->setEditing(false);
    EXPECT_FALSE(view->isHidden());
    ASSERT_TRUE(waitFor([view] { return number(*view, "speed") == 83.5; }));

    // Teardown with a cluster on the panel: the host must go before the views it made.
    panel.reset();
    settle();

    ASSERT_TRUE(QmlWarningCatcher::messages().isEmpty()) << QmlWarningCatcher::describe();
}
