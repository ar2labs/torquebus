// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a Faults widget lists, before it is words.
//
// The J1939 block works out which trouble codes the bus is reporting and hands them over with its
// view of the bus (J1939Network.h). A widget wants rows: worst first, each with the ECU that
// reported it and what that ECU is called. The choices that make the rows are made here - which
// code is the worse one, what a lamp says about the codes of its message, what to call an ECU - and
// made once, in the core and without Qt, so that the widget, its hover text and a test cannot
// answer differently and a test needs no screen to ask.
//
// --- What a lamp says about a code ------------------------------------------------------------
//
// A lamp belongs to a DM1 and not to one code in it: an ECU that reports three codes in one message
// lights the same lamps for all three. So a row is as bad as the lamps of the message it came from,
// and no worse - the standard has no way to say that one of the three is the reason the red lamp is
// lit, and a widget that guessed would be inventing a severity per code.
//
//   * the red stop lamp is **Danger**: the vehicle is to be stopped;
//   * any other lit lamp (amber warning, malfunction indicator, protect) is **Warning**;
//   * a message whose lamps are all dark has no opinion and its codes are **Neutral**. Real ECUs do
//     report a code with every lamp dark - a logged fault that is not yet worth telling the driver
//     - and drawing it in a colour of alarm would be saying more than the ECU did.
//
// --- The lamps of the widget ------------------------------------------------------------------
//
// The four lamps at the top of the widget are the lamps of the whole bus, which the J1939 block
// writes to system variables (J1939Diagnostics.h) and the instrument cluster reads. The widget
// reads the same variables, so the lamp on the cluster and the lamp on the list cannot disagree.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// The system variables behind the four lamps of the widget, in the order it draws them: stop,
/// warning, malfunction indicator, protect.
inline constexpr std::array<std::string_view, 4> kFaultLampVariables{
    kJ1939LampStopVariable,
    kJ1939LampWarningVariable,
    kJ1939LampMilVariable,
    kJ1939LampProtectVariable,
};

/// What the lamps of one DM1 say about the codes it carries. See the note at the top of this file.
[[nodiscard]] DashboardTone toneOfLamps(const J1939Lamps& lamps) noexcept;

/// One row of a Faults widget, before it is words.
struct DashboardFault final {
    std::uint8_t sourceAddress{0U};

    /// What the J1939 name tables call the ECU's function - "Engine" - when the ECU has been seen
    /// claiming its address and the machine has a table that names the function. Absent otherwise,
    /// and the address is then all there is to say: a number that is known to be right is better
    /// than a name that is a guess.
    std::optional<std::string> ecu;

    J1939Dtc code;

    /// What the lamps of the message the code arrived in say about it.
    DashboardTone tone{DashboardTone::Neutral};

    [[nodiscard]] friend bool operator==(const DashboardFault&, const DashboardFault&) = default;
};

/// The codes the bus is reporting in `snapshot`, worst first: the codes of a message that lights
/// the stop lamp, then those of one that lights any other lamp, then the rest. Within one of those
/// the order is the snapshot's - ECU by ECU, address ascending, and within an ECU the order it sent
/// them in - so the list does not shuffle from one pass to the next.
///
/// `names` may be null, for a machine with no name tables: every ECU is then a number.
[[nodiscard]] std::vector<DashboardFault> dashboardFaultsOf(const J1939NetworkSnapshot& snapshot,
                                                            const J1939NameTables* names);

} // namespace torquebus
