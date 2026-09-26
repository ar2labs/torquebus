// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The dashboard, before anything draws it: what it is as data, and the values
// it shares with the scripts.
//
// Most of these are about the refusals. A dashboard that opens and shows
// nothing is the failure this layer exists to prevent, and every way of
// reaching it - a widget bound to nothing, a gauge whose range cannot be swept,
// a slider bound to something it cannot write - is cheaper to catch here than
// on a bench with the hardware plugged in.

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SystemVariables.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

/// A widget that passes validation, so a case can break exactly one thing.
[[nodiscard]] DashboardWidget goodGauge(std::string id = "gauge")
{
    DashboardWidget widget;
    widget.id = std::move(id);
    widget.kind = DashboardWidgetKind::Gauge;
    widget.binding.source = DashboardBinding::Source::Signal;
    widget.binding.message = "Engine";
    widget.binding.signal = "EngineSpeed";
    widget.minimum = 0.0;
    widget.maximum = 8000.0;

    return widget;
}

} // namespace

TEST(DashboardTests, ADashboardOfBoundWidgetsValidates)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge());

    DashboardWidget slider;
    slider.id = "brake";
    slider.kind = DashboardWidgetKind::Slider;
    slider.binding.source = DashboardBinding::Source::Variable;
    slider.binding.variable = "brake_pedal";
    slider.minimum = 0.0;
    slider.maximum = 1.0;
    dashboard.add(slider);

    DashboardWidget label;
    label.id = "title";
    label.kind = DashboardWidgetKind::Label;
    label.title = "Powertrain";
    dashboard.add(label);

    SCOPED_TRACE(::testing::Message() << std::string{dashboard.validate().message()});
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(DashboardTests, AWidgetBoundToNothingIsRefused)
{
    // A gauge bound to nothing is a picture of a gauge, and it is the mistake
    // somebody makes on the way to binding it - so the message has to say what
    // is missing rather than that something is.
    DashboardDescription dashboard;

    DashboardWidget widget = goodGauge();
    widget.binding = DashboardBinding{};
    dashboard.add(widget);

    const Result result = dashboard.validate();

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("bound") != std::string::npos);
}

TEST(DashboardTests, ALabelNeedsNoBinding)
{
    // The one kind with nothing to show but itself. Held to the same rule as
    // the others, it would be impossible to put a heading on a dashboard.
    DashboardDescription dashboard;

    DashboardWidget label;
    label.id = "heading";
    label.kind = DashboardWidgetKind::Label;
    dashboard.add(label);

    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(DashboardTests, AControlBoundToACANSignalIsRefusedAndToldWhy)
{
    // The refusal that saves an afternoon. A slider bound to a signal looks
    // reasonable, moves under the mouse, and changes nothing on the bus - and
    // the reason is a design decision nobody can guess from the screen.
    DashboardDescription dashboard;

    DashboardWidget slider;
    slider.id = "throttle";
    slider.kind = DashboardWidgetKind::Slider;
    slider.binding.source = DashboardBinding::Source::Signal;
    slider.binding.message = "Engine";
    slider.binding.signal = "Throttle";
    dashboard.add(slider);

    const Result result = dashboard.validate();

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});

    // Names the way out, not only the problem.
    EXPECT_TRUE(std::string{result.message()}.find("variable") != std::string::npos);
}

TEST(DashboardTests, ASignalBindingCarriesItsMessage)
{
    // Signal names are only unique within a message, and two ECUs on one bus
    // routinely publish a "Temperature".
    DashboardDescription dashboard;

    DashboardWidget widget = goodGauge();
    widget.binding.message.clear();
    dashboard.add(widget);

    const Result result = dashboard.validate();

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("message") != std::string::npos);
}

TEST(DashboardTests, ARangeANeedleCannotSweepIsRefused)
{
    DashboardDescription dashboard;

    DashboardWidget widget = goodGauge();
    widget.minimum = 100.0;
    widget.maximum = 100.0;
    dashboard.add(widget);

    EXPECT_TRUE(dashboard.validate().failed());

    // A lamp has only a threshold, so an empty range is not its problem.
    DashboardDescription lamps;

    DashboardWidget lamp;
    lamp.id = "warning";
    lamp.kind = DashboardWidgetKind::Lamp;
    lamp.binding.source = DashboardBinding::Source::Variable;
    lamp.binding.variable = "mil";
    lamp.minimum = 0.0;
    lamp.maximum = 0.0;
    lamps.add(lamp);

    EXPECT_TRUE(lamps.validate().succeeded());
}

TEST(DashboardTests, TwoWidgetsCannotShareAnId)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge("speed"));
    dashboard.add(goodGauge("speed"));

    EXPECT_TRUE(dashboard.validate().failed());
}

TEST(DashboardTests, ANewIdDoesNotReadAsACopyOfTheFirst)
{
    DashboardDescription dashboard;

    EXPECT_TRUE(dashboard.uniqueId("gauge") == "gauge");

    dashboard.add(goodGauge("gauge"));
    EXPECT_TRUE(dashboard.uniqueId("gauge") == "gauge_2");

    dashboard.add(goodGauge("gauge_2"));
    EXPECT_TRUE(dashboard.uniqueId("gauge") == "gauge_3");
}

TEST(DashboardTests, RemovingAWidgetRemovesThatOne)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge("a"));
    dashboard.add(goodGauge("b"));

    dashboard.remove("a");

    EXPECT_TRUE(dashboard.widgets().size() == 1);
    EXPECT_TRUE(dashboard.find("a") == nullptr);
    ASSERT_TRUE(dashboard.find("b") != nullptr);
}

TEST(DashboardTests, WidgetKindsSurviveTheRoundTripThroughTheirNames)
{
    // The names go into project files, so a kind that does not come back is a
    // dashboard that opens with the wrong widget on it.
    static constexpr DashboardWidgetKind kAll[] = {
        DashboardWidgetKind::Gauge,
        DashboardWidgetKind::Numeric,
        DashboardWidgetKind::Lamp,
        DashboardWidgetKind::Button,
        DashboardWidgetKind::Switch,
        DashboardWidgetKind::Slider,
        DashboardWidgetKind::Knob,
        DashboardWidgetKind::Label,
    };

    for (const DashboardWidgetKind kind : kAll) {
        DashboardWidgetKind parsed{};

        SCOPED_TRACE(::testing::Message() << std::string{nameOf(kind)});
        ASSERT_TRUE(kindFromName(nameOf(kind), parsed));
        EXPECT_TRUE(parsed == kind);
    }

    DashboardWidgetKind unused{};
    EXPECT_FALSE(kindFromName("hologram", unused));
}

TEST(DashboardTests, ControlsAreTheKindsThatWrite)
{
    EXPECT_TRUE(writesItsBinding(DashboardWidgetKind::Slider));
    EXPECT_TRUE(writesItsBinding(DashboardWidgetKind::Knob));
    EXPECT_TRUE(writesItsBinding(DashboardWidgetKind::Button));
    EXPECT_TRUE(writesItsBinding(DashboardWidgetKind::Switch));

    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Gauge));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Numeric));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Lamp));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Label));
}

// ---------------------------------------------------------------------------
// System variables
// ---------------------------------------------------------------------------

TEST(DashboardTests, AVariableRemembersWhatWasWrittenToIt)
{
    SystemVariables variables;

    const SystemVariables::Handle pedal = variables.resolve("brake_pedal");

    EXPECT_TRUE(variables.value(pedal) == 0.0);

    variables.set(pedal, 0.75);
    EXPECT_TRUE(variables.value(pedal) == 0.75);

    // By name, which is how a panel reaches it.
    EXPECT_TRUE(variables.value("brake_pedal") == 0.75);

    variables.set("brake_pedal", 0.25);
    EXPECT_TRUE(variables.value(pedal) == 0.25);
}

TEST(DashboardTests, TheSameNameIsTheSameVariable)
{
    // A script and a dashboard widget name the same variable without either
    // being "first". If resolve() handed out two slots, the slider would move
    // and the script would read zero forever.
    SystemVariables variables;

    const SystemVariables::Handle first = variables.resolve("speed");
    const SystemVariables::Handle second = variables.resolve("speed");

    EXPECT_TRUE(first == second);
    EXPECT_TRUE(variables.count() == 1);
}

TEST(DashboardTests, AnUnknownNameReadsAsZeroRatherThanFailing)
{
    SystemVariables variables;

    EXPECT_TRUE(variables.find("nothing") == SystemVariables::kUnknown);
    EXPECT_TRUE(variables.value("nothing") == 0.0);
    EXPECT_TRUE(variables.value(SystemVariables::kUnknown) == 0.0);

    // And writing through a bad handle is ignored rather than corrupting a slot
    // that belongs to something else.
    variables.set(SystemVariables::kUnknown, 42.0);
    EXPECT_TRUE(variables.count() == 0);
}

TEST(DashboardTests, WritingCountsEvenWhenTheValueDidNotChange)
{
    // "The number is the same" and "nothing happened" are different questions.
    // A script writing 0 every cycle is doing something, and a gauge that
    // stopped updating is a different fault from a value that is not changing.
    SystemVariables variables;

    const SystemVariables::Handle handle = variables.resolve("idle");

    const std::uint64_t before = variables.revision(handle);

    variables.set(handle, 0.0);
    variables.set(handle, 0.0);

    EXPECT_TRUE(variables.revision(handle) == before + 2);
}

TEST(DashboardTests, NamesComeBackInTheOrderTheyWereFirstSeen)
{
    SystemVariables variables;

    static_cast<void>(variables.resolve("zebra"));
    static_cast<void>(variables.resolve("alpha"));
    static_cast<void>(variables.resolve("middle"));

    const std::vector<std::string> names = variables.names();

    ASSERT_TRUE(names.size() == 3);
    EXPECT_TRUE(names[0] == "zebra");
    EXPECT_TRUE(names[1] == "alpha");
    EXPECT_TRUE(names[2] == "middle");
}

TEST(DashboardTests, TheTableSaysWhenItIsFullInsteadOfLosingAName)
{
    SystemVariables variables;

    for (std::size_t index = 0; index < SystemVariables::kMaximumVariables; ++index) {
        const SystemVariables::Handle handle = variables.resolve("v" + std::to_string(index));

        ASSERT_TRUE(handle != SystemVariables::kUnknown);
    }

    EXPECT_TRUE(variables.isFull());
    EXPECT_TRUE(variables.resolve("one_too_many") == SystemVariables::kUnknown);

    // A name already in the table still resolves when it is full: the limit is
    // on how many there are, not on how often they are asked for.
    EXPECT_TRUE(variables.resolve("v0") == 0);
}

TEST(DashboardTests, AReaderAndAWriterDoNotNeedALockBetweenThem)
{
    // Not a proof - a race is not proved absent by running it - but it does
    // exercise the path under a thread sanitizer, which is where this would be
    // caught. The first version of this class used a growing deque, which is a
    // real race on exactly this pattern: a reader indexing while the container
    // reallocates its block map.
    SystemVariables variables;

    const SystemVariables::Handle handle = variables.resolve("rpm");

    std::thread writer{[&variables, handle] {
        for (int index = 0; index < 20'000; ++index) {
            variables.set(handle, static_cast<double>(index));
        }
    }};

    double last = 0.0;
    for (int index = 0; index < 20'000; ++index) {
        last = variables.value(handle);
    }

    writer.join();

    EXPECT_TRUE(last >= 0.0);
    EXPECT_TRUE(variables.value(handle) == 19'999.0);
}

TEST(DashboardTests, AScriptReadsWhatASliderWroteAndWritesWhatAGaugeShows)
{
    // The whole point of the layer, end to end: somebody's hand moves a
    // control, a simulated ECU reads it and answers.
    SystemVariables variables;
    variables.set("brake_pedal", 0.4);

    LuaEcuNode node{R"(
        function on_enable()
            var_set("brake_light", var_get("brake_pedal") > 0.1 and 1 or 0)
            var_set("doubled", var_get("brake_pedal") * 2)
        end
    )",
                    "dash.lua"};

    node.setSystemVariables(&variables);

    ASSERT_TRUE(node.prepare(64).succeeded());

    EXPECT_TRUE(variables.value("brake_light") == 1.0);
    EXPECT_TRUE(variables.value("doubled") == 0.8);
}

TEST(DashboardTests, AScriptWithNoVariablesBehindItIsToldNotGivenZeroes)
{
    // Reading zero off a table that does not exist would look like a pedal
    // nobody is pressing, which is a fault that looks like data.
    LuaEcuNode node{R"(
        function on_enable()
            var_set("anything", 1)
        end
    )",
                    "dash.lua"};

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("var_set") != std::string::npos);
}

TEST(DashboardTests, AVariableReadEveryPassCostsNoLock)
{
    // Not a timing assertion - it is that the path works at all when a script
    // touches a variable from a timer, which is where a per-call resolve()
    // would have taken the library's lock on the frame path.
    SystemVariables variables;

    // The graph owns its nodes, so the node is built into it rather than
    // living on the stack and being handed over - the first version of this
    // case did the latter, which is a double delete with a test around it.
    auto owned = std::make_unique<LuaEcuNode>(R"(
        function on_enable()
            every(2, function()
                var_set("ticks", var_get("ticks") + 1)
            end)
        end
    )",
                                              "dash.lua");

    owned->setSystemVariables(&variables);

    PipelineGraph graph;
    static_cast<void>(graph.addNode(std::move(owned)));

    ASSERT_TRUE(graph.compile().succeeded());

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{60};
    while (std::chrono::steady_clock::now() < until) {
        graph.execute();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    EXPECT_TRUE(variables.value("ticks") >= 2.0);
}

TEST(DashboardTests, ClearingForgetsTheValuesAsWellAsTheNames)
{
    SystemVariables variables;

    variables.set("speed", 42.0);
    ASSERT_TRUE(variables.count() == 1);

    variables.clear();

    EXPECT_TRUE(variables.count() == 0);
    EXPECT_TRUE(variables.value("speed") == 0.0);

    // And the slot is reusable, with no memory of what it held.
    const SystemVariables::Handle handle = variables.resolve("speed");
    EXPECT_TRUE(variables.value(handle) == 0.0);
    EXPECT_TRUE(variables.revision(handle) == 0);
}
