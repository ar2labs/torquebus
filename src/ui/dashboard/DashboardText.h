// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The words the Dashboard says, in one place.
//
// The panel says them in a tooltip, the signal picker in its details pane, the widget form under
// the field it concerns and the Output panel when a project is opened - and a sentence that is
// built in four places is a sentence that is worded four ways. These are the sentences, and the
// names the kinds of widget go by, so that a Table is a Table wherever a person meets it.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"

#include <QString>

namespace torquebus::ui {

/// What a kind of widget is called to a person.
[[nodiscard]] QString displayNameOf(DashboardWidgetKind kind);

/// What a tone is called to a person.
[[nodiscard]] QString displayNameOf(DashboardTone tone);

/// What a signal is, in a line: "rpm, 0 to 8031.875, every 20 ms, from EMS". Only what the
/// database says - a signal with no unit and no cycle time is described by its range alone.
[[nodiscard]] QString summaryOf(const DictionaryEntry& entry);

/// Everything the database says of a signal, one fact to a line: its name, its comment, its unit
/// and range and scaling, how it is sent and by whom, and the names of its values.
[[nodiscard]] QString detailsOf(const DictionaryEntry& entry);

/// What is wrong with a binding that the databases do not bear out, and the nearest thing that
/// would be right: "EEC1 has no signal "EngineSped". Did you mean EngineSpeed?". Empty for a
/// finding that is not a problem.
[[nodiscard]] QString describeFinding(const BindingFinding& finding,
                                      const DashboardBinding& binding);

} // namespace torquebus::ui
