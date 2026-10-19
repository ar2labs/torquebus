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
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
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

TEST(DashboardTests, AClusterNeedsAProfileInsteadOfABinding)
{
    // A cluster shows dozens of values and names none of them, so a binding would be a lie about
    // what it reads. What it has to name is its profile, and a profile this build does not have is
    // the way it ends up bound to nothing.
    DashboardWidget cluster;
    cluster.id = "cluster";
    cluster.kind = DashboardWidgetKind::Cluster;
    cluster.profile = std::string{kDefaultClusterProfile};

    DashboardDescription dashboard;
    dashboard.add(cluster);
    EXPECT_TRUE(dashboard.validate().succeeded());

    dashboard.widgets().front().profile = "from-the-future";
    const Result unknown = dashboard.validate();
    ASSERT_TRUE(unknown.failed());
    SCOPED_TRACE(::testing::Message() << std::string{unknown.message()});
    EXPECT_TRUE(std::string{unknown.message()}.find("from-the-future") != std::string::npos);

    dashboard.widgets().front().profile.clear();
    EXPECT_TRUE(dashboard.validate().failed());
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
        DashboardWidgetKind::Cluster,
        DashboardWidgetKind::Bar,
        DashboardWidgetKind::State,
        DashboardWidgetKind::Image,
        DashboardWidgetKind::Trend,
        DashboardWidgetKind::Table,
        DashboardWidgetKind::Group,
        DashboardWidgetKind::Faults,
    };

    for (const DashboardWidgetKind kind : kAll) {
        DashboardWidgetKind parsed{};

        SCOPED_TRACE(::testing::Message() << std::string{nameOf(kind)});
        ASSERT_TRUE(kindFromName(nameOf(kind), parsed));
        EXPECT_TRUE(parsed == kind);
    }

    // Written into project files, so spelled out: a rename orphans every dashboard that has one.
    EXPECT_EQ(nameOf(DashboardWidgetKind::Faults), "faults");

    DashboardWidgetKind unused{};
    EXPECT_FALSE(kindFromName("hologram", unused));
}

TEST(DashboardTests, ANewKindIsAppendedAndNeverInsertedBecauseTheNamesAreInProjectFiles)
{
    // The numbers are not in a project file, the names are - but a kind inserted in the middle
    // moves every one after it, and anything that stores the number (a saved selection, a combo
    // box's data) would then mean another widget. The last kind is the newest one.
    EXPECT_EQ(static_cast<int>(DashboardWidgetKind::Faults),
              static_cast<int>(DashboardWidgetKind::Group) + 1);
    EXPECT_EQ(static_cast<int>(DashboardWidgetKind::Group),
              static_cast<int>(DashboardWidgetKind::Table) + 1);
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
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Cluster));

    // The kinds that came later only read.
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Bar));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::State));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Image));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Trend));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Table));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Group));
    EXPECT_FALSE(writesItsBinding(DashboardWidgetKind::Faults));
}

// ---------------------------------------------------------------------------
// What each kind asks of the person configuring it
// ---------------------------------------------------------------------------

TEST(DashboardTests, EachKindKeepsTheFieldsItsFormShowedBeforeTheTableExisted)
{
    // The table replaced conditions written in two places. It is only a refactoring if the kinds
    // that were here come out the same, so the rows they had are pinned.
    const DashboardKindTraits& gauge = traitsOf(DashboardWidgetKind::Gauge);
    EXPECT_EQ(gauge.binding, BindingNeed::Required);
    EXPECT_TRUE(gauge.range && gauge.unit && gauge.decimals);
    EXPECT_FALSE(gauge.threshold);

    // A lamp has a threshold and nothing to sweep, no unit and no digits.
    const DashboardKindTraits& lamp = traitsOf(DashboardWidgetKind::Lamp);
    EXPECT_TRUE(lamp.threshold);
    EXPECT_FALSE(lamp.range || lamp.unit || lamp.decimals);

    // A button writes its range, so it has one; it shows no unit.
    for (const DashboardWidgetKind kind :
         {DashboardWidgetKind::Button, DashboardWidgetKind::Switch}) {
        const DashboardKindTraits& control = traitsOf(kind);
        EXPECT_TRUE(control.range && control.threshold && control.decimals);
        EXPECT_FALSE(control.unit);
    }

    for (const DashboardWidgetKind kind :
         {DashboardWidgetKind::Slider, DashboardWidgetKind::Knob}) {
        const DashboardKindTraits& control = traitsOf(kind);
        EXPECT_TRUE(control.range && control.unit && control.decimals);
        EXPECT_FALSE(control.threshold);
    }

    // A label and a cluster read nothing, and a cluster has a profile instead.
    for (const DashboardWidgetKind kind :
         {DashboardWidgetKind::Label, DashboardWidgetKind::Cluster}) {
        const DashboardKindTraits& none = traitsOf(kind);
        EXPECT_EQ(none.binding, BindingNeed::None);
        EXPECT_FALSE(none.range || none.unit || none.decimals || none.threshold);
    }
}

TEST(DashboardTests, TheKindsThatCameLaterAskForWhatTheyShow)
{
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Bar).range);
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Bar).limits);

    EXPECT_TRUE(traitsOf(DashboardWidgetKind::State).states);
    EXPECT_FALSE(traitsOf(DashboardWidgetKind::State).range);

    EXPECT_EQ(traitsOf(DashboardWidgetKind::Image).binding, BindingNeed::Optional);
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Image).image);
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Image).states);

    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Trend).window);
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Trend).range);

    EXPECT_EQ(traitsOf(DashboardWidgetKind::Table).binding, BindingNeed::Message);
    EXPECT_EQ(traitsOf(DashboardWidgetKind::Group).binding, BindingNeed::None);

    // A lamp can carry a picture and names; a gauge cannot.
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Lamp).image);
    EXPECT_FALSE(traitsOf(DashboardWidgetKind::Gauge).image);
    EXPECT_TRUE(traitsOf(DashboardWidgetKind::Numeric).states);
}

TEST(DashboardTests, AWidgetStartsAsTheMenuHasAlwaysMadeIt)
{
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Slider).width, 220.0);
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Slider).maximum, 1.0);
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Knob).height, 140.0);
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Lamp).maximum, 1.0);
    EXPECT_EQ(defaultWidgetOf(DashboardWidgetKind::Label).title, "Label");
    EXPECT_EQ(defaultWidgetOf(DashboardWidgetKind::Cluster).profile,
              std::string{kDefaultClusterProfile});

    // A gauge is the size of every widget before it had a size of its own.
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Gauge).width, 160.0);
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Gauge).height, 120.0);
    EXPECT_DOUBLE_EQ(defaultWidgetOf(DashboardWidgetKind::Gauge).maximum, 100.0);

    for (const DashboardWidgetKind kind : {DashboardWidgetKind::Bar,
                                           DashboardWidgetKind::State,
                                           DashboardWidgetKind::Image,
                                           DashboardWidgetKind::Trend,
                                           DashboardWidgetKind::Table,
                                           DashboardWidgetKind::Group,
                                           DashboardWidgetKind::Faults}) {
        EXPECT_GT(defaultWidgetOf(kind).width, 0.0) << nameOf(kind);
        EXPECT_GT(defaultWidgetOf(kind).height, 0.0) << nameOf(kind);
    }
}

TEST(DashboardTests, AFaultListReadsTheBusAndAsksForNoBinding)
{
    // It lists what the J1939 block says the bus is reporting. There is nothing to bind it to, no
    // range to sweep and no unit to show - so the form has a title to offer and nothing else.
    const DashboardKindTraits& traits = traitsOf(DashboardWidgetKind::Faults);

    EXPECT_EQ(traits.binding, BindingNeed::None);
    EXPECT_FALSE(traits.range || traits.unit || traits.decimals || traits.threshold);
    EXPECT_FALSE(traits.limits || traits.states || traits.image || traits.window);

    DashboardWidget faults = defaultWidgetOf(DashboardWidgetKind::Faults);
    faults.id = "faults";

    EXPECT_EQ(faults.kind, DashboardWidgetKind::Faults);
    EXPECT_EQ(faults.title, "Faults");
    EXPECT_EQ(faults.binding.source, DashboardBinding::Source::None);

    // Room for the four lamps and a few two-line rows, and not a Group's or a Cluster's.
    EXPECT_GE(faults.width, 240.0);
    EXPECT_GE(faults.height, 200.0);

    DashboardDescription dashboard;
    dashboard.add(faults);

    // As made, unbound, it is a dashboard that can be saved: not "a picture of a gauge".
    SCOPED_TRACE(::testing::Message() << std::string{dashboard.validate().message()});
    EXPECT_TRUE(dashboard.validate().succeeded());

    // It is held to what any widget is held to: a size.
    dashboard.widgets().front().width = 0.0;
    EXPECT_TRUE(dashboard.validate().failed());
}

TEST(DashboardTests, AFaultListIsHeldToTheRulesOfBindingsLikeAnyOtherWidget)
{
    // It has no binding of its own, as a Label has none; one typed into a project file by hand is
    // held to the rules as anywhere else, and a whole message is something only a table can show.
    DashboardDescription dashboard;

    DashboardWidget faults = defaultWidgetOf(DashboardWidgetKind::Faults);
    faults.id = "faults";
    faults.binding.source = DashboardBinding::Source::Message;
    faults.binding.message = "DM1";
    dashboard.add(faults);

    const Result result = dashboard.validate();

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_NE(std::string{result.message()}.find("table"), std::string::npos);
}

// ---------------------------------------------------------------------------
// A table, a picture, a group
// ---------------------------------------------------------------------------

TEST(DashboardTests, ATableIsBoundToAMessageAndToNothingElse)
{
    DashboardWidget table = defaultWidgetOf(DashboardWidgetKind::Table);
    table.id = "eec1";

    DashboardDescription dashboard;

    // Bound to nothing: which message?
    dashboard.add(table);
    const Result nothing = dashboard.validate();
    ASSERT_TRUE(nothing.failed());
    EXPECT_NE(std::string{nothing.message()}.find("message"), std::string::npos);

    // To a signal: a table lists many.
    dashboard.widgets().front().binding.source = DashboardBinding::Source::Signal;
    dashboard.widgets().front().binding.message = "EEC1";
    dashboard.widgets().front().binding.signal = "EngineSpeed";
    EXPECT_TRUE(dashboard.validate().failed());

    // To a message with no name.
    dashboard.widgets().front().binding.source = DashboardBinding::Source::Message;
    dashboard.widgets().front().binding.message.clear();
    EXPECT_TRUE(dashboard.validate().failed());

    dashboard.widgets().front().binding.message = "EEC1";
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(DashboardTests, OnlyATableMayBeBoundToAWholeMessage)
{
    DashboardDescription dashboard;

    DashboardWidget gauge = goodGauge();
    gauge.binding.source = DashboardBinding::Source::Message;
    gauge.binding.message = "EEC1";
    gauge.binding.signal.clear();
    dashboard.add(gauge);

    const Result result = dashboard.validate();

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});

    // Names the way out: pick a signal.
    EXPECT_NE(std::string{result.message()}.find("signal"), std::string::npos);
}

TEST(DashboardTests, APictureNeedsNoBindingAndAGroupNeedsNothing)
{
    DashboardDescription dashboard;

    DashboardWidget logo = defaultWidgetOf(DashboardWidgetKind::Image);
    logo.id = "logo";
    logo.image = "pictures/logo.png";
    dashboard.add(logo);

    DashboardWidget group = defaultWidgetOf(DashboardWidgetKind::Group);
    group.id = "powertrain";
    dashboard.add(group);

    SCOPED_TRACE(::testing::Message() << std::string{dashboard.validate().message()});
    EXPECT_TRUE(dashboard.validate().succeeded());

    // Bound, it is the picture of a state; the binding is held to the same rules as any other.
    dashboard.widgets().front().binding.source = DashboardBinding::Source::Signal;
    dashboard.widgets().front().binding.message = "Doors";
    EXPECT_TRUE(dashboard.validate().failed());

    dashboard.widgets().front().binding.signal = "DoorState";
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(DashboardTests, AStateAndATrendAreBoundLikeAGauge)
{
    DashboardDescription dashboard;

    for (const DashboardWidgetKind kind :
         {DashboardWidgetKind::State, DashboardWidgetKind::Trend, DashboardWidgetKind::Bar}) {
        DashboardWidget widget = defaultWidgetOf(kind);
        widget.id = std::string{nameOf(kind)};
        dashboard.add(widget);
    }

    // Unbound, each is a picture of itself.
    EXPECT_TRUE(dashboard.validate().failed());

    for (DashboardWidget& widget : dashboard.widgets()) {
        widget.binding.source = DashboardBinding::Source::Variable;
        widget.binding.variable = "rpm";
    }

    EXPECT_TRUE(dashboard.validate().succeeded());

    // A trend and a bar sweep a range, a state does not: the state is not the one refused.
    for (DashboardWidget& widget : dashboard.widgets()) {
        widget.minimum = 5.0;
        widget.maximum = 5.0;
    }

    const Result result = dashboard.validate();
    ASSERT_TRUE(result.failed());
    EXPECT_NE(std::string{result.message()}.find("'trend'"), std::string::npos);
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------

TEST(DashboardTests, AStateIsFoundByItsValueToAPartInABillion)
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::State);

    DashboardState reverse;
    reverse.value = -1.0;
    reverse.label = "Reverse";

    DashboardState first;
    first.value = 1.0;
    first.label = "First";

    widget.states = {reverse, first};

    ASSERT_NE(widget.stateOf(-1.0), nullptr);
    EXPECT_EQ(widget.stateOf(-1.0)->label, "Reverse");
    EXPECT_EQ(widget.stateOf(1.0 + 1e-12)->label, "First");

    EXPECT_EQ(widget.stateOf(0.0), nullptr);
    EXPECT_EQ(widget.stateOf(1.1), nullptr);
    EXPECT_EQ(widget.stateOf(std::numeric_limits<double>::quiet_NaN()), nullptr);

    EXPECT_EQ(DashboardWidget{}.stateOf(0.0), nullptr);
}

TEST(DashboardTests, TwoNamesForOneValueAreRefusedAndSoIsAValueThatIsNotANumber)
{
    DashboardDescription dashboard;

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::State);
    widget.id = "gear";
    widget.binding.source = DashboardBinding::Source::Variable;
    widget.binding.variable = "gear";

    DashboardState a;
    a.value = 1.0;
    a.label = "First";

    DashboardState b;
    b.value = 2.0;
    b.label = "Second";

    widget.states = {a, b};
    dashboard.add(widget);
    EXPECT_TRUE(dashboard.validate().succeeded());

    dashboard.widgets().front().states[1].value = 1.0;
    const Result twice = dashboard.validate();
    ASSERT_TRUE(twice.failed());
    EXPECT_NE(std::string{twice.message()}.find("twice"), std::string::npos);

    dashboard.widgets().front().states[1].value = std::numeric_limits<double>::quiet_NaN();
    const Result nan = dashboard.validate();
    ASSERT_TRUE(nan.failed());
    EXPECT_NE(std::string{nan.message()}.find("Second"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Alarm limits
// ---------------------------------------------------------------------------

TEST(DashboardTests, ALimitIsExceededOnlyWhenTheReadingIsStrictlyPastIt)
{
    DashboardLimits limits;
    limits.warningAbove = 100.0;
    limits.criticalAbove = 110.0;
    limits.warningBelow = 20.0;
    limits.criticalBelow = 10.0;

    EXPECT_TRUE(limits.any());

    EXPECT_EQ(limits.toneOf(60.0), DashboardTone::Neutral);

    // At the limit is not past it.
    EXPECT_EQ(limits.toneOf(100.0), DashboardTone::Neutral);
    EXPECT_EQ(limits.toneOf(20.0), DashboardTone::Neutral);

    EXPECT_EQ(limits.toneOf(100.1), DashboardTone::Warning);
    EXPECT_EQ(limits.toneOf(19.9), DashboardTone::Warning);

    EXPECT_EQ(limits.toneOf(110.0), DashboardTone::Warning);
    EXPECT_EQ(limits.toneOf(110.1), DashboardTone::Danger);
    EXPECT_EQ(limits.toneOf(10.0), DashboardTone::Warning);
    EXPECT_EQ(limits.toneOf(9.9), DashboardTone::Danger);

    // Not a number is not past anything.
    EXPECT_EQ(limits.toneOf(std::numeric_limits<double>::quiet_NaN()), DashboardTone::Neutral);
}

TEST(DashboardTests, LimitsThatAreNotSetAreNotThere)
{
    EXPECT_FALSE(DashboardLimits{}.any());
    EXPECT_EQ(DashboardLimits{}.toneOf(1e9), DashboardTone::Neutral);

    // Only the side that is set counts.
    DashboardLimits oilPressure;
    oilPressure.criticalBelow = 100.0;

    EXPECT_EQ(oilPressure.toneOf(5000.0), DashboardTone::Neutral);
    EXPECT_EQ(oilPressure.toneOf(99.0), DashboardTone::Danger);
}

TEST(DashboardTests, LimitsThatContradictEachOtherAreRefusedAndTheOnesThatDoNotAreNot)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge());

    DashboardLimits& limits = dashboard.widgets().front().limits;

    limits.warningAbove = 6000.0;
    limits.criticalAbove = 7000.0;
    limits.warningBelow = 800.0;
    limits.criticalBelow = 500.0;
    EXPECT_TRUE(dashboard.validate().succeeded());

    // A band that is empty: everything is a warning.
    limits.warningBelow = 6500.0;
    EXPECT_TRUE(dashboard.validate().failed());
    limits.warningBelow = 800.0;

    // A critical limit short of the warning one it is meant to follow.
    limits.criticalAbove = 5000.0;
    const Result short_ = dashboard.validate();
    ASSERT_TRUE(short_.failed());
    EXPECT_NE(std::string{short_.message()}.find("critical"), std::string::npos);
    limits.criticalAbove = 7000.0;

    limits.criticalBelow = 900.0;
    EXPECT_TRUE(dashboard.validate().failed());
    limits.criticalBelow = 500.0;

    limits.warningAbove = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(dashboard.validate().failed());
    limits.warningAbove = 6000.0;

    EXPECT_TRUE(dashboard.validate().succeeded());
}

// ---------------------------------------------------------------------------
// A trend's window, and how long a signal may go quiet
// ---------------------------------------------------------------------------

TEST(DashboardTests, ATrendDrawsBetweenASecondAndAnHour)
{
    DashboardWidget trend = defaultWidgetOf(DashboardWidgetKind::Trend);
    trend.id = "rpm";
    trend.binding.source = DashboardBinding::Source::Variable;
    trend.binding.variable = "rpm";

    DashboardDescription dashboard;
    dashboard.add(trend);
    EXPECT_TRUE(dashboard.validate().succeeded());

    for (const double bad : {0.0, 0.5, 3601.0, -5.0, std::numeric_limits<double>::quiet_NaN()}) {
        dashboard.widgets().front().windowSeconds = bad;
        EXPECT_TRUE(dashboard.validate().failed()) << bad;
    }

    for (const double good : {1.0, 30.0, 3600.0}) {
        dashboard.widgets().front().windowSeconds = good;
        EXPECT_TRUE(dashboard.validate().succeeded()) << good;
    }

    // A window is the Trend's business: a gauge with a nonsense one is not refused for it.
    DashboardDescription gauges;
    gauges.add(goodGauge());
    gauges.widgets().front().windowSeconds = 0.0;
    EXPECT_TRUE(gauges.validate().succeeded());
}

TEST(DashboardTests, ASignalMayNotBeCalledStaleAfterMoreThanAnHour)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge());

    dashboard.widgets().front().staleMs = 0;
    EXPECT_TRUE(dashboard.validate().succeeded());

    dashboard.widgets().front().staleMs = 3'600'000;
    EXPECT_TRUE(dashboard.validate().succeeded());

    dashboard.widgets().front().staleMs = 3'600'001;
    EXPECT_TRUE(dashboard.validate().failed());
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

TEST(DashboardTests, ACopyHasAnIdOfItsOwnAndSitsOffsetFromTheOriginal)
{
    DashboardDescription dashboard;

    DashboardWidget widget = goodGauge("engine_speed");
    widget.x = 40.0;
    widget.y = 60.0;
    dashboard.add(widget);

    const std::string first = dashboard.duplicate("engine_speed");
    EXPECT_EQ(first, "engine_speed_2");

    const DashboardWidget* copy = dashboard.find(first);
    ASSERT_NE(copy, nullptr);
    EXPECT_DOUBLE_EQ(copy->x, 60.0);
    EXPECT_DOUBLE_EQ(copy->y, 80.0);
    EXPECT_EQ(copy->binding, widget.binding);

    // A copy of a copy is the next number, and not "engine_speed_2_2".
    EXPECT_EQ(dashboard.duplicate(first, 10.0), "engine_speed_3");
    EXPECT_EQ(dashboard.widgets().size(), 3U);

    EXPECT_EQ(dashboard.duplicate("nothing"), "");
    EXPECT_EQ(dashboard.widgets().size(), 3U);
    EXPECT_TRUE(dashboard.validate().succeeded());
}

TEST(DashboardTests, TheOrderOfTheListIsTheLayering)
{
    DashboardDescription dashboard;
    dashboard.add(goodGauge("a"));
    dashboard.add(goodGauge("b"));
    dashboard.add(goodGauge("c"));

    const auto order = [&dashboard] {
        std::string ids;
        for (const DashboardWidget& widget : dashboard.widgets()) {
            ids += widget.id;
        }
        return ids;
    };

    dashboard.bringToFront("a");
    EXPECT_EQ(order(), "bca");

    dashboard.sendToBack("c");
    EXPECT_EQ(order(), "cba");

    // Already there, or not there at all: nothing moves.
    dashboard.bringToFront("a");
    dashboard.sendToBack("c");
    dashboard.bringToFront("nothing");
    dashboard.sendToBack("nothing");
    EXPECT_EQ(order(), "cba");
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
