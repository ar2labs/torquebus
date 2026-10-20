// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the main window does for the Dashboard: it gives the Dashboard and its form the databases a
// project loaded, it tells them where the project's pictures are, and it says on the Output panel
// what a dashboard names that is not there.
//
// None of that is in a panel. It is wiring, in the one object that knows both ends of it, and
// wiring is exactly what a panel's own tests cannot see: a Dashboard that completes from databases
// when it is handed them and is never handed them by the window is a Dashboard that does not. So
// the window is built whole, as the application builds it, and a project is opened the way a person
// opens one.

#include "core/can/CanEngine.h"
#include "core/j1939/J1939Id.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "drivers/api/CanBackendRegistry.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "plugins/host/PluginLoader.h"
#include "services/ProjectFile.h"
#include "services/SettingsStore.h"
#include "ui/dashboard/DashboardPanel.h"
#include "ui/dashboard/DashboardWidgetEditor.h"
#include "ui/dashboard/DashboardWidgetPainters.h"
#include "ui/engine/CanEngineController.h"
#include "ui/j1939/J1939NetworkPanel.h"
#include "ui/mainwindow/Docking.h"
#include "ui/mainwindow/MainWindow.h"
#include "ui/output/OutputPanel.h"
#include "ui/theme/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <span>
#include <string>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

constexpr const char* kDatabase = R"(VERSION "test"
BU_: EMS

BO_ 2364539904 EEC1: 8 EMS
 SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] "rpm" Vector__XXX
)";

void pump(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();

    while (clock.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

/// A project folder with a database, a picture and a project file that names them as a person's
/// project does: relative to itself.
struct ProjectFolder final {
    QTemporaryDir directory;

    [[nodiscard]] QString file(const QString& name) const { return directory.filePath(name); }

    /// Writes `dashboard` as a project that loads the database when `withDatabase` is set.
    [[nodiscard]] QString write(const DashboardDescription& dashboard, bool withDatabase)
    {
        EXPECT_TRUE(directory.isValid());

        QFile database{file(QStringLiteral("test.dbc"))};
        EXPECT_TRUE(database.open(QIODevice::WriteOnly));
        database.write(kDatabase);
        database.close();

        QDir{directory.path()}.mkpath(QStringLiteral("pictures"));

        QImage picture{16, 16, QImage::Format_ARGB32};
        picture.fill(Qt::red);
        EXPECT_TRUE(picture.save(file(QStringLiteral("pictures/there.png"))));

        const QString path = file(QStringLiteral("project.tbsproj"));

        const Result saved = services::ProjectFile::save(
            path,
            GraphDescription{},
            TransmitList{},
            dashboard,
            withDatabase ? QStringList{QStringLiteral("test.dbc")} : QStringList{});
        EXPECT_TRUE(saved.succeeded()) << std::string{saved.message()};

        return path;
    }
};

/// The window, built as the application builds it.
struct Application final {
    QTemporaryDir settingsDirectory;
    services::SettingsStore settings{settingsDirectory.filePath(QStringLiteral("settings.json"))};
    ThemeManager themes;
    NodeCatalog catalog{NodeCatalog::withBuiltinTypes()};
    plugins::PluginLoader loader;
    J1939NameTables names;
    std::unique_ptr<MainWindow> window;

    Application()
    {
        CanBackendRegistry::instance().registerBuiltins();

        window = std::make_unique<MainWindow>(settings, themes, catalog, loader, names);
        window->resize(1400, 900);
        window->show();
        pump(300);
    }

    /// Opens `path` as a person opens a project, and returns what the Output panel said while it
    /// did.
    QString open(const QString& path)
    {
        const QString before = output();

        window->openProject(path);
        pump(400);

        return output().mid(before.size());
    }

    /// Brings the Dashboard to the front: a panel that is not showing does no work, and so has not
    /// checked its widgets against anything.
    void showDashboard() const
    {
        for (DockWidget* dock : window->findChildren<DockWidget*>()) {
            if (dock->uniqueName() == QLatin1String{"torquebus.dock.dashboard"}) {
                dock->setAsCurrentTab();
                break;
            }
        }

        pump(300);
    }

    [[nodiscard]] DashboardPanel& panel() const { return *window->findChild<DashboardPanel*>(); }
    [[nodiscard]] DashboardWidgetEditor& editor() const
    {
        return *window->findChild<DashboardWidgetEditor*>();
    }

    /// Everything the Output panel has said.
    [[nodiscard]] QString output() const
    {
        const auto* view = window->findChild<OutputPanel*>()->findChild<QPlainTextEdit*>();

        return view != nullptr ? view->toPlainText() : QString{};
    }
};

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

/// One window for all of them: building the whole application takes seconds, and what each test
/// looks at is what the Output panel said while *its* project opened, which does not depend on what
/// was opened before.
class MainWindowDashboardTests : public testing::Test {
protected:
    static void SetUpTestSuite() { s_application = std::make_unique<Application>(); }
    static void TearDownTestSuite() { s_application.reset(); }

    [[nodiscard]] static Application& app() { return *s_application; }

private:
    static inline std::unique_ptr<Application> s_application;
};

} // namespace

TEST_F(MainWindowDashboardTests, OpeningAProjectGivesTheDashboardAndItsFormTheDatabasesItLoaded)
{
    ProjectFolder folder;

    DashboardDescription dashboard;
    dashboard.add(signalWidget(DashboardWidgetKind::Gauge, "speed", "EEC1", "EngineSpeed"));

    const QString path = folder.write(dashboard, true);

    const QString said = app().open(path);

    ASSERT_NE(app().panel().dictionary().find("EEC1", "EngineSpeed"), nullptr);

    // The form too: it says what the binding is, and not that no database is loaded.
    app().panel().setEditing(true);
    app().panel().select("speed");
    pump(100);

    ASSERT_EQ(app().editor().currentWidget(), QStringLiteral("speed"));
    EXPECT_TRUE(app().editor().bindingText().contains(QStringLiteral("rpm")))
        << app().editor().bindingText().toStdString();
    EXPECT_FALSE(app().editor().bindingText().contains(QStringLiteral("No database")));

    // A dashboard that names what its databases have has nothing to be told about.
    EXPECT_FALSE(said.contains(QStringLiteral("Dashboard widget"))) << said.toStdString();
}

TEST_F(MainWindowDashboardTests,
       AWidgetNamingWhatNoLoadedDatabaseHasIsSaidOnTheOutputPanelWithTheNearestName)
{
    ProjectFolder folder;

    DashboardDescription dashboard;
    dashboard.add(signalWidget(DashboardWidgetKind::Gauge, "typo", "EEC1", "EngineSped"));
    dashboard.add(signalWidget(DashboardWidgetKind::Numeric, "fine", "EEC1", "EngineSpeed"));

    const QString path = folder.write(dashboard, true);

    const QString said = app().open(path);

    EXPECT_TRUE(said.contains(QStringLiteral("Dashboard widget 'typo'"))) << said.toStdString();
    EXPECT_TRUE(said.contains(QStringLiteral("EngineSped")));
    EXPECT_TRUE(said.contains(QStringLiteral("Did you mean EngineSpeed?")));

    // Only the one that is wrong.
    EXPECT_FALSE(said.contains(QStringLiteral("Dashboard widget 'fine'")));

    // And the panel itself knows, for the mark it wears in Edit mode.
    app().showDashboard();
    EXPECT_FALSE(app().panel().findingFor("typo").isEmpty());
    EXPECT_TRUE(app().panel().findingFor("fine").isEmpty());
}

TEST_F(MainWindowDashboardTests, WithNoDatabaseLoadedNoNameIsCalledWrongAndNothingIsRefused)
{
    ProjectFolder folder;

    DashboardDescription dashboard;
    dashboard.add(signalWidget(DashboardWidgetKind::Gauge, "typo", "EEC1", "EngineSped"));

    const QString path = folder.write(dashboard, false);

    const QString said = app().open(path);

    // The project opened - an empty dictionary cannot say a widget is wrong, and refusing a project
    // over a database that is on another machine would be the wrong trade.
    EXPECT_TRUE(said.contains(QStringLiteral("Opened project.tbsproj"))) << said.toStdString();
    EXPECT_FALSE(said.contains(QStringLiteral("Dashboard widget")));

    EXPECT_TRUE(app().panel().dictionary().empty());

    app().panel().setEditing(true);
    app().panel().select("typo");
    pump(100);

    EXPECT_TRUE(app().editor().bindingText().contains(QStringLiteral("No database is loaded")))
        << app().editor().bindingText().toStdString();
}

TEST_F(MainWindowDashboardTests, ThePicturesOfAProjectAreFoundRelativeToItAndAMissingOneIsSaid)
{
    ProjectFolder folder;

    DashboardDescription dashboard;

    DashboardWidget there = defaultWidgetOf(DashboardWidgetKind::Image);
    there.id = "there";
    there.image = "pictures/there.png";
    dashboard.add(there);

    DashboardWidget gone = defaultWidgetOf(DashboardWidgetKind::Image);
    gone.id = "gone";
    gone.image = "pictures/gone.png";
    dashboard.add(gone);

    const QString path = folder.write(dashboard, false);

    const QString said = app().open(path);

    // The folder the pictures are relative to is the project's, whatever the directory the program
    // was started in.
    EXPECT_EQ(QDir::fromNativeSeparators(app().panel().assetBase()),
              QDir::fromNativeSeparators(QFileInfo{path}.absolutePath()));

    EXPECT_TRUE(said.contains(QStringLiteral("Dashboard widget 'gone'"))) << said.toStdString();
    EXPECT_TRUE(said.contains(QStringLiteral("pictures/gone.png")));
    EXPECT_FALSE(said.contains(QStringLiteral("Dashboard widget 'there'")));
}

TEST_F(MainWindowDashboardTests,
       TheDashboardIsGivenTheBusItsEngineReportsOnAndTheNamesTheWindowKnows)
{
    // The wiring a panel's own tests cannot see: a fault list that lists what it is given is a
    // fault list that lists nothing if the window never gives it the engine's network.
    ProjectFolder folder;

    DashboardDescription dashboard;
    DashboardWidget faults = defaultWidgetOf(DashboardWidgetKind::Faults);
    faults.id = "faults";
    dashboard.add(faults);

    (void)app().open(folder.write(dashboard, false));
    app().showDashboard();

    auto* controller = app().window->findChild<CanEngineController*>();
    ASSERT_NE(controller, nullptr);

    J1939Network& bus = controller->engine().j1939Network();
    bus.clear();

    EXPECT_TRUE(app().panel().faultRows().empty());

    // The tables the window was built with, which name an ECU by the function of its NAME.
    ASSERT_TRUE(app().names.load("function,0,Engine\n").succeeded());

    J1939NetworkNode node;
    node.address = 0U;
    node.claimSeen = true;
    node.name = J1939Name{};
    node.name->function = 0U;

    J1939ActiveFault fault;
    fault.sourceAddress = 0U;
    fault.code.spn = 100U;
    fault.code.spnAssembled = true;
    fault.code.fmi = 1U;
    fault.code.occurrenceCount = 1U;
    fault.lamps.redStop = J1939LampState::On;

    // What the J1939 block publishes while a measurement runs, by the door it uses.
    bus.publish({node}, {}, {}, {fault});

    const std::vector<FaultRow> rows = app().panel().faultRows();

    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].code.toStdString(), "SPN 100 / FMI 1");
    EXPECT_EQ(rows[0].tone, DashboardTone::Danger);
    EXPECT_EQ(rows[0].ecu.toStdString(), QStringLiteral("SA 0 · Engine").toStdString());

    // The next Start clears the hand-over, and the list follows.
    bus.clear();
    app().names.clear();

    EXPECT_TRUE(app().panel().faultRows().empty());
}

TEST_F(MainWindowDashboardTests, TheJ1939PanelsRequestClaimsAsksTheRunningMeasurementAndSaysSo)
{
    // The wiring a panel's own tests cannot see: a button that emits a signal nobody listens to is
    // a button that does nothing, and one that stays on after Stop asks a bus that is not there.
    auto* controller = app().window->findChild<CanEngineController*>();
    ASSERT_NE(controller, nullptr);

    auto* panel = app().window->findChild<J1939NetworkPanel*>();
    ASSERT_NE(panel, nullptr);

    auto* button = panel->findChild<QPushButton*>(QStringLiteral("requestClaimsButton"));
    ASSERT_NE(button, nullptr);

    EXPECT_FALSE(button->isEnabled());

    // The channels the window bound when it was built - whatever this machine offers - or one of
    // its own where there are none, so that there is a bus to ask on.
    bool addedAChannel = false;

    if (controller->engine().channelCount() == 0U) {
        CanChannelConfig config;
        config.deviceHandle = "virtual:0";
        config.timing.bitrate = 500'000;
        ASSERT_TRUE(controller->engine()
                        .addChannel(std::make_unique<VirtualCanBackend>(), config)
                        .succeeded());
        addedAChannel = true;
    }

    const auto channels = static_cast<int>(controller->engine().channelCount());

    std::atomic<int> requests{0};
    const SinkId sink =
        controller->engine().addFrameSink([&requests](std::span<const CanFrame> frames) {
            for (const CanFrame& frame : frames) {
                if (j1939RequestedPgn(frame) == kPgnAddressClaimed) {
                    ++requests;
                }
            }
        });

    controller->start();
    pump(100);
    ASSERT_TRUE(controller->isRunning());

    // The measurement's start turns it on, with nothing else done to it.
    EXPECT_TRUE(button->isEnabled());

    const QString before = app().output();
    button->click();

    // A request on each channel at least: two channels on one virtual bus hear each other's.
    QElapsedTimer clock;
    clock.start();
    while (requests.load() < channels && clock.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    EXPECT_GE(requests.load(), channels);

    const QString said = app().output().mid(before.size());
    EXPECT_TRUE(said.contains(QStringLiteral("asked every ECU on CAN 1"))) << said.toStdString();
    EXPECT_TRUE(said.contains(QStringLiteral("to claim its address again"))) << said.toStdString();

    controller->stop();
    pump(100);

    // And off again when it stops.
    EXPECT_FALSE(button->isEnabled());

    controller->engine().removeFrameSink(sink);

    if (addedAChannel) {
        controller->engine().clearChannels();
    }
}
