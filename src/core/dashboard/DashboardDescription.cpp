// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/DashboardDescription.h"

#include "core/dashboard/cluster/ClusterProfiles.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <set>

namespace torquebus {
namespace {

/// "gauge_2" -> "gauge". What a copy's id is built from, so that duplicating the third copy of a
/// widget gives the fourth and not "gauge_2_2".
[[nodiscard]] std::string withoutNumberSuffix(const std::string& id)
{
    const std::size_t underscore = id.rfind('_');

    if (underscore == std::string::npos || underscore == 0 || underscore + 1 == id.size()) {
        return id;
    }

    const bool digits = std::all_of(id.begin() + static_cast<std::ptrdiff_t>(underscore) + 1,
                                    id.end(),
                                    [](char c) { return c >= '0' && c <= '9'; });

    return digits ? id.substr(0, underscore) : id;
}

} // namespace

std::string_view nameOf(DashboardWidgetKind kind)
{
    // Written into project files, so these never change once released - a
    // rename orphans every dashboard anybody saved.
    switch (kind) {
    case DashboardWidgetKind::Gauge:
        return "gauge";
    case DashboardWidgetKind::Numeric:
        return "numeric";
    case DashboardWidgetKind::Lamp:
        return "lamp";
    case DashboardWidgetKind::Button:
        return "button";
    case DashboardWidgetKind::Switch:
        return "switch";
    case DashboardWidgetKind::Slider:
        return "slider";
    case DashboardWidgetKind::Knob:
        return "knob";
    case DashboardWidgetKind::Label:
        return "label";
    case DashboardWidgetKind::Cluster:
        return "cluster";
    case DashboardWidgetKind::Bar:
        return "bar";
    case DashboardWidgetKind::State:
        return "state";
    case DashboardWidgetKind::Image:
        return "image";
    case DashboardWidgetKind::Trend:
        return "trend";
    case DashboardWidgetKind::Table:
        return "table";
    case DashboardWidgetKind::Group:
        return "group";
    case DashboardWidgetKind::Faults:
        return "faults";
    }

    return "numeric";
}

bool kindFromName(std::string_view name, DashboardWidgetKind& kind)
{
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

    for (const DashboardWidgetKind candidate : kAll) {
        if (nameOf(candidate) == name) {
            kind = candidate;
            return true;
        }
    }

    return false;
}

bool writesItsBinding(DashboardWidgetKind kind)
{
    switch (kind) {
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
    case DashboardWidgetKind::Slider:
    case DashboardWidgetKind::Knob:
        return true;

    case DashboardWidgetKind::Gauge:
    case DashboardWidgetKind::Numeric:
    case DashboardWidgetKind::Lamp:
    case DashboardWidgetKind::Label:
    case DashboardWidgetKind::Cluster:
    case DashboardWidgetKind::Bar:
    case DashboardWidgetKind::State:
    case DashboardWidgetKind::Image:
    case DashboardWidgetKind::Trend:
    case DashboardWidgetKind::Table:
    case DashboardWidgetKind::Group:
    case DashboardWidgetKind::Faults:
        return false;
    }

    return false;
}

const DashboardKindTraits& traitsOf(DashboardWidgetKind kind)
{
    // Only what is true is written; the rest is false. Each row is what the form showed for that
    // kind before this table existed, so a kind that was here keeps the fields it had.
    static constexpr DashboardKindTraits kGauge{.binding = BindingNeed::Required,
                                                .range = true,
                                                .unit = true,
                                                .decimals = true,
                                                .limits = true};

    static constexpr DashboardKindTraits kNumeric{.binding = BindingNeed::Required,
                                                  .range = true,
                                                  .unit = true,
                                                  .decimals = true,
                                                  .limits = true,
                                                  .states = true};

    static constexpr DashboardKindTraits kLamp{
        .binding = BindingNeed::Required, .threshold = true, .states = true, .image = true};

    // The range of a button is what it writes when pressed and when let go.
    static constexpr DashboardKindTraits kButton{
        .binding = BindingNeed::Required, .range = true, .decimals = true, .threshold = true};

    static constexpr DashboardKindTraits kSlider{
        .binding = BindingNeed::Required, .range = true, .unit = true, .decimals = true};

    static constexpr DashboardKindTraits kNothing{};

    static constexpr DashboardKindTraits kBar{.binding = BindingNeed::Required,
                                              .range = true,
                                              .unit = true,
                                              .decimals = true,
                                              .limits = true};

    // A state with no name for the value shows the number, so it has decimals but no unit.
    static constexpr DashboardKindTraits kState{
        .binding = BindingNeed::Required, .decimals = true, .states = true};

    static constexpr DashboardKindTraits kImage{
        .binding = BindingNeed::Optional, .states = true, .image = true};

    static constexpr DashboardKindTraits kTrend{.binding = BindingNeed::Required,
                                                .range = true,
                                                .unit = true,
                                                .decimals = true,
                                                .limits = true,
                                                .window = true};

    static constexpr DashboardKindTraits kTable{.binding = BindingNeed::Message};

    switch (kind) {
    case DashboardWidgetKind::Gauge:
        return kGauge;
    case DashboardWidgetKind::Numeric:
        return kNumeric;
    case DashboardWidgetKind::Lamp:
        return kLamp;
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
        return kButton;
    case DashboardWidgetKind::Slider:
    case DashboardWidgetKind::Knob:
        return kSlider;
    // A fault list reads the J1939 block and nothing a person could configure but its title: no
    // binding, no range, no unit. The form shows it a title and a sentence about where it gets its
    // codes.
    case DashboardWidgetKind::Label:
    case DashboardWidgetKind::Cluster:
    case DashboardWidgetKind::Group:
    case DashboardWidgetKind::Faults:
        return kNothing;
    case DashboardWidgetKind::Bar:
        return kBar;
    case DashboardWidgetKind::State:
        return kState;
    case DashboardWidgetKind::Image:
        return kImage;
    case DashboardWidgetKind::Trend:
        return kTrend;
    case DashboardWidgetKind::Table:
        return kTable;
    }

    return kNothing;
}

std::string_view nameOf(DashboardTone tone)
{
    // Written into project files, like the kinds.
    switch (tone) {
    case DashboardTone::Neutral:
        return "neutral";
    case DashboardTone::Ok:
        return "ok";
    case DashboardTone::Info:
        return "info";
    case DashboardTone::Warning:
        return "warning";
    case DashboardTone::Danger:
        return "danger";
    case DashboardTone::Off:
        return "off";
    }

    return "neutral";
}

bool toneFromName(std::string_view name, DashboardTone& tone)
{
    static constexpr DashboardTone kAll[] = {
        DashboardTone::Neutral,
        DashboardTone::Ok,
        DashboardTone::Info,
        DashboardTone::Warning,
        DashboardTone::Danger,
        DashboardTone::Off,
    };

    for (const DashboardTone candidate : kAll) {
        if (nameOf(candidate) == name) {
            tone = candidate;
            return true;
        }
    }

    return false;
}

DashboardTone DashboardLimits::toneOf(double value) const noexcept
{
    // A reading that is not a number is not past anything.
    if (std::isnan(value)) {
        return DashboardTone::Neutral;
    }

    if ((criticalAbove.has_value() && value > *criticalAbove)
        || (criticalBelow.has_value() && value < *criticalBelow)) {
        return DashboardTone::Danger;
    }

    if ((warningAbove.has_value() && value > *warningAbove)
        || (warningBelow.has_value() && value < *warningBelow)) {
        return DashboardTone::Warning;
    }

    return DashboardTone::Neutral;
}

const DashboardState* DashboardWidget::stateOf(double value) const noexcept
{
    // Equal to within a part in a billion, not exactly: the value in the state was worked out from
    // a raw number by the same arithmetic the decoder uses, but a person can also have typed it.
    for (const DashboardState& state : states) {
        const double scale = std::max(1.0, std::abs(state.value));

        if (std::abs(state.value - value) <= scale * 1e-9) {
            return &state;
        }
    }

    return nullptr;
}

DashboardSize defaultSizeOf(DashboardWidgetKind kind)
{
    switch (kind) {
    case DashboardWidgetKind::Slider:
        return {220.0, 60.0};
    case DashboardWidgetKind::Knob:
        return {140.0, 140.0};
    case DashboardWidgetKind::Lamp:
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
        return {120.0, 80.0};
    case DashboardWidgetKind::Label:
        return {200.0, 40.0};

    // The cluster's own stage is 1280 x 560; drawn at another shape it is centred in what it is
    // given, so this is just the size at which it is first seen whole.
    case DashboardWidgetKind::Cluster:
        return {720.0, 315.0};

    case DashboardWidgetKind::Bar:
        return {220.0, 64.0};
    case DashboardWidgetKind::State:
        return {160.0, 64.0};
    case DashboardWidgetKind::Image:
        return {120.0, 120.0};
    case DashboardWidgetKind::Trend:
        return {280.0, 140.0};
    case DashboardWidgetKind::Table:
        return {260.0, 200.0};
    case DashboardWidgetKind::Group:
        return {360.0, 240.0};

    // The lamps, and room for four codes of two lines each.
    case DashboardWidgetKind::Faults:
        return {320.0, 260.0};

    case DashboardWidgetKind::Gauge:
    case DashboardWidgetKind::Numeric:
        break;
    }

    return {};
}

DashboardWidget defaultWidgetOf(DashboardWidgetKind kind)
{
    DashboardWidget widget;
    widget.kind = kind;

    const DashboardSize size = defaultSizeOf(kind);
    widget.width = size.width;
    widget.height = size.height;

    switch (kind) {
    // A control needs a variable to write and a reader needs something to read. Neither is
    // guessed: the widget arrives unbound and validate() says so, which is the same contract a
    // half-configured block on the canvas has.
    case DashboardWidgetKind::Slider:
    case DashboardWidgetKind::Knob:
    case DashboardWidgetKind::Lamp:
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
        widget.maximum = 1.0;
        break;

    case DashboardWidgetKind::Label:
        widget.title = "Label";
        break;

    case DashboardWidgetKind::Group:
        widget.title = "Group";
        break;

    case DashboardWidgetKind::Faults:
        widget.title = "Faults";
        break;

    case DashboardWidgetKind::Cluster:
        widget.profile = std::string{kDefaultClusterProfile};
        break;

    case DashboardWidgetKind::Gauge:
    case DashboardWidgetKind::Numeric:
    case DashboardWidgetKind::Bar:
    case DashboardWidgetKind::State:
    case DashboardWidgetKind::Image:
    case DashboardWidgetKind::Trend:
    case DashboardWidgetKind::Table:
        break;
    }

    return widget;
}

void DashboardDescription::remove(const std::string& id)
{
    std::erase_if(m_widgets, [&id](const DashboardWidget& widget) { return widget.id == id; });
}

const DashboardWidget* DashboardDescription::find(const std::string& id) const
{
    const auto found =
        std::find_if(m_widgets.begin(), m_widgets.end(), [&id](const DashboardWidget& widget) {
            return widget.id == id;
        });

    return found == m_widgets.end() ? nullptr : &*found;
}

void DashboardDescription::clear()
{
    m_name.clear();
    m_widgets.clear();
}

std::string DashboardDescription::uniqueId(std::string_view base) const
{
    std::string candidate{base};

    if (find(candidate) == nullptr) {
        return candidate;
    }

    // From 2, not from 1: the first one is "gauge", so the next is "gauge_2".
    // Starting at 1 would produce "gauge" and "gauge_1", which reads as though
    // one of them came first and the other is a copy of it.
    for (int suffix = 2; suffix < 10'000; ++suffix) {
        candidate = std::format("{}_{}", base, suffix);

        if (find(candidate) == nullptr) {
            return candidate;
        }
    }

    return candidate;
}

std::string DashboardDescription::duplicate(const std::string& id, double offset)
{
    const DashboardWidget* original = find(id);

    if (original == nullptr) {
        return {};
    }

    // A copy made before the push: the vector may move its elements, and `original` with them.
    DashboardWidget copy = *original;
    copy.id = uniqueId(withoutNumberSuffix(id));
    copy.x += offset;
    copy.y += offset;

    std::string newId = copy.id;
    m_widgets.push_back(std::move(copy));

    return newId;
}

void DashboardDescription::bringToFront(const std::string& id)
{
    const auto found =
        std::find_if(m_widgets.begin(), m_widgets.end(), [&id](const DashboardWidget& widget) {
            return widget.id == id;
        });

    if (found != m_widgets.end()) {
        std::rotate(found, std::next(found), m_widgets.end());
    }
}

void DashboardDescription::sendToBack(const std::string& id)
{
    const auto found =
        std::find_if(m_widgets.begin(), m_widgets.end(), [&id](const DashboardWidget& widget) {
            return widget.id == id;
        });

    if (found != m_widgets.end()) {
        std::rotate(m_widgets.begin(), found, std::next(found));
    }
}

Result DashboardDescription::validate() const
{
    std::set<std::string> seen;

    for (const DashboardWidget& widget : m_widgets) {
        if (widget.id.empty()) {
            return Result::error(ErrorCode::InvalidArgument, "A dashboard widget has no id");
        }

        if (!seen.insert(widget.id).second) {
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("Two dashboard widgets share the id '{}'", widget.id));
        }

        if (widget.width <= 0.0 || widget.height <= 0.0) {
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("Widget '{}' has no size", widget.id));
        }

        // A Cluster is held to its profile instead of to a binding: it shows dozens of values and
        // names none of them, so a profile this build does not have is the one way it can be bound
        // to nothing.
        if (widget.kind == DashboardWidgetKind::Cluster
            && ClusterProfiles::instance().find(widget.profile) == nullptr) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' is a cluster fed by the profile '{}', which this build "
                            "does not have",
                            widget.id,
                            widget.profile));
        }

        const DashboardKindTraits& traits = traitsOf(widget.kind);

        if (traits.binding == BindingNeed::Required
            && widget.binding.source == DashboardBinding::Source::None) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' is not bound to anything. A gauge bound to "
                            "nothing is a picture of a gauge.",
                            widget.id));
        }

        // A table lists the signals of one message, and a signal or a variable is not that.
        if (traits.binding == BindingNeed::Message
            && widget.binding.source != DashboardBinding::Source::Message) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' is a table of one message and is not bound to one. Pick "
                            "the message whose signals it should list.",
                            widget.id));
        }

        switch (widget.binding.source) {
        case DashboardBinding::Source::Signal:
            if (widget.binding.message.empty() || widget.binding.signal.empty()) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' names a signal without its message. Signal "
                                "names are only unique within a message.",
                                widget.id));
            }

            // The refusal that saves an afternoon: a slider bound to a signal
            // looks reasonable, moves, and changes nothing on the bus. Said
            // here, where the binding was chosen.
            if (writesItsBinding(widget.kind)) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' is a control bound to a CAN signal, which "
                                "it cannot write. Bind it to a variable and let a "
                                "script or a transmit entry put that on the bus.",
                                widget.id));
            }
            break;

        case DashboardBinding::Source::Variable:
            if (widget.binding.variable.empty()) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' is bound to a variable with no name", widget.id));
            }
            break;

        case DashboardBinding::Source::Message:
            if (widget.binding.message.empty()) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' is bound to a message with no name", widget.id));
            }

            if (traits.binding != BindingNeed::Message) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' is bound to a whole message, which only a table can "
                                "show. Pick one of its signals.",
                                widget.id));
            }
            break;

        case DashboardBinding::Source::None:
            break;
        }

        // Ranges matter to the kinds that sweep one. A Label has no range and a
        // Lamp only has a threshold, so neither is held to this; a Cluster has a
        // range per role, in its profile and in its own scales.
        if (traits.range && !(widget.maximum > widget.minimum)) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' has a range of {} to {}, which a needle cannot "
                            "sweep",
                            widget.id,
                            widget.minimum,
                            widget.maximum));
        }

        if (widget.decimals < 0 || widget.decimals > 6) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' asks for {} decimals", widget.id, widget.decimals));
        }

        // --- States ------------------------------------------------------------------------
        //
        // Two names for one value is a table that answers differently depending on which row it
        // reads first. Sorted and compared with the neighbour, so that a project with a thousand
        // states is not a thousand squared; and a value that is not a number cannot be sorted, nor
        // matched, nor typed - it is refused instead.
        if (!widget.states.empty()) {
            std::vector<double> values;
            values.reserve(widget.states.size());

            for (const DashboardState& state : widget.states) {
                if (!std::isfinite(state.value)) {
                    return Result::error(
                        ErrorCode::InvalidArgument,
                        std::format("Widget '{}' has a state, \"{}\", whose value is not a number",
                                    widget.id,
                                    state.label));
                }

                values.push_back(state.value);
            }

            std::sort(values.begin(), values.end());

            if (const auto twice = std::adjacent_find(values.begin(), values.end());
                twice != values.end()) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Widget '{}' names the value {} twice", widget.id, *twice));
            }
        }

        // --- Alarm limits ------------------------------------------------------------------
        //
        // Each pair has to leave a normal band between them, and a critical limit has to lie
        // beyond the warning one it is meant to follow. Otherwise a reading could be critical
        // without ever having been a warning, or the whole range could be an alarm.
        const DashboardLimits& limits = widget.limits;

        const auto contradicts = [&widget](std::string_view what) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' has alarm limits that contradict each other: {}",
                            widget.id,
                            what));
        };

        const auto finite = [](const std::optional<double>& limit) {
            return !limit.has_value() || std::isfinite(*limit);
        };

        if (!finite(limits.warningBelow) || !finite(limits.warningAbove)
            || !finite(limits.criticalBelow) || !finite(limits.criticalAbove)) {
            return contradicts("a limit is not a number");
        }

        if (limits.warningBelow && limits.warningAbove
            && !(*limits.warningBelow < *limits.warningAbove)) {
            return contradicts("the warning limit below is not under the one above");
        }

        if (limits.criticalBelow && limits.criticalAbove
            && !(*limits.criticalBelow < *limits.criticalAbove)) {
            return contradicts("the critical limit below is not under the one above");
        }

        if (limits.warningAbove && limits.criticalAbove
            && *limits.criticalAbove < *limits.warningAbove) {
            return contradicts("the critical limit above is under the warning one");
        }

        if (limits.warningBelow && limits.criticalBelow
            && *limits.criticalBelow > *limits.warningBelow) {
            return contradicts("the critical limit below is over the warning one");
        }

        // --- A trend's window, and how long a signal may go quiet ---------------------------
        if (traits.window && !(widget.windowSeconds >= 1.0 && widget.windowSeconds <= 3600.0)) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' draws {} seconds, which is outside 1 to 3600",
                            widget.id,
                            widget.windowSeconds));
        }

        if (widget.staleMs > 3'600'000U) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' waits {} ms before calling a signal stale, which is more "
                            "than an hour",
                            widget.id,
                            widget.staleMs));
        }
    }

    return Result::ok();
}

} // namespace torquebus
