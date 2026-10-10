// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// examples/projects/j1939-vehicle.tbsproj, opened and run the way a person opens it.
//
// The project is six simulated ECUs, the TinyML virtual ECU, a J1939 block and an instrument
// cluster on the Dashboard, and it is the one place where all of them have to agree: the scripts
// on the wire, the database that names what they send, the profile that reads the names and the
// pipeline that carries one to the other. Each of those has tests of its own. None of them could
// notice that the project joining them did not open, which is what this file is for - an example
// that does not start is worse than none, because it is the first thing somebody tries.
//
// The run is real: the virtual bus, the engine's own executor thread and the clock. It lasts a
// couple of seconds because the slowest message the cluster reads is sent once a second.

#include "core/can/CanEngine.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfile.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/dashboard/cluster/ClusterRole.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "services/ProjectFile.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace torquebus::services;

namespace {

[[nodiscard]] QString projectPath()
{
    return QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/j1939-vehicle.tbsproj");
}

/// Everything a project file holds.
struct Project final {
    GraphDescription pipeline;
    TransmitList transmit;
    DashboardDescription dashboard;
    QStringList databases;
};

[[nodiscard]] Result openProject(Project& project)
{
    return ProjectFile::load(
        projectPath(), project.pipeline, project.transmit, project.dashboard, &project.databases);
}

[[nodiscard]] const DashboardWidget* clusterOf(const Project& project)
{
    for (const DashboardWidget& widget : project.dashboard.widgets()) {
        if (widget.kind == DashboardWidgetKind::Cluster) {
            return &widget;
        }
    }

    return nullptr;
}

/// What a run of the project left in the plot store and the variables.
struct Observed final {
    /// "Message.Signal" -> its newest value, which may be NaN: "not available" is a value.
    std::map<std::string, double> series;

    /// Every error line a script or a block produced.
    std::vector<std::string> errors;

    /// Whether the engine started at all.
    Result started{Result::ok()};

    /// The cluster profile the project's cluster names.
    const ClusterProfile* profile{nullptr};

    /// A role of the profile, as the cluster would read it: the newest sample of its signal or the
    /// value of its variable. Empty when there is nothing to read.
    std::map<ClusterRole, double> roles;
};

/// Opens the project, lets `edit` change it (a scenario, say) and runs it for `duration`.
template<typename Edit>
[[nodiscard]] Observed runProject(std::chrono::milliseconds duration, Edit&& edit)
{
    Observed run;

    Project project;
    run.started = openProject(project);
    if (run.started.failed()) {
        return run;
    }

    edit(project.pipeline);

    const DashboardWidget* cluster = clusterOf(project);
    if (cluster != nullptr) {
        run.profile = ClusterProfiles::instance().find(cluster->profile);
    }

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    run.started = engine.addChannel(std::make_unique<VirtualCanBackend>(), config);
    if (run.started.failed()) {
        return run;
    }

    std::mutex errorMutex;
    engine.addLogSink([&run, &errorMutex](const std::string& text, bool isError) {
        if (isError) {
            const std::lock_guard lock{errorMutex};
            run.errors.push_back(text);
        }
    });

    const std::string base = QFileInfo{projectPath()}.absolutePath().toStdString();
    engine.setGraphDescription(project.pipeline, NodeCatalog::withBuiltinTypes(), base);

    run.started = engine.start();
    if (run.started.failed()) {
        return run;
    }

    std::this_thread::sleep_for(duration);

    // Read while it is still running. The J1939 block puts the lamps of the bus out at Stop, which
    // is right for a cluster - a stopped measurement has no fault to report - and wrong for a test
    // that wants to know what the cluster was showing.
    for (const SeriesInfo& info : engine.plotStore().listSeries()) {
        double value = 0.0;
        std::uint64_t when = 0;
        if (engine.plotStore().latest(info.id, value, when)) {
            run.series[info.name] = value;
        }
    }

    if (run.profile != nullptr) {
        for (const ClusterSource& source : run.profile->sources) {
            if (source.binding.source == DashboardBinding::Source::Signal) {
                const auto found =
                    run.series.find(source.binding.message + "." + source.binding.signal);
                if (found != run.series.end()) {
                    run.roles[source.role] = found->second;
                }
            } else if (source.binding.source == DashboardBinding::Source::Variable) {
                run.roles[source.role] = engine.variables().value(source.binding.variable);
            }
        }
    }

    engine.stop();

    return run;
}

[[nodiscard]] Observed runProject(std::chrono::milliseconds duration)
{
    return runProject(duration, [](GraphDescription&) { });
}

[[nodiscard]] std::string joined(const std::vector<std::string>& lines)
{
    std::ostringstream text;
    for (const std::string& line : lines) {
        text << "\n  " << line;
    }
    return text.str();
}

/// "speed=100 rpm=2400 ..." for a failure message that shows what the cluster would have had.
[[nodiscard]] std::string describe(const Observed& run)
{
    std::ostringstream text;
    for (const auto& [role, value] : run.roles) {
        text << ' ' << nameOf(role) << '=' << value;
    }
    return text.str();
}

/// Sets one parameter of one block, the way the Block panel would.
void setParameter(GraphDescription& pipeline,
                  const std::string& node,
                  const std::string& name,
                  ParameterValue value)
{
    for (NodeDescription& description : pipeline.nodes()) {
        if (description.id == node) {
            description.parameters.set(name, std::move(value));
            return;
        }
    }

    ADD_FAILURE() << "the project has no block called " << node;
}

void setScenario(GraphDescription& pipeline, const std::string& node, const char* scenario)
{
    setParameter(pipeline, node, "scenario", ParameterValue::fromText(scenario));
}

[[nodiscard]] bool has(const Observed& run, ClusterRole role)
{
    const auto found = run.roles.find(role);
    return found != run.roles.end() && std::isfinite(found->second);
}

[[nodiscard]] double of(const Observed& run, ClusterRole role)
{
    const auto found = run.roles.find(role);
    return found != run.roles.end() ? found->second : std::nan("");
}

} // namespace

TEST(J1939ExampleProjectTests, TheProjectOpensAndItsPipelineValidates)
{
    Project project;
    const Result opened = openProject(project);

    SCOPED_TRACE(::testing::Message() << std::string{opened.message()});
    ASSERT_TRUE(opened.succeeded());

    // The check the application makes when Start is pressed. An input takes one wire, and a project
    // that wired six ECUs to one transmit block was refused here, with "Cannot start", by the very
    // first thing somebody did with it.
    const Result valid = project.pipeline.validate(NodeCatalog::withBuiltinTypes());

    SCOPED_TRACE(::testing::Message() << std::string{valid.message()});
    EXPECT_TRUE(valid.succeeded());

    EXPECT_TRUE(project.dashboard.validate().succeeded());
}

TEST(J1939ExampleProjectTests, TheProjectNamesItsFilesRelativeToItselfAndTheyExist)
{
    Project project;
    ASSERT_TRUE(openProject(project).succeeded());

    const QDir folder = QFileInfo{projectPath()}.absoluteDir();

    ASSERT_FALSE(project.databases.isEmpty());
    for (const QString& name : project.databases) {
        SCOPED_TRACE(name.toStdString());
        EXPECT_FALSE(QFileInfo{name}.isAbsolute());
        EXPECT_TRUE(QFileInfo{folder.filePath(name)}.isFile());
    }

    int scripts = 0;
    int databases = 0;
    for (const NodeDescription& node : project.pipeline.nodes()) {
        for (const char* key : {"scriptPath", "database"}) {
            if (!node.parameters.contains(key)) {
                continue;
            }

            const QString name = QString::fromStdString(node.parameters.text(key));
            SCOPED_TRACE(node.id + ": " + name.toStdString());

            EXPECT_FALSE(QFileInfo{name}.isAbsolute());
            EXPECT_TRUE(QFileInfo{folder.filePath(name)}.isFile());

            scripts += std::string{key} == "scriptPath" ? 1 : 0;
            databases += std::string{key} == "database" ? 1 : 0;
        }
    }

    // Six ECUs in Lua, and a decoder with a database: the claim the project makes about itself.
    EXPECT_EQ(scripts, 6);
    EXPECT_GE(databases, 1);
}

TEST(J1939ExampleProjectTests, TheClusterNamesAProfileThisBuildHas)
{
    Project project;
    ASSERT_TRUE(openProject(project).succeeded());

    const DashboardWidget* cluster = clusterOf(project);
    ASSERT_TRUE(cluster != nullptr);
    EXPECT_EQ(cluster->profile, std::string{kJ1939CommercialProfile});
    EXPECT_TRUE(ClusterProfiles::instance().find(cluster->profile) != nullptr);
}

namespace {

constexpr std::chrono::milliseconds kRun{2500};

/// Fails with the whole picture of what the cluster would have had, and what the scripts said.
#define EXPECT_RUN_CLEAN(run)                                                                      \
    SCOPED_TRACE(::testing::Message() << "the cluster would have:" << describe(run)                \
                                      << "\nscript errors:" << joined((run).errors));              \
    ASSERT_TRUE((run).started.succeeded()) << std::string{(run).started.message()};                \
    EXPECT_TRUE((run).errors.empty())

[[nodiscard]] bool lit(const Observed& run, ClusterRole role)
{
    return of(run, role) > 0.5;
}

[[nodiscard]] bool dark(const Observed& run, ClusterRole role)
{
    return has(run, role) && of(run, role) < 0.5;
}

} // namespace

TEST(J1939ExampleProjectTests, TheProjectStartsRunsAndFeedsTheCluster)
{
    const Observed run = runProject(kRun);
    EXPECT_RUN_CLEAN(run);

    ASSERT_TRUE(run.profile != nullptr);

    // Every role the profile has a source for must have something to read, because a role that has
    // nothing is a gauge that never moves with no line anywhere to say why.
    for (const ClusterSource& source : run.profile->sources) {
        SCOPED_TRACE(std::string{nameOf(source.role)});
        EXPECT_TRUE(has(run, source.role)) << "no value for this role";
    }
}

TEST(J1939ExampleProjectTests, ANormalVehicleShowsNormalInstrumentsAndNoWarning)
{
    // The default scenario, a couple of seconds in: the drive cycle opens with four seconds at
    // standstill. What matters is what is NOT there - no lamp, no alarm - as much as what is.
    const Observed run = runProject(kRun);
    EXPECT_RUN_CLEAN(run);

    EXPECT_NEAR(of(run, ClusterRole::Speed), 0.0, 1.0);
    EXPECT_NEAR(of(run, ClusterRole::Rpm), 750.0, 50.0);
    EXPECT_NEAR(of(run, ClusterRole::Gear), 0.0, 0.1); // neutral
    EXPECT_GT(of(run, ClusterRole::Coolant), 80.0);
    EXPECT_LT(of(run, ClusterRole::Coolant), 100.0);
    EXPECT_GT(of(run, ClusterRole::Oil), 150.0);
    EXPECT_NEAR(of(run, ClusterRole::Fuel), 78.0, 2.0);
    EXPECT_NEAR(of(run, ClusterRole::Def), 68.0, 2.0);
    EXPECT_NEAR(of(run, ClusterRole::Battery), 12.6, 0.2);
    EXPECT_NEAR(of(run, ClusterRole::AirPressure), 8.5, 0.2);
    EXPECT_NEAR(of(run, ClusterRole::Ambient), 22.0, 0.5);
    EXPECT_NEAR(of(run, ClusterRole::Odometer), 14852.0, 1.0);
    EXPECT_NEAR(of(run, ClusterRole::Hours), 124.5, 0.5);

    // The AI panel of a healthy vehicle is calm. The model was set against a vehicle whose coolant
    // idles at 70 degC and a J1939 engine idles at 88, and fed those numbers as they are it called
    // every healthy truck a thermal emergency from the first second.
    EXPECT_LT(of(run, ClusterRole::AiRegime), 3.0);
    EXPECT_LT(of(run, ClusterRole::AiAnomaly), 50.0);
    EXPECT_GT(of(run, ClusterRole::AiHealth), 50.0);
    EXPECT_GT(of(run, ClusterRole::AiConfidence), 0.0);

    for (const ClusterRole role : {ClusterRole::Cruise,
                                   ClusterRole::LampPark,
                                   ClusterRole::LampBelt,
                                   ClusterRole::LampAbs,
                                   ClusterRole::LampHigh,
                                   ClusterRole::LampLeft,
                                   ClusterRole::LampRight,
                                   ClusterRole::LampHazard,
                                   ClusterRole::DmStop,
                                   ClusterRole::DmWarn,
                                   ClusterRole::DmMil,
                                   ClusterRole::DmProtect}) {
        SCOPED_TRACE(std::string{nameOf(role)});
        EXPECT_TRUE(dark(run, role));
    }

    // Headlamps, by default: the low beam and the position lights.
    EXPECT_TRUE(lit(run, ClusterRole::LampLow));
    EXPECT_TRUE(lit(run, ClusterRole::LampPosition));
}

TEST(J1939ExampleProjectTests, AVehicleOnTheHighwayDrivesWithCruiseControlOn)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_engine", "highway");
        setParameter(pipeline, "ecu_engine", "initial_speed", ParameterValue::fromReal(100.0));
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_NEAR(of(run, ClusterRole::Speed), 100.0, 3.0);
    EXPECT_GT(of(run, ClusterRole::Rpm), 1200.0);
    EXPECT_LT(of(run, ClusterRole::Rpm), 2800.0);
    EXPECT_GE(of(run, ClusterRole::Gear), 3.0);
    EXPECT_TRUE(lit(run, ClusterRole::Cruise));

    // And the model, which calls a vehicle at 100 km/h with a warm engine cruising, is not alarmed.
    EXPECT_LT(of(run, ClusterRole::AiRegime), 4.0);
    EXPECT_GT(of(run, ClusterRole::AiHealth), 50.0);
    EXPECT_TRUE(dark(run, ClusterRole::DmStop));
    EXPECT_TRUE(dark(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, TheParkingBrakeLightsItsLamp)
{
    const Observed run = runProject(
        kRun, [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_engine", "parked"); });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampPark));
    EXPECT_NEAR(of(run, ClusterRole::Speed), 0.0, 1.0);
}

TEST(J1939ExampleProjectTests, LowOilPressureLightsTheStopLampFromTheEnginesDm1)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_engine", "low_oil_pressure");
    });
    EXPECT_RUN_CLEAN(run);

    // Five other ECUs send a DM1 a second with every lamp dark, and the lamp is still lit: the
    // lamps of the bus are the OR of the ECUs, not whichever message came last.
    EXPECT_TRUE(lit(run, ClusterRole::DmStop));
    EXPECT_LT(of(run, ClusterRole::Oil), 100.0);
}

TEST(J1939ExampleProjectTests, AnOverheatingEngineLightsTheStopAndEngineLampsAndAlarmsTheModel)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_engine", "overheating");
        // Where it would get to in half a minute, so that a test does not have to wait for it.
        setParameter(
            pipeline, "ecu_engine", "initial_coolant_temp", ParameterValue::fromReal(112.0));
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_GT(of(run, ClusterRole::Coolant), 105.0);
    EXPECT_TRUE(lit(run, ClusterRole::DmMil));
    EXPECT_TRUE(lit(run, ClusterRole::DmStop));

    // The model must notice what the engine's own ECU already has: a calibration that quiets the
    // healthy vehicle by deafening the model would pass the test above and fail this one.
    EXPECT_GE(of(run, ClusterRole::AiRegime), 3.0);
    EXPECT_GT(of(run, ClusterRole::AiAnomaly), 50.0);
}

TEST(J1939ExampleProjectTests, ACoolantSensorThatFailsIsNoDataAndAnAmberLamp)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_engine", "sensor_error");
    });
    EXPECT_RUN_CLEAN(run);

    // "Not available" is NaN, which the cluster draws as dashes - and not 215 degC.
    EXPECT_FALSE(has(run, ClusterRole::Coolant));
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAlternatorThatStopsChargingLightsTheWarningLamp)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_body", "alternator_failure");
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_LT(of(run, ClusterRole::Battery), 11.8);
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnEmptyAdBlueTankLightsTheWarningLamp)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_aftertreatment", "low_adblue");
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_NEAR(of(run, ClusterRole::Def), 8.0, 1.0);
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAbsFaultLightsTheAbsLampAndTheWarningLamp)
{
    const Observed run = runProject(
        kRun, [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_brakes", "abs_fault"); });
    EXPECT_RUN_CLEAN(run);

    // The ABS lamp is the amber warning signal, not "the ABS is regulating", which is a thing that
    // happens under hard braking on a wet road and no lamp is for.
    EXPECT_TRUE(lit(run, ClusterRole::LampAbs));
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAirLeakLightsTheStopLamp)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_brakes", "air_leak");
        setParameter(pipeline, "ecu_brakes", "initial_circuit1_air", ParameterValue::fromReal(4.0));
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_LT(of(run, ClusterRole::AirPressure), 4.5);
    EXPECT_TRUE(lit(run, ClusterRole::DmStop));
}

TEST(J1939ExampleProjectTests, TheTurnStalkOnThePanelLightsTheArrowOnTheCluster)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_switches", "turn_left");
    });
    EXPECT_RUN_CLEAN(run);

    // Panel -> bus -> body controller -> bus -> cluster: two ECUs that have to hear each other.
    EXPECT_TRUE(lit(run, ClusterRole::LampLeft));
    EXPECT_TRUE(dark(run, ClusterRole::LampRight));
    EXPECT_TRUE(dark(run, ClusterRole::LampHazard));
}

TEST(J1939ExampleProjectTests, TheHazardButtonOnThePanelLightsBothArrowsAndItsOwnLamp)
{
    const Observed run = runProject(
        kRun, [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_switches", "hazard"); });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampHazard));
    EXPECT_TRUE(lit(run, ClusterRole::LampLeft));
    EXPECT_TRUE(lit(run, ClusterRole::LampRight));
}

TEST(J1939ExampleProjectTests, TheLightSwitchAndTheHighBeamReachTheCluster)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setParameter(pipeline, "ecu_switches", "high_beam", ParameterValue::fromInteger(1));
    });
    EXPECT_RUN_CLEAN(run);
    EXPECT_TRUE(lit(run, ClusterRole::LampHigh));
    EXPECT_TRUE(lit(run, ClusterRole::LampLow));

    const Observed off = runProject(kRun, [](GraphDescription& pipeline) {
        setParameter(pipeline, "ecu_switches", "lighting", ParameterValue::fromInteger(0));
    });
    EXPECT_RUN_CLEAN(off);
    EXPECT_TRUE(dark(off, ClusterRole::LampLow));
    EXPECT_TRUE(dark(off, ClusterRole::LampPosition));
    EXPECT_TRUE(dark(off, ClusterRole::LampHigh));
}

TEST(J1939ExampleProjectTests, AnUnfastenedSeatBeltLightsItsLamp)
{
    const Observed run = runProject(kRun, [](GraphDescription& pipeline) {
        setScenario(pipeline, "ecu_body", "belt_unbuckled");
    });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampBelt));
}

TEST(J1939ExampleProjectTests, NoSignalOfANormalRunIsReportedAsNotAvailableByMistake)
{
    const Observed run = runProject(kRun);
    EXPECT_RUN_CLEAN(run);

    // The J1939 block turns "error" and "not available" into NaN. A script that writes one of those
    // into a field its database describes - 0xCF into a byte whose low bits are a cruise control
    // that should read 0 was the one that found this - is a signal that is not available for the
    // whole run, and nothing says so: the cluster draws dashes for it as it would for a bus without
    // it.
    //
    // So in the normal run, a signal the database describes is either a number or on this list, and
    // the list is the things a script deliberately does not report.
    const std::set<std::string> deliberate{
        "DD1.FuelLevel2", // one tank
        "DM1.FlashProtectLamp", // no lamp flashes, which is what a lamp's flash byte says with 0xFF
        "DM1.FlashAmberWarningLamp",
        "DM1.FlashRedStopLamp",
        "DM1.FlashMalfunctionIndicatorLamp",
    };

    std::vector<std::string> missing;
    for (const auto& [name, value] : run.series) {
        if (std::isnan(value) && deliberate.count(name) == 0) {
            missing.push_back(name);
        }
    }

    SCOPED_TRACE(::testing::Message() << "not available:" << joined(missing));
    EXPECT_TRUE(missing.empty());
}
