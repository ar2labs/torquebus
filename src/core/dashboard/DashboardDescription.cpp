// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/DashboardDescription.h"

#include "core/dashboard/cluster/ClusterProfiles.h"

#include <algorithm>
#include <format>
#include <set>

namespace torquebus {

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
        return false;
    }

    return false;
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

        // A Label is the one kind with nothing to show but itself, and a Cluster has its profile.
        const bool needsBinding = widget.kind != DashboardWidgetKind::Label
                                  && widget.kind != DashboardWidgetKind::Cluster;

        if (needsBinding && widget.binding.source == DashboardBinding::Source::None) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Widget '{}' is not bound to anything. A gauge bound to "
                            "nothing is a picture of a gauge.",
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

        case DashboardBinding::Source::None:
            break;
        }

        // Ranges matter to the kinds that sweep one. A Label has no range and a
        // Lamp only has a threshold, so neither is held to this; a Cluster has a
        // range per role, in its profile and in its own scales.
        const bool needsRange = widget.kind != DashboardWidgetKind::Label
                                && widget.kind != DashboardWidgetKind::Lamp
                                && widget.kind != DashboardWidgetKind::Cluster;

        if (needsRange && !(widget.maximum > widget.minimum)) {
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
    }

    return Result::ok();
}

} // namespace torquebus
