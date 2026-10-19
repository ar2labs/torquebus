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
#include "core/dashboard/DashboardFaults.h"
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

// --- The Faults widget ----------------------------------------------------------------------

/// A trouble code, as a technician writes one: "SPN 100 / FMI 1". An SPN that the J1939 block did
/// not assemble (a legacy packing nobody declared, see J1939Diagnostics.h) is a question mark and
/// not a number - a number read under the wrong packing still looks like an SPN - and the raw bytes
/// are in faultDetailsText.
[[nodiscard]] QString faultCodeText(const J1939Dtc& code);

/// The failure mode in words: "Data valid but below normal operational range - most severe level".
[[nodiscard]] QString faultMeaningText(const J1939Dtc& code);

/// How many times the ECU says the fault has occurred, with a multiplication sign in front: "x3".
/// 127 is the most the field holds and means "at least", so it is written "x127+".
[[nodiscard]] QString faultCountText(const J1939Dtc& code);

/// Which ECU: "SA 0", and after it - a middle dot and the name of its function - when the name
/// tables know one; and in front of it the bus, "CAN 2 · SA 0 · Engine", when the measurement has
/// more than one J1939 bus.
[[nodiscard]] QString faultEcuText(const DashboardFault& fault);

/// Everything the widget knows of one code, over a few lines, for a tooltip.
[[nodiscard]] QString faultDetailsText(const DashboardFault& fault);

} // namespace torquebus::ui
