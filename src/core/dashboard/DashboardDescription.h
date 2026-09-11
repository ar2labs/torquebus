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

#pragma once

#include "core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// What a widget looks like. The list is PLAN.md v0.14's, minus the ones that
/// need a second mechanism to mean anything: "Graph" is the Graph panel, which
/// exists and is better at it, and "Image" is a file reference that a project
/// would then have to carry and resolve.
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
    };

    Source source{Source::None};

    /// For Source::Signal. Both are needed: signal names are only unique within
    /// a message, and two ECUs on one bus routinely publish a "Temperature".
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

    [[nodiscard]] friend bool operator==(const DashboardWidget&,
                                         const DashboardWidget&) = default;
};

class DashboardDescription final {
public:
    /// Shown on the tab when a project holds more than one. Empty is "the
    /// dashboard", which is the common case.
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    void add(DashboardWidget widget) { m_widgets.push_back(std::move(widget)); }
    void remove(const std::string& id);

    [[nodiscard]] const std::vector<DashboardWidget>& widgets() const noexcept
    {
        return m_widgets;
    }

    [[nodiscard]] std::vector<DashboardWidget>& widgets() noexcept { return m_widgets; }

    /// Nullptr when absent.
    [[nodiscard]] const DashboardWidget* find(const std::string& id) const;

    [[nodiscard]] bool empty() const noexcept { return m_widgets.empty(); }
    void clear();

    /// An id nothing else uses, derived from `base` ("gauge", "gauge_2").
    [[nodiscard]] std::string uniqueId(std::string_view base) const;

    /// Everything that can be checked without a database, a measurement or a
    /// screen: duplicate ids, a widget bound to nothing that needs a binding, a
    /// range that is empty or inverted, a control bound to a signal it cannot
    /// write, a negative size.
    ///
    /// Checked here rather than when drawing, so a dashboard says what is wrong
    /// with it while it is being built - the same reason GraphDescription
    /// validates a wire the moment it is drawn instead of at the next Start.
    ///
    /// Whether the *signal* exists is not asked: a dashboard is edited with the
    /// database unloaded as often as with it loaded, and refusing to open a
    /// project because a .dbc is on another machine would be the wrong trade.
    [[nodiscard]] Result validate() const;

    [[nodiscard]] friend bool operator==(const DashboardDescription&,
                                         const DashboardDescription&) = default;

private:
    std::string m_name;
    std::vector<DashboardWidget> m_widgets;
};

} // namespace torquebus
