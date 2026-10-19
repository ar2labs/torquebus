// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A dashboard, as the user composed it: PLAN.md v0.14.
//
// The same shape as GraphDescription, and for the same reasons. This is plain
// data with no Qt in it and no widgets behind it - what a dashboard *is*,
// separate from what draws it. That separation is what lets the project file
// carry a dashboard, a test compare two of them, and the drawing be replaced
// later without touching what a project holds.
//
// A widget is a rectangle, a kind, and a binding. The binding is the whole
// point: a gauge that is not bound to anything is a picture of a gauge.
//
//     Gauge   bound to   signal EngineSpeed of message Engine   -> shows the bus
//     Slider  bound to   variable "brake_pedal"                 -> drives a script
//
// Two sources, deliberately, and the second one is why this is more than a
// second trace window. A dashboard that can only *read* the bus is a display; a
// dashboard whose slider a simulated ECU is reading lets somebody drive the
// simulation with their hands, which is what a bench is for.
//
// Writing a CAN signal directly is not one of them, and the omission is on
// purpose: a widget that wrote a signal would have to own a message, a cycle
// time and a transmit channel - which is a transmit list entry, and there is
// one of those already. A slider writes a variable; a script or a transmit
// entry decides what that means on the wire. One mechanism, one place to look
// when a value does not arrive.
//
// A third way to name what a widget reads - a whole message - exists for one
// kind only, the Table, which lists every signal of the message it names.
//
// --- What a database adds ------------------------------------------------------
//
// A widget can be configured by hand, and it can be configured from a database:
// SignalDictionary.h turns the loaded .dbc files into suggestions - which kind of
// widget a signal wants, its unit, its range, the names in its value table - and
// into the check that a binding names something that exists. None of that is in
// this file, because a dashboard is opened as often with the database unloaded as
// loaded, and what the description holds has to mean the same either way: the unit
// and the state names are copied into the widget, not looked up when it is drawn.

#pragma once

#include "core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// What a widget looks like. The list is PLAN.md v0.14's, minus "Graph", which is the Plot
/// Viewer's job and which it does better: a Trend is the last seconds of one signal in a box,
/// not a second plotting window. "Image" was left out at first because a project would have to
/// carry and resolve a file reference; it does now, and DashboardAssets.h says how.
///
/// Append only. The names go into project files, and a project written by a build that has a
/// kind this one lacks is refused rather than drawn wrongly.
enum class DashboardWidgetKind : std::uint8_t {
    /// A dial with a needle. The one people mean by "dashboard".
    Gauge,

    /// The number, in text, with its unit. Less impressive and more often what
    /// somebody actually needs to read off a screen.
    Numeric,

    /// On or off, by colour. Anything above the threshold is on.
    Lamp,

    /// Writes `pressed` while held and `released` when let go. A momentary
    /// contact, which is what most vehicle buttons are.
    Button,

    /// Writes one of two values and stays there.
    Switch,

    /// A continuous value between the minimum and the maximum.
    Slider,

    /// The same, round. A knob and a slider differ only in how they are drawn,
    /// and both are here because a bench panel looks like the thing it
    /// simulates or it does not get used.
    Knob,

    /// Fixed text. No binding, no value - a title over a group of gauges.
    Label,

    /// An instrument cluster: speed, engine speed, temperatures, tell-tales and the TinyML panel on
    /// one screen. It has no binding of its own - a profile (`DashboardWidget::profile`) says where
    /// each of its many values comes from. Last of the kinds that existed when the file format
    /// settled, as a kind added after it has to be.
    Cluster,

    /// A level: a bar that fills between the minimum and the maximum along its longer side. Reads
    /// like a Gauge and is the better answer for a tank, a charge or a load, where the question is
    /// "how full" and not "how fast".
    Bar,

    /// The name the value has, and not the number: "Reverse", "Critical", "Engaged". A value table
    /// of the database, on a chip tinted by what the name means. A value with no name shows as the
    /// number, so an unexpected one is seen rather than hidden.
    State,

    /// A picture. Alone it is a logo or a diagram; bound to a value it is the picture of the state
    /// that value is in - a door open or shut, the gear that is engaged - with the widget's own
    /// picture for a value no state names.
    Image,

    /// The last seconds of one signal as a line, in the box it is given.
    Trend,

    /// Every signal of one message with its value: what a person wants when they have found the
    /// message and want to see what is in it, without placing eight widgets by hand.
    Table,

    /// A titled frame behind other widgets. It shows no value; it keeps a group of them looking
    /// like one.
    Group,

    /// The J1939 trouble codes of the whole bus: the four DM1 lamps and, under them, one row for
    /// each active code - the SPN, the failure mode in words, how many times it has occurred and
    /// which ECU reports it. It reads no signal and no variable of its own (the J1939 block says
    /// what the bus is reporting, and DashboardFaults.h turns that into rows), so it has no
    /// binding; with no J1939 block, or no measurement, it is an empty list and not an error.
    Faults,
};

[[nodiscard]] std::string_view nameOf(DashboardWidgetKind kind);

/// Parses what nameOf produced. False when the name is not one of them - which
/// is how a project file written by a newer version reports a widget this build
/// does not have, rather than silently drawing the wrong one.
[[nodiscard]] bool kindFromName(std::string_view name, DashboardWidgetKind& kind);

/// True for the kinds a person operates rather than reads.
///
/// The distinction decides whether a widget writes its binding or only reads
/// it, and it is asked in several places - so it is answered once, here, rather
/// than as a switch statement that grows a case out of step with the enum.
[[nodiscard]] bool writesItsBinding(DashboardWidgetKind kind);

/// How a kind relates to what it reads.
enum class BindingNeed : std::uint8_t {
    /// Reads nothing: a Label, a Group, a Cluster (which has a profile instead).
    None,

    /// May show a value and is complete without one: an Image.
    Optional,

    /// A value is the point of it, and a widget without one is a picture of that widget.
    Required,

    /// A whole message, not a signal: the Table.
    Message,
};

/// What a kind asks of the person configuring it - which fields mean something to it.
///
/// One table, read by validate() and by the form that edits a widget, so that "a lamp has no range"
/// is a fact in one place and not a condition written twice and drifting. A new kind has to be
/// given a row (the switch has no default), which is the point.
struct DashboardKindTraits final {
    BindingNeed binding{BindingNeed::None};

    /// `minimum` and `maximum` mean something: a needle sweeps them, a bar fills between them, a
    /// control writes them. validate() refuses a range that cannot be swept.
    bool range{false};

    bool unit{false};
    bool decimals{false};

    /// A lamp is on above it, and a switch starts on one side of it.
    bool threshold{false};

    /// Alarm limits tint what it draws.
    bool limits{false};

    /// Named states: the words for the values, and optionally a picture for each.
    bool states{false};

    /// Has a picture of its own.
    bool image{false};

    /// How many seconds of history it draws.
    bool window{false};
};

[[nodiscard]] const DashboardKindTraits& traitsOf(DashboardWidgetKind kind);

/// What a state or a reading means, as a colour would say it. Not a colour itself: the theme
/// decides what "danger" looks like, so a dashboard built on the dark theme reads the same on the
/// light one.
enum class DashboardTone : std::uint8_t {
    /// No opinion. Drawn in the ordinary text colour.
    Neutral,

    /// Good, or running.
    Ok,

    /// Informative.
    Info,

    /// Attention.
    Warning,

    /// Trouble.
    Danger,

    /// Inactive, absent, not applicable: drawn quiet.
    Off,
};

[[nodiscard]] std::string_view nameOf(DashboardTone tone);

/// False when the name is not one of them; `tone` is then left alone.
[[nodiscard]] bool toneFromName(std::string_view name, DashboardTone& tone);

/// One named value of a widget: what a number is called, what it means and how it is pictured.
///
/// `value` is in the units the widget reads - physical, after the signal's factor and offset - and
/// not the raw number of a value table, which is what a database holds. The two are the same for
/// most signals and different for exactly the ones where a person would be confused by it.
struct DashboardState final {
    double value{0.0};
    std::string label;
    DashboardTone tone{DashboardTone::Neutral};

    /// A picture for this state, as DashboardAssets.h resolves it. Empty means the widget's own.
    std::string image;

    [[nodiscard]] friend bool operator==(const DashboardState&, const DashboardState&) = default;
};

/// Where a reading stops being normal. Each limit is optional, and a widget with none draws what it
/// always drew.
struct DashboardLimits final {
    std::optional<double> warningBelow;
    std::optional<double> warningAbove;
    std::optional<double> criticalBelow;
    std::optional<double> criticalAbove;

    [[nodiscard]] bool any() const noexcept
    {
        return warningBelow.has_value() || warningAbove.has_value() || criticalBelow.has_value()
               || criticalAbove.has_value();
    }

    /// Danger past a critical limit, Warning past a warning one, Neutral between them.
    ///
    /// *Strictly* past: a coolant at exactly its warning limit has not exceeded it, which is also
    /// how the cluster reads a low oil pressure. Neutral and not Ok inside the limits, because a
    /// gauge that turned green whenever it was fine would be a gauge that is mostly green.
    [[nodiscard]] DashboardTone toneOf(double value) const noexcept;

    [[nodiscard]] friend bool operator==(const DashboardLimits&, const DashboardLimits&) = default;
};

struct DashboardBinding final {
    enum class Source : std::uint8_t {
        /// Draws nothing but itself. Correct for a Label, and a mistake
        /// everywhere else - which validate() says out loud.
        None,

        /// A decoded CAN signal, by message and signal name. Read-only: see the
        /// note at the top of this file.
        Signal,

        /// A system variable, by name. Read and written.
        Variable,

        /// A whole message, by name: every signal it describes. For a Table, and refused for every
        /// other kind. Last, like a kind added after the file format settled has to be.
        Message,
    };

    Source source{Source::None};

    /// For Source::Signal, where it is half of the name, and for Source::Message, where it is all
    /// of it. Both halves are needed for a signal: signal names are only unique within a message,
    /// and two ECUs on one bus routinely publish a "Temperature".
    std::string message;
    std::string signal;

    /// For Source::Variable.
    std::string variable;

    [[nodiscard]] friend bool operator==(const DashboardBinding&,
                                         const DashboardBinding&) = default;
};

struct DashboardWidget final {
    /// Unique within the dashboard, stable across saves.
    std::string id;

    DashboardWidgetKind kind{DashboardWidgetKind::Numeric};

    DashboardBinding binding;

    /// What is written above or beside it. Empty means the binding names it -
    /// a gauge on EngineSpeed is labelled EngineSpeed unless somebody says
    /// otherwise, because that is the label they would have typed.
    std::string title;

    /// Where it sits, in dashboard coordinates. Not pixels: a dashboard is
    /// scaled to the panel showing it, so a layout built on a laptop is the
    /// same layout on a bench monitor rather than a corner of one.
    double x{0.0};
    double y{0.0};
    double width{160.0};
    double height{120.0};

    /// The range a Gauge sweeps, a Slider covers, and a Numeric is clamped to
    /// for display. Not a limit on the value itself: a signal outside its
    /// range is a fact worth seeing, and a gauge pinned at maximum is how it
    /// gets seen.
    double minimum{0.0};
    double maximum{100.0};

    /// A Lamp is on above this. A Switch writes maximum when on and minimum
    /// when off, so the threshold is what decides which way it starts.
    double threshold{0.5};

    /// Shown after the number. Free text, because a unit is whatever the
    /// database says it is.
    std::string unit;

    /// Digits after the point on a Numeric. A speed to three decimals is noise
    /// pretending to be precision.
    int decimals{1};

    /// For a Cluster: the id of the ClusterProfile that says where its values come from. Empty for
    /// every other kind, which have a binding instead.
    std::string profile;

    /// A picture, as a path DashboardAssets.h knows how to resolve: relative to the project's
    /// folder when it is inside or near it, absolute otherwise. For an Image it is what is shown;
    /// for a Lamp it is drawn in place of the bulb.
    std::string image;

    /// What the values are called. Filled from a database's value table when a widget is made from
    /// one, and editable after: a state is a decision about how something reads, not a copy that
    /// has to stay in step.
    std::vector<DashboardState> states;

    /// Where the reading turns amber and red.
    DashboardLimits limits;

    /// For a Trend: how many seconds it draws.
    double windowSeconds{30.0};

    /// How long a signal may stay silent before the widget stops showing its last value and says
    /// so. Zero never, which is what every widget did before this field existed and what a widget
    /// made by hand still does; one made from a database gets three of its message's cycles.
    std::uint32_t staleMs{0};

    /// The state `value` is in, or nullptr when no state names it.
    [[nodiscard]] const DashboardState* stateOf(double value) const noexcept;

    [[nodiscard]] friend bool operator==(const DashboardWidget&, const DashboardWidget&) = default;
};

/// How big a new widget of a kind starts out.
struct DashboardSize final {
    double width{160.0};
    double height{120.0};
};

[[nodiscard]] DashboardSize defaultSizeOf(DashboardWidgetKind kind);

/// A widget of `kind` as it is first put on a dashboard: its size, the range a control starts with,
/// the profile a cluster is fed by. The one place those live, so that the Dashboard's menu and a
/// widget made from a database start from the same thing. Unbound, and with no id.
[[nodiscard]] DashboardWidget defaultWidgetOf(DashboardWidgetKind kind);

class DashboardDescription final {
public:
    /// Shown on the tab when a project holds more than one. Empty is "the
    /// dashboard", which is the common case.
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    void add(DashboardWidget widget) { m_widgets.push_back(std::move(widget)); }
    void remove(const std::string& id);

    [[nodiscard]] const std::vector<DashboardWidget>& widgets() const noexcept { return m_widgets; }

    [[nodiscard]] std::vector<DashboardWidget>& widgets() noexcept { return m_widgets; }

    /// Nullptr when absent.
    [[nodiscard]] const DashboardWidget* find(const std::string& id) const;

    [[nodiscard]] bool empty() const noexcept { return m_widgets.empty(); }
    void clear();

    /// An id nothing else uses, derived from `base` ("gauge", "gauge_2").
    [[nodiscard]] std::string uniqueId(std::string_view base) const;

    /// A copy of a widget under an id of its own, a little down and to the right of the original so
    /// that it can be seen. The id is the original's without its number and with the next free one
    /// ("engine_speed_2"), so a copy is recognisable. Returns it, or empty when `id` is not here.
    [[nodiscard]] std::string duplicate(const std::string& id, double offset = 20.0);

    /// Later widgets are drawn over earlier ones and hit first, so the order of the list is the
    /// only layering there is. A group goes to the back; the thing on top of it does not.
    void bringToFront(const std::string& id);
    void sendToBack(const std::string& id);

    /// Everything that can be checked without a database, a measurement or a
    /// screen: duplicate ids, a widget bound to nothing that needs a binding, a
    /// range that is empty or inverted, a control bound to a signal it cannot
    /// write, a negative size, a cluster whose profile this build does not have.
    ///
    /// Checked here rather than when drawing, so a dashboard says what is wrong
    /// with it while it is being built - the same reason GraphDescription
    /// validates a wire the moment it is drawn instead of at the next Start.
    ///
    /// Whether the *signal* exists is not asked: a dashboard is edited with the
    /// database unloaded as often as with it loaded, and refusing to open a
    /// project because a .dbc is on another machine would be the wrong trade.
    /// SignalDictionary::audit asks it, for a person to read and not for a file to be refused over.
    /// The same goes for a picture that is no longer where the project says it is.
    [[nodiscard]] Result validate() const;

    [[nodiscard]] friend bool operator==(const DashboardDescription&,
                                         const DashboardDescription&) = default;

private:
    std::string m_name;
    std::vector<DashboardWidget> m_widgets;
};

} // namespace torquebus
