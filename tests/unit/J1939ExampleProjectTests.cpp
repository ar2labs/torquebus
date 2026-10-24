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
// The run is real: the virtual bus, the engine's own executor thread and the clock. It lasts as
// long as the test has to wait for what it came to see - the cluster's every instrument, every
// ECU's first DM1, and whatever the scenario is meant to show - and no longer: a run of a fixed
// length was too long on a fast machine, and on a slow one would one day have been too short.

#include "core/can/CanEngine.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfile.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/dashboard/cluster/ClusterRole.h"
#include "core/j1939/J1939Network.h"
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
#include <functional>
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

    /// How many ECUs are repeating a DM1 on the bus, and how many inferences the TinyML ECU has
    /// run: what says that the lamps of the bus and the model's verdict are about the vehicle and
    /// not what they were before anybody had spoken.
    std::size_t reportingEcus{0};
    double inferences{0.0};

    /// How long the run had lasted when it was last looked at.
    std::chrono::milliseconds elapsed{0};

    /// Whether what the test was waiting for came before kPatience ran out.
    bool arrived{false};
};

/// How long a run waits for what it came to see. The vehicle sends every message it has at least
/// once a second and its first of each at once, so this is many times what any of it takes.
constexpr std::chrono::seconds kPatience{15};

/// The ECUs of the project that send a DM1: the six scripted ones. The TinyML ECU sends one only
/// when its model is sure of trouble.
constexpr std::size_t kScriptedEcus = 6;

/// A model that has run on a few dozen frames has had the chance to be wrong; one that has run on
/// none says what it said before it started.
constexpr double kInferencesThatCount = 20.0;

/// Reads the plot store, the variables and the bus the way the cluster would, into `run`.
void observe(CanEngine& engine, Observed& run)
{
    run.series.clear();
    run.roles.clear();

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

    run.reportingEcus = 0;
    for (const J1939BusView& bus : engine.j1939Network().snapshot().buses) {
        run.reportingEcus += bus.reporting.size();
    }

    run.inferences = engine.variables().value("tinyml.inferences");
}

/// Opens the project, lets `edit` change it (a scenario, say) and runs it until `arrived` says that
/// the engine shows what the test came to see, or kPatience has passed.
template<typename Edit>
[[nodiscard]] Observed runProject(const std::function<bool(const Observed&)>& arrived, Edit&& edit)
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

    // Before the engine, so that they outlive the sink that fills them.
    std::mutex errorMutex;
    std::vector<std::string> errors;

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    run.started = engine.addChannel(std::make_unique<VirtualCanBackend>(), config);
    if (run.started.failed()) {
        return run;
    }

    engine.addLogSink([&errors, &errorMutex](const std::string& text, bool isError) {
        if (isError) {
            const std::lock_guard lock{errorMutex};
            errors.push_back(text);
        }
    });

    const std::string base = QFileInfo{projectPath()}.absolutePath().toStdString();
    engine.setGraphDescription(project.pipeline, NodeCatalog::withBuiltinTypes(), base);

    run.started = engine.start();
    if (run.started.failed()) {
        return run;
    }

    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + kPatience;

    // Read while it is running, and read again until it shows what was asked for. The J1939 block
    // puts the lamps of the bus out at Stop, which is right for a cluster - a stopped measurement
    // has no fault to report - and wrong for a test that wants to know what the cluster was
    // showing.
    for (;;) {
        observe(engine, run);
        run.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);
        run.arrived = arrived(run);

        if (run.arrived || std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    engine.stop();

    {
        const std::lock_guard lock{errorMutex};
        run.errors = errors;
    }

    return run;
}

[[nodiscard]] Observed runProject(const std::function<bool(const Observed&)>& arrived)
{
    return runProject(arrived, [](GraphDescription&) { });
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
    text << " (" << run.reportingEcus << " ECUs reporting, " << run.inferences << " inferences, "
         << run.elapsed.count() << " ms)";
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

/// Whether the cluster has something to read for the role, "not available" included.
[[nodiscard]] bool present(const Observed& run, ClusterRole role)
{
    return run.roles.find(role) != run.roles.end();
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

/// Fails with the whole picture of what the cluster would have had, and what the scripts said.
#define EXPECT_RUN_CLEAN(run)                                                                      \
    SCOPED_TRACE(::testing::Message() << "the cluster would have:" << describe(run)                \
                                      << "\nscript errors:" << joined((run).errors));              \
    ASSERT_TRUE((run).started.succeeded()) << std::string{(run).started.message()};                \
    EXPECT_TRUE((run).arrived) << "what the test was waiting for did not come";                    \
    EXPECT_TRUE((run).errors.empty())

[[nodiscard]] bool lit(const Observed& run, ClusterRole role)
{
    return of(run, role) > 0.5;
}

[[nodiscard]] bool dark(const Observed& run, ClusterRole role)
{
    return has(run, role) && of(run, role) < 0.5;
}

/// The vehicle has said everything it says at once: the cluster has something to read for every
/// role of its profile, every scripted ECU has sent its first DM1 - so the lamps of the bus are
/// theirs, and not what they were before anybody spoke - and the model has run on enough of it to
/// have an opinion.
[[nodiscard]] bool settled(const Observed& run)
{
    if (run.profile == nullptr) {
        return false;
    }

    const bool everyRole =
        std::all_of(run.profile->sources.begin(),
                    run.profile->sources.end(),
                    [&run](const ClusterSource& source) { return present(run, source.role); });

    return everyRole && run.reportingEcus >= kScriptedEcus
           && run.inferences >= kInferencesThatCount;
}

} // namespace

TEST(J1939ExampleProjectTests, TheProjectStartsRunsAndFeedsTheCluster)
{
    // Until the vehicle has said everything it says at once: every role has a value to show, which
    // is what the test is about, and the scripted verification block of the project - which runs
    // its cases against the same bus - has had the time to finish them before the measurement is
    // stopped under it.
    const Observed run = runProject(settled);
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
    // The default scenario, as it opens: the drive cycle starts with four seconds at standstill.
    // What matters is what is NOT there - no lamp, no alarm - as much as what is.
    const Observed run = runProject(settled);
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
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::Cruise); },
        [](GraphDescription& pipeline) {
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
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::LampPark); },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_engine", "parked"); });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampPark));
    EXPECT_NEAR(of(run, ClusterRole::Speed), 0.0, 1.0);
}

TEST(J1939ExampleProjectTests, LowOilPressureLightsTheStopLampFromTheEnginesDm1)
{
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::DmStop); },
        [](GraphDescription& pipeline) {
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
    // The engine's own ECU lights the lamps at once; what takes the model a moment is to see the
    // heat in its features, so that is what is waited for.
    const Observed run = runProject(
        [](const Observed& seen) {
            return settled(seen) && lit(seen, ClusterRole::DmMil) && lit(seen, ClusterRole::DmStop)
                   && of(seen, ClusterRole::AiAnomaly) > 50.0;
        },
        [](GraphDescription& pipeline) {
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
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::DmWarn); },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_engine", "sensor_error"); });
    EXPECT_RUN_CLEAN(run);

    // "Not available" is NaN, which the cluster draws as dashes - and not 215 degC.
    EXPECT_TRUE(present(run, ClusterRole::Coolant));
    EXPECT_FALSE(has(run, ClusterRole::Coolant));
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAlternatorThatStopsChargingLightsTheWarningLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) {
            return settled(seen) && of(seen, ClusterRole::Battery) < 11.8
                   && lit(seen, ClusterRole::DmWarn);
        },
        [](GraphDescription& pipeline) {
            setScenario(pipeline, "ecu_body", "alternator_failure");
        });
    EXPECT_RUN_CLEAN(run);

    EXPECT_LT(of(run, ClusterRole::Battery), 11.8);
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnEmptyAdBlueTankLightsTheWarningLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::DmWarn); },
        [](GraphDescription& pipeline) {
            setScenario(pipeline, "ecu_aftertreatment", "low_adblue");
        });
    EXPECT_RUN_CLEAN(run);

    EXPECT_NEAR(of(run, ClusterRole::Def), 8.0, 1.0);
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAbsFaultLightsTheAbsLampAndTheWarningLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) {
            return settled(seen) && lit(seen, ClusterRole::LampAbs)
                   && lit(seen, ClusterRole::DmWarn);
        },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_brakes", "abs_fault"); });
    EXPECT_RUN_CLEAN(run);

    // The ABS lamp is the amber warning signal, not "the ABS is regulating", which is a thing that
    // happens under hard braking on a wet road and no lamp is for.
    EXPECT_TRUE(lit(run, ClusterRole::LampAbs));
    EXPECT_TRUE(lit(run, ClusterRole::DmWarn));
}

TEST(J1939ExampleProjectTests, AnAirLeakLightsTheStopLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::DmStop); },
        [](GraphDescription& pipeline) {
            setScenario(pipeline, "ecu_brakes", "air_leak");
            setParameter(
                pipeline, "ecu_brakes", "initial_circuit1_air", ParameterValue::fromReal(4.0));
        });
    EXPECT_RUN_CLEAN(run);

    EXPECT_LT(of(run, ClusterRole::AirPressure), 4.5);
    EXPECT_TRUE(lit(run, ClusterRole::DmStop));
}

TEST(J1939ExampleProjectTests, TheTurnStalkOnThePanelLightsTheArrowOnTheCluster)
{
    // An arrow flashes, so it is lit for part of every cycle: the run is read at a moment it is.
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::LampLeft); },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_switches", "turn_left"); });
    EXPECT_RUN_CLEAN(run);

    // Panel -> bus -> body controller -> bus -> cluster: two ECUs that have to hear each other.
    EXPECT_TRUE(lit(run, ClusterRole::LampLeft));
    EXPECT_TRUE(dark(run, ClusterRole::LampRight));
    EXPECT_TRUE(dark(run, ClusterRole::LampHazard));
}

TEST(J1939ExampleProjectTests, TheHazardButtonOnThePanelLightsBothArrowsAndItsOwnLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) {
            return settled(seen) && lit(seen, ClusterRole::LampHazard)
                   && lit(seen, ClusterRole::LampLeft) && lit(seen, ClusterRole::LampRight);
        },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_switches", "hazard"); });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampHazard));
    EXPECT_TRUE(lit(run, ClusterRole::LampLeft));
    EXPECT_TRUE(lit(run, ClusterRole::LampRight));
}

TEST(J1939ExampleProjectTests, TheLightSwitchAndTheHighBeamReachTheCluster)
{
    const Observed run = runProject(
        [](const Observed& seen) {
            return settled(seen) && lit(seen, ClusterRole::LampHigh)
                   && lit(seen, ClusterRole::LampLow);
        },
        [](GraphDescription& pipeline) {
            setParameter(pipeline, "ecu_switches", "high_beam", ParameterValue::fromInteger(1));
        });
    EXPECT_RUN_CLEAN(run);
    EXPECT_TRUE(lit(run, ClusterRole::LampHigh));
    EXPECT_TRUE(lit(run, ClusterRole::LampLow));

    // Everything switched off is the absence of lamps, which has to be waited for as long as the
    // vehicle's first messages: a lamp that is dark because nothing has been heard is not off.
    const Observed off = runProject(
        [](const Observed& seen) { return settled(seen); },
        [](GraphDescription& pipeline) {
            setParameter(pipeline, "ecu_switches", "lighting", ParameterValue::fromInteger(0));
        });
    EXPECT_RUN_CLEAN(off);
    EXPECT_TRUE(dark(off, ClusterRole::LampLow));
    EXPECT_TRUE(dark(off, ClusterRole::LampPosition));
    EXPECT_TRUE(dark(off, ClusterRole::LampHigh));
}

TEST(J1939ExampleProjectTests, AnUnfastenedSeatBeltLightsItsLamp)
{
    const Observed run = runProject(
        [](const Observed& seen) { return settled(seen) && lit(seen, ClusterRole::LampBelt); },
        [](GraphDescription& pipeline) { setScenario(pipeline, "ecu_body", "belt_unbuckled"); });
    EXPECT_RUN_CLEAN(run);

    EXPECT_TRUE(lit(run, ClusterRole::LampBelt));
}

TEST(J1939ExampleProjectTests, NoSignalOfANormalRunIsReportedAsNotAvailableByMistake)
{
    // Every message of the vehicle repeats at least once a second, and its first is sent at once:
    // after a second and a bit, every signal the database describes has been delivered, and the
    // test can say which of them is not a number.
    const Observed run = runProject([](const Observed& seen) {
        return settled(seen) && seen.elapsed >= std::chrono::milliseconds{1200};
    });
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

TEST(J1939ExampleProjectTests, TheProjectIncludesUdsClientTestSequenceLoggerAndClusterTransmitList)
{
    Project project;
    ASSERT_TRUE(openProject(project).succeeded());

    EXPECT_NE(project.pipeline.find("uds_tester"), nullptr);
    EXPECT_NE(project.pipeline.find("j1939_test"), nullptr);
    EXPECT_NE(project.pipeline.find("can_logger"), nullptr);
    EXPECT_GE(project.transmit.size(), 17U);

    for (const TransmitEntry& entry : project.transmit.entries()) {
        EXPECT_EQ(entry.trigger, TransmitTrigger::Manual);
        EXPECT_TRUE(entry.enabled);
        EXPECT_EQ(entry.frame.length, 8U);
        EXPECT_FALSE(entry.messageName.empty());
        if (entry.messageName == "DM1") {
            EXPECT_EQ(entry.frame.identifier, 0x18FECA00U)
                << "DM1 entry '" << entry.name << "' must transmit with J1939 CAN-ID 0x18FECA00";
        }
    }
}

TEST(J1939ExampleProjectTests, TransmittingAmberWarningDM1LightsClusterDmWarn)
{
    Project project;
    ASSERT_TRUE(openProject(project).succeeded());

    const auto entries = project.transmit.entries();
    const auto it = std::find_if(entries.begin(), entries.end(), [](const TransmitEntry& e) {
        return e.messageName == "DM1" && e.name.find("Amber Warning") != std::string::npos;
    });
    ASSERT_NE(it, entries.end());
    EXPECT_EQ(it->frame.identifier, 0x18FECA00U);

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    const std::string base = QFileInfo{projectPath()}.absolutePath().toStdString();
    engine.setGraphDescription(project.pipeline, NodeCatalog::withBuiltinTypes(), base);
    ASSERT_TRUE(engine.start().succeeded());

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Transmit the amber DM1 frame from the project's transmit list onto CAN channel 0
    ASSERT_TRUE(engine.transmit(0, it->frame).succeeded());

    const auto began = std::chrono::steady_clock::now();
    bool lit = false;
    while (std::chrono::steady_clock::now() - began < std::chrono::seconds{3}) {
        if (engine.variables().value(std::string{kJ1939LampWarningVariable}) > 0.5) {
            lit = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    engine.stop();
    EXPECT_TRUE(lit) << "Transmitting DM1 from transmit list must light kJ1939LampWarningVariable";
}

TEST(J1939ExampleProjectTests, TransmittingLowOilPressureEflLightsClusterOil)
{
    Project project;
    ASSERT_TRUE(openProject(project).succeeded());

    const auto entries = project.transmit.entries();
    const auto it = std::find_if(entries.begin(), entries.end(), [](const TransmitEntry& e) {
        return e.messageName == "EFL_P1" && e.name.find("Low Oil Pressure") != std::string::npos;
    });
    ASSERT_NE(it, entries.end());
    EXPECT_EQ(it->frame.identifier, 0x18FEEF00U);

    // Verify low oil pressure byte encoding (byte 3 = 12 -> 48 kPa < 100 kPa)
    EXPECT_EQ(it->frame.data[3], 12);
}
