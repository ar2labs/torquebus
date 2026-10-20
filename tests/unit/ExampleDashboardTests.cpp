// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Dashboards of the two example projects, opened and run the way a person opens them.
//
// The Dashboard is the first thing on the screen when an example is opened, and what it can get
// wrong is what nothing else in the repository would notice:
//
//   * a widget that names a signal its own database spells differently, which shows dashes and says
//     nothing;
//   * a picture that was renamed or never committed, which shows a box that says it is missing;
//   * a signal the database has and the pipeline never carries to the plot store, which is a widget
//     that is correct and shows nothing for ever.
//
// The first two are checked against the files. The third is checked by running the project on the
// virtual bus, which is the only way to know what a pipeline delivers.

#include "core/can/CanEngine.h"
#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/DashboardFaults.h"
#include "core/dashboard/SignalDictionary.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/j1939/J1939ClaimRequest.h"
#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939Id.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/plot/SignalSeries.h"
#include "core/transmit/TransmitList.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "services/ProjectFile.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace torquebus;
using namespace torquebus::services;

namespace {

constexpr const char* kExamples[] = {"j1939-vehicle.tbsproj", "virtual-vehicle.tbsproj"};

/// An example project and what its databases say.
struct Example final {
    QString path;

    GraphDescription pipeline;
    TransmitList transmit;
    DashboardDescription dashboard;
    SignalDictionary dictionary;

    [[nodiscard]] std::string folder() const
    {
        return QFileInfo{path}.absolutePath().toStdString();
    }
};

[[nodiscard]] bool open(const char* file, Example& example)
{
    example.path = QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/") + QString::fromLatin1(file);

    QStringList names;
    const Result opened = ProjectFile::load(
        example.path, example.pipeline, example.transmit, example.dashboard, &names);

    EXPECT_TRUE(opened.succeeded()) << std::string{opened.message()};

    if (opened.failed()) {
        return false;
    }

    std::vector<std::shared_ptr<const CanDatabase>> databases;

    for (const QString& name : names) {
        auto database = std::make_shared<CanDatabase>();
        const QString databaseFile = QFileInfo{example.path}.absoluteDir().filePath(name);

        const Result parsed = DbcParser::parseFile(databaseFile.toStdString(), *database);
        EXPECT_TRUE(parsed.succeeded())
            << databaseFile.toStdString() << ": " << std::string{parsed.message()};

        database->sourcePath = databaseFile.toStdString();
        databases.push_back(std::move(database));
    }

    example.dictionary.rebuild(databases);

    return !example.dictionary.empty();
}

/// What a run of the project left in the plot store.
struct Delivered final {
    /// "Message.Signal" -> its newest value, which may be NaN: "not available" is a value.
    std::map<std::string, double> series;

    Result started{Result::ok()};

    /// Whether what the run was waiting for came before kPatience ran out.
    bool arrived{false};
};

/// How long a run waits for what it came to see. The ECUs of the examples repeat every message at
/// least once a second, so this is many times what any of it takes: a slow machine waits longer and
/// a fast one hardly at all - where a run of a fixed length was too long on one and, one day, would
/// have been too short on the other.
constexpr std::chrono::seconds kPatience{15};

/// What the plot store holds now: "Message.Signal" -> its newest value.
[[nodiscard]] std::map<std::string, double> seriesOf(CanEngine& engine)
{
    std::map<std::string, double> series;

    for (const SeriesInfo& info : engine.plotStore().listSeries()) {
        double value = 0.0;
        std::uint64_t when = 0;

        if (engine.plotStore().latest(info.id, value, when)) {
            series[info.name] = value;
        }
    }

    return series;
}

/// Runs the project on the virtual bus until `arrived` says the engine shows what the test came to
/// see, or kPatience has passed: as many channels as its CAN blocks name. `whileRunning`, when
/// there is one, is called with the engine then and before it is stopped - which puts some lamps
/// out and empties the list of faults, so it is the moment to look. `afterStart` is called as soon
/// as the engine is running, for what a person would do to it then; `beforeStart`, for what has to
/// be in place before it runs, such as a sink that must see the first frame.
[[nodiscard]] Delivered run(const Example& example,
                            const std::function<bool(CanEngine&)>& arrived,
                            const std::function<void(CanEngine&)>& whileRunning = {},
                            const std::function<void(CanEngine&)>& afterStart = {},
                            const std::function<void(CanEngine&)>& beforeStart = {})
{
    Delivered delivered;

    std::int64_t highest = 0;

    for (const NodeDescription& node : example.pipeline.nodes()) {
        if (node.typeName == "can.bus") {
            highest = std::max(highest, node.parameters.integer("channel", 0));
        }
    }

    CanEngine engine;

    for (std::int64_t channel = 0; channel <= highest; ++channel) {
        CanChannelConfig config;
        config.deviceHandle = "virtual:" + std::to_string(channel);
        config.applicationChannel = static_cast<std::uint8_t>(channel);
        config.timing.bitrate = 500'000;

        delivered.started = engine.addChannel(std::make_unique<VirtualCanBackend>(), config);

        if (delivered.started.failed()) {
            return delivered;
        }
    }

    engine.setGraphDescription(example.pipeline, NodeCatalog::withBuiltinTypes(), example.folder());

    if (beforeStart) {
        beforeStart(engine);
    }

    delivered.started = engine.start();

    if (delivered.started.failed()) {
        return delivered;
    }

    if (afterStart) {
        afterStart(engine);
    }

    const auto deadline = std::chrono::steady_clock::now() + kPatience;

    for (;;) {
        delivered.arrived = arrived(engine);

        if (delivered.arrived || std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    // Read while it is running: stopping puts some lamps out.
    delivered.series = seriesOf(engine);

    if (whileRunning) {
        whileRunning(engine);
    }

    engine.stop();

    return delivered;
}

/// Sets the scenario of one block of the example's pipeline, the way the Block panel would.
void setScenario(Example& example, const std::string& block, const char* scenario)
{
    for (NodeDescription& node : example.pipeline.nodes()) {
        if (node.id == block) {
            node.parameters.set("scenario", ParameterValue::fromText(scenario));
            return;
        }
    }

    ADD_FAILURE() << "the example has no block called " << block;
}

/// The table of function names that ships with the program, from the source tree - the file that
/// goes into the package, and not a copy beside the executable.
[[nodiscard]] std::filesystem::path shippedFunctionTable()
{
    return std::filesystem::path{TORQUEBUS_DATA_DIR} / "j1939-names-functions.csv";
}

[[nodiscard]] const DashboardWidget* faultListOf(const Example& example)
{
    for (const DashboardWidget& widget : example.dashboard.widgets()) {
        if (widget.kind == DashboardWidgetKind::Faults) {
            return &widget;
        }
    }

    return nullptr;
}

/// The bus the J1939 example's one J1939 block reads, or an empty one when it published nothing.
[[nodiscard]] const J1939BusView& busOf(const J1939NetworkSnapshot& snapshot)
{
    static const J1939BusView kNothingPublished{};

    EXPECT_LE(snapshot.buses.size(), 1U) << "the example has one J1939 block, on one bus";
    return snapshot.buses.empty() ? kNothingPublished : snapshot.buses.front();
}

/// Whether every widget of the example that reads the plot store has something in it to show: its
/// signal, with a value, or for a table at least one signal of its message.
[[nodiscard]] bool everyWidgetHasItsSignals(const Example& example,
                                            const std::map<std::string, double>& series)
{
    for (const DashboardWidget& widget : example.dashboard.widgets()) {
        if (widget.binding.source == DashboardBinding::Source::Signal) {
            const auto found = series.find(widget.binding.message + "." + widget.binding.signal);

            if (found == series.end() || !std::isfinite(found->second)) {
                return false;
            }
        } else if (widget.binding.source == DashboardBinding::Source::Message) {
            bool any = false;

            for (const DictionaryEntry* entry :
                 example.dictionary.signalsOf(widget.binding.message)) {
                const auto found = series.find(entry->qualified());
                any = any || (found != series.end() && std::isfinite(found->second));
            }

            if (!any) {
                return false;
            }
        }
    }

    return true;
}

/// Whether the J1939 example's bus is reporting `spn` from `address` now.
[[nodiscard]] bool
isReporting(const J1939NetworkSnapshot& snapshot, std::uint8_t address, std::uint32_t spn)
{
    const J1939BusView& bus = busOf(snapshot);

    return std::any_of(
        bus.activeFaults.begin(), bus.activeFaults.end(), [&](const J1939ActiveFault& fault) {
            return fault.sourceAddress == address && fault.code.spn == spn;
        });
}

/// Whether the ECU at `address` has been seen claiming it, with a NAME.
[[nodiscard]] bool hasClaimed(const J1939NetworkSnapshot& snapshot, int address)
{
    const J1939BusView& bus = busOf(snapshot);

    return std::any_of(bus.nodes.begin(), bus.nodes.end(), [address](const J1939NetworkNode& node) {
        return node.address == address && node.name.has_value();
    });
}

} // namespace

TEST(ExampleDashboardTests, EveryWidgetOfAnExampleNamesWhatItsDatabasesHave)
{
    for (const char* file : kExamples) {
        SCOPED_TRACE(file);

        Example example;
        ASSERT_TRUE(open(file, example));

        const Result valid = example.dashboard.validate();
        EXPECT_TRUE(valid.succeeded()) << std::string{valid.message()};

        for (const WidgetFinding& finding : example.dictionary.audit(example.dashboard)) {
            ADD_FAILURE() << "widget '" << finding.widgetId << "' names "
                          << example.dashboard.find(finding.widgetId)->binding.message << "."
                          << example.dashboard.find(finding.widgetId)->binding.signal
                          << ", which the databases do not have; did you mean "
                          << finding.finding.suggestedMessage << "."
                          << finding.finding.suggestedSignal << "?";
        }
    }
}

TEST(ExampleDashboardTests, EachExampleShowsMoreThanTheClusterAndEveryKindOfObjectIsInOne)
{
    std::set<DashboardWidgetKind> kinds;

    for (const char* file : kExamples) {
        SCOPED_TRACE(file);

        Example example;
        ASSERT_TRUE(open(file, example));

        std::set<DashboardWidgetKind> here;

        for (const DashboardWidget& widget : example.dashboard.widgets()) {
            here.insert(widget.kind);
        }

        // The point of an example is what it demonstrates, and this one has to demonstrate that a
        // Dashboard is more than its instrument cluster.
        EXPECT_TRUE(here.contains(DashboardWidgetKind::Cluster));

        for (const DashboardWidgetKind kind : {DashboardWidgetKind::Bar,
                                               DashboardWidgetKind::State,
                                               DashboardWidgetKind::Image,
                                               DashboardWidgetKind::Trend,
                                               DashboardWidgetKind::Table,
                                               DashboardWidgetKind::Group}) {
            EXPECT_TRUE(here.contains(kind)) << nameOf(kind);
        }

        kinds.insert(here.begin(), here.end());
    }

    EXPECT_GE(kinds.size(), 8U);
}

TEST(ExampleDashboardTests, EveryPictureOfAnExampleIsWhereTheProjectSaysItIsAndNamesAnImage)
{
    for (const char* file : kExamples) {
        SCOPED_TRACE(file);

        Example example;
        ASSERT_TRUE(open(file, example));

        const std::vector<std::string> used = assetsUsedBy(example.dashboard);
        EXPECT_FALSE(used.empty());

        for (const std::string& image : used) {
            SCOPED_TRACE(image);

            EXPECT_TRUE(isImagePath(image));

            // Written relative to the project: the example is opened on any machine.
            EXPECT_FALSE(QFileInfo{QString::fromStdString(image)}.isAbsolute());
        }

        for (const MissingAsset& missing : missingAssets(example.dashboard, example.folder())) {
            ADD_FAILURE() << "widget '" << missing.widgetId
                          << "' names a picture that is not there: " << missing.image;
        }

        // A picture bound to a value has one for every name the value has, or the state that has
        // none shows the widget's own - which for these examples is nothing at all.
        for (const DashboardWidget& widget : example.dashboard.widgets()) {
            if (widget.kind != DashboardWidgetKind::Image || widget.states.empty()) {
                continue;
            }

            for (const DashboardState& state : widget.states) {
                EXPECT_FALSE(state.image.empty()) << widget.id << ": " << state.label;
            }
        }
    }
}

TEST(ExampleDashboardTests, EverySignalAnExampleDashboardReadsIsDeliveredByItsPipeline)
{
    for (const char* file : kExamples) {
        SCOPED_TRACE(file);

        Example example;
        ASSERT_TRUE(open(file, example));

        // Until every widget has something to show, which is the answer - or until the patience
        // runs out, and the loop below says which widget is still waiting.
        const Delivered delivered = run(example, [&example](CanEngine& engine) {
            return everyWidgetHasItsSignals(example, seriesOf(engine));
        });

        ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
        EXPECT_TRUE(delivered.arrived)
            << "not every widget had its signal after " << kPatience.count() << " s";

        for (const DashboardWidget& widget : example.dashboard.widgets()) {
            SCOPED_TRACE(widget.id);

            if (widget.binding.source == DashboardBinding::Source::Signal) {
                const auto found =
                    delivered.series.find(widget.binding.message + "." + widget.binding.signal);

                // Not ASSERT_NE: a failure would print two iterators as bytes and not the sentence.
                ASSERT_TRUE(found != delivered.series.end())
                    << widget.binding.message << "." << widget.binding.signal
                    << " never reached the plot store: the widget is bound correctly and would "
                       "show dashes";

                EXPECT_TRUE(std::isfinite(found->second))
                    << widget.binding.message << "." << widget.binding.signal
                    << " is not available";
            } else if (widget.binding.source == DashboardBinding::Source::Message) {
                // A table lists every signal of its message; it is not a table of dashes if any of
                // them arrives.
                std::size_t arriving = 0;

                for (const DictionaryEntry* entry :
                     example.dictionary.signalsOf(widget.binding.message)) {
                    const auto found = delivered.series.find(entry->qualified());

                    if (found != delivered.series.end() && std::isfinite(found->second)) {
                        ++arriving;
                    }
                }

                EXPECT_GT(arriving, 0U)
                    << "no signal of " << widget.binding.message << " reached the plot store";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The fault list
// ---------------------------------------------------------------------------
//
// A Faults widget is bound to nothing, so nothing above would notice it showing an empty list for
// ever: the audit has no binding to check and the plot store has no series to be missing. What the
// J1939 example has to prove is that a fault on its bus is a row on its dashboard - which is the
// engine's own "low oil pressure" scenario, run through the pipeline the project ships.

TEST(ExampleDashboardTests, TheJ1939ExampleHasAFaultListBesideItsClusterAndAGraphThatFeedsIt)
{
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));

    const DashboardWidget* faults = faultListOf(example);
    ASSERT_NE(faults, nullptr);

    // It sits in plain view and not on top of anything: it may be on a Group, which is a frame
    // behind what it holds, and on nothing else.
    const QRectF rect{faults->x, faults->y, faults->width, faults->height};

    for (const DashboardWidget& other : example.dashboard.widgets()) {
        if (&other == faults || other.kind == DashboardWidgetKind::Group) {
            continue;
        }

        const QRectF theirs{other.x, other.y, other.width, other.height};
        EXPECT_FALSE(rect.intersects(theirs)) << "the fault list overlaps " << other.id;
    }

    // And the graph has the block that says what the bus is reporting.
    bool hasJ1939Block = false;

    for (const NodeDescription& node : example.pipeline.nodes()) {
        hasJ1939Block = hasJ1939Block || node.typeName == "j1939.decoder";
    }

    EXPECT_TRUE(hasJ1939Block);
    EXPECT_TRUE(example.dashboard.validate().succeeded());
}

TEST(ExampleDashboardTests, TheOilPressureFaultOfTheEnginesLowOilScenarioIsOnTheFaultList)
{
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));
    ASSERT_NE(faultListOf(example), nullptr);

    setScenario(example, "ecu_engine", "low_oil_pressure");

    J1939NetworkSnapshot running;
    J1939NetworkSnapshot stopped;
    double stopLamp = 0.0;

    // The engine claims its address when it starts and reports its oil pressure in a DM1 once a
    // second, under the red stop lamp: waited for, and looked at as soon as it is there.
    const Delivered delivered = run(
        example,
        [](CanEngine& engine) {
            const J1939NetworkSnapshot now = engine.j1939Network().snapshot();
            return isReporting(now, 0x00U, 100U) && hasClaimed(now, 0x00)
                   && engine.variables().value(std::string{kJ1939LampStopVariable}) > 0.5;
        },
        [&](CanEngine& engine) {
            running = engine.j1939Network().snapshot();
            stopLamp = engine.variables().value(std::string{kJ1939LampStopVariable});

            engine.stop();
            stopped = engine.j1939Network().snapshot();
        });

    ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
    EXPECT_TRUE(delivered.arrived) << "the engine's oil pressure fault did not arrive";

    // What the widget lists is built from the snapshot by the same function the panel calls, with
    // the function table that ships with the program.
    J1939NameTables names;
    ASSERT_TRUE(names.loadFile(shippedFunctionTable()).succeeded());

    const std::vector<DashboardFault> faults = dashboardFaultsOf(running, &names);

    const auto oil = std::find_if(faults.begin(), faults.end(), [](const DashboardFault& fault) {
        return fault.sourceAddress == 0U && fault.code.spn == 100U;
    });

    ASSERT_TRUE(oil != faults.end())
        << "SPN 100 of the engine is not on the list: the engine reports it in a DM1 once a "
           "second, and the widget would show an empty list for a bus with a red lamp lit";

    // The engine's own words for it: SPN 100 (engine oil pressure), FMI 1 (below normal, most
    // severe), once, under the red stop lamp.
    EXPECT_TRUE(oil->code.spnAssembled);
    EXPECT_EQ(oil->code.fmi, 1U);
    EXPECT_EQ(oil->code.occurrenceCount, 1U);
    EXPECT_EQ(oil->tone, DashboardTone::Danger);

    // From an ECU the list can name: the engine claimed its address when it started, and function 0
    // is what the table calls the engine.
    ASSERT_TRUE(oil->ecu.has_value());
    EXPECT_EQ(*oil->ecu, "Engine");

    // The lamp of the widget is the lamp of the cluster, and it is lit.
    EXPECT_GT(stopLamp, 0.5);

    // Worst first: nothing that is not as bad is above it.
    EXPECT_EQ(faults.front().tone, DashboardTone::Danger);

    // The measurement ended, and so did its faults: an empty list - while what each ECU last said
    // is still there for the J1939 panel to read, and said to be the last thing, because no ECU is
    // repeating it any more.
    EXPECT_TRUE(dashboardFaultsOf(stopped, nullptr).empty());
    EXPECT_FALSE(busOf(stopped).diagnostics.empty());
    EXPECT_FALSE(busOf(running).reporting.empty());
    EXPECT_TRUE(busOf(stopped).reporting.empty());
}

TEST(ExampleDashboardTests, EveryEcuOfTheJ1939ExampleClaimsItsAddressAndIsNamedByTheShippedTable)
{
    // The rows of the fault list say "Engine" and not only an address because the ECU claimed one
    // when it started and the function table that ships knows what its NAME says it is. Nothing
    // between the script and the table is allowed to lose that: not the claim, not the address
    // table, not the spelling of the function. The six scripted ECUs claim from their scripts, and
    // the TinyML ECU - a block of the graph and not a script - from the block itself.
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));

    const std::map<int, std::string> expected{{0x00, "Engine"},
                                              {0x0B, "System Control Brakes"},
                                              {0x17, "Instrument Cluster"},
                                              {0x21, "Body Control"},
                                              {0x37, "Operator Controls Lighting"},
                                              {0x3D, "Exhaust Emission Control"},
                                              {0x80, "System Monitor"}};

    J1939NetworkSnapshot snapshot;

    // Until every one of them has claimed: the scripted ECUs do it as they start, and the TinyML
    // ECU the first time it hears the bus.
    const Delivered delivered = run(
        example,
        [&expected](CanEngine& engine) {
            const J1939NetworkSnapshot now = engine.j1939Network().snapshot();
            return std::all_of(expected.begin(), expected.end(), [&now](const auto& entry) {
                return hasClaimed(now, entry.first);
            });
        },
        [&](CanEngine& engine) { snapshot = engine.j1939Network().snapshot(); });

    ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
    EXPECT_TRUE(delivered.arrived) << "not every ECU of the example was seen claiming";

    J1939NameTables names;
    ASSERT_TRUE(names.loadFile(shippedFunctionTable()).succeeded());

    for (const auto& [address, word] : expected) {
        SCOPED_TRACE(word);

        const auto node = std::find_if(
            busOf(snapshot).nodes.begin(),
            busOf(snapshot).nodes.end(),
            [address = address](const auto& candidate) { return candidate.address == address; });

        ASSERT_TRUE(node != busOf(snapshot).nodes.end())
            << "nothing was heard from address " << address;
        EXPECT_TRUE(node->claimSeen);
        ASSERT_TRUE(node->name.has_value());

        const std::optional<std::string_view> function = names.function(*node->name);
        ASSERT_TRUE(function.has_value());
        EXPECT_EQ(std::string{*function}, word);
    }
}

TEST(ExampleDashboardTests, EveryEcuOfTheJ1939ExampleClaimsAgainWhenTheBusIsAskedForItsClaims)
{
    // A measurement started on a machine that was already running has no claims to read, and the
    // J1939 Network panel's button asks the bus for them. Every ECU of the example has to answer:
    // the six scripted ones from the node that runs them - which can only hear the request if the
    // bus is wired to them - and the TinyML ECU from its block.
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));

    const std::set<int> ecus{0x00, 0x0B, 0x17, 0x21, 0x37, 0x3D, 0x80};

    // How many times each address has been heard claiming, counted on the engine's thread.
    std::array<std::atomic<int>, 256> claims{};
    J1939ClaimRequestOutcome outcome;
    bool asked = false;

    const auto allClaimed = [&claims, &ecus](int times) {
        return std::all_of(ecus.begin(), ecus.end(), [&claims, times](int address) {
            return claims[static_cast<std::size_t>(address)].load() >= times;
        });
    };

    const Delivered delivered = run(
        example,
        [&](CanEngine& engine) {
            // Everyone at power-up first: what follows the request is then known to answer it.
            if (!asked) {
                if (!allClaimed(1)) {
                    return false;
                }

                asked = true;
                outcome = j1939RequestAddressClaims(engine);
            }

            return allClaimed(2);
        },
        {},
        {},
        [&claims](CanEngine& engine) {
            engine.addFrameSink([&claims](std::span<const CanFrame> frames) {
                for (const CanFrame& frame : frames) {
                    const std::optional<J1939Id> id = j1939Decompose(frame);

                    if (id.has_value() && id->pgn() == kPgnAddressClaimed) {
                        ++claims[id->sourceAddress];
                    }
                }
            });
        });

    ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
    EXPECT_TRUE(asked) << "not every ECU of the example claimed when it started";
    EXPECT_EQ(outcome.sent, (std::vector<std::uint8_t>{0U}));
    EXPECT_TRUE(outcome.refused.empty());

    for (const int address : ecus) {
        EXPECT_GE(claims[static_cast<std::size_t>(address)].load(), 2)
            << "the ECU at address " << address << " did not answer the request for claims";
    }
}

TEST(ExampleDashboardTests, TheTinyMlEcusPredictiveCodeIsOnTheFaultListUnderItsOwnName)
{
    // A fault only the model can see, from a block of the graph and not from a script - and the
    // block's claim of its address is what lets the list say whose it is, and not only that it came
    // from address 128. Injecting a fault is what the canvas's button does: it sets this variable.
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));

    J1939NetworkSnapshot snapshot;

    const Delivered delivered = run(
        example,
        [](CanEngine& engine) {
            const J1939NetworkSnapshot now = engine.j1939Network().snapshot();
            return isReporting(now, 0x80U, 110U) && hasClaimed(now, 0x80);
        },
        [&](CanEngine& engine) { snapshot = engine.j1939Network().snapshot(); },
        [](CanEngine& engine) { engine.variables().set("tinyml.inject_fault", 1.5); });

    ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
    EXPECT_TRUE(delivered.arrived) << "the TinyML ECU's predictive code did not arrive";

    J1939NameTables names;
    ASSERT_TRUE(names.loadFile(shippedFunctionTable()).succeeded());

    const std::vector<DashboardFault> faults = dashboardFaultsOf(snapshot, &names);

    const auto predictive =
        std::find_if(faults.begin(), faults.end(), [](const DashboardFault& fault) {
            return fault.sourceAddress == 0x80U && fault.code.spn == 110U;
        });

    ASSERT_TRUE(predictive != faults.end())
        << "SPN 110 of the TinyML ECU is not on the list: the model is sure of trouble at once "
           "with a fault injected, and says so in a DM1 once a second";

    ASSERT_TRUE(predictive->ecu.has_value());
    EXPECT_EQ(*predictive->ecu, "System Monitor");
}

TEST(ExampleDashboardTests, AHealthyVehicleLeavesTheFaultListEmptyAndEveryLampDark)
{
    Example example;
    ASSERT_TRUE(open("j1939-vehicle.tbsproj", example));

    J1939NetworkSnapshot snapshot;
    double lamps = 0.0;

    // Until every ECU has said what it has to say - each of them a DM1 - which is when an empty
    // list means a healthy truck and not one that has not been heard from yet.
    const Delivered delivered = run(
        example,
        [](CanEngine& engine) {
            const J1939NetworkSnapshot now = engine.j1939Network().snapshot();
            return busOf(now).diagnostics.size() >= 5U && busOf(now).reporting.size() >= 5U;
        },
        [&](CanEngine& engine) {
            snapshot = engine.j1939Network().snapshot();

            for (const std::string_view lamp : kFaultLampVariables) {
                lamps += engine.variables().value(std::string{lamp});
            }
        });

    ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};
    EXPECT_TRUE(delivered.arrived) << "not every ECU of the example sent its DM1";

    // Six ECUs send a DM1 a second, every one of them saying that all is well: a code numbered
    // zero on each would be what a tool that did not know about the placeholder shows.
    EXPECT_TRUE(snapshot.published);
    EXPECT_GE(busOf(snapshot).diagnostics.size(), 5U);
    EXPECT_TRUE(busOf(snapshot).activeFaults.empty());
    EXPECT_TRUE(dashboardFaultsOf(snapshot, nullptr).empty());
    EXPECT_DOUBLE_EQ(lamps, 0.0);

    // And all of them are still saying it: the empty list cannot tell a healthy ECU from one that
    // has gone quiet, and the ECUs that are repeating their DM1 are what can.
    EXPECT_GE(busOf(snapshot).reporting.size(), 5U);

    for (const J1939Diagnostic& message : busOf(snapshot).diagnostics) {
        if (message.active) {
            EXPECT_TRUE(std::find(busOf(snapshot).reporting.begin(),
                                  busOf(snapshot).reporting.end(),
                                  message.sourceAddress)
                        != busOf(snapshot).reporting.end())
                << "address " << static_cast<int>(message.sourceAddress);
        }
    }
}
