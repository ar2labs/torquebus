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
#include "core/dashboard/SignalDictionary.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/plot/SignalSeries.h"
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
#include <set>
#include <string>
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
};

/// Runs the project on the virtual bus for `duration`: as many channels as its CAN blocks name.
[[nodiscard]] Delivered run(const Example& example, std::chrono::milliseconds duration)
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

    delivered.started = engine.start();

    if (delivered.started.failed()) {
        return delivered;
    }

    std::this_thread::sleep_for(duration);

    // Read while it is running: stopping puts some lamps out.
    for (const SeriesInfo& info : engine.plotStore().listSeries()) {
        double value = 0.0;
        std::uint64_t when = 0;

        if (engine.plotStore().latest(info.id, value, when)) {
            delivered.series[info.name] = value;
        }
    }

    engine.stop();

    return delivered;
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

        const Delivered delivered = run(example, std::chrono::milliseconds{3000});

        ASSERT_TRUE(delivered.started.succeeded()) << std::string{delivered.started.message()};

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
