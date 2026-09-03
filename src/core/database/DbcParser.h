// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Reads a Vector .dbc file into a CanDatabase.
//
// What this parser handles, which is what real files in this domain contain:
//
//   VERSION           the version string
//   BU_               the node list
//   BO_ / SG_         messages and their signals, including multiplexing
//   VAL_              value tables, the thing that turns a 3 into "Reverse"
//   CM_               comments on a message, a signal or a node
//   BA_DEF_ / BA_     attributes; GenMsgCycleTime is read, the rest are kept
//                     as text so nothing is lost on a round trip
//   SIG_VALTYPE_      IEEE 754 float and double signals
//
// Everything else - environment variables, signal groups, the NS_ section - is
// skipped. Those sections exist, but nothing downstream of here reads them.
//
// **Unknown sections are skipped; malformed known ones are an error.** A .dbc
// with a section this parser has never heard of should still load, because the
// format grows and files are shared between tools. But an `SG_` line that does
// not parse is not a section this parser is ignoring - it is a signal that
// would silently go missing, and a missing signal is a panel that shows nothing
// with no explanation. Those fail the load and name the line.

#pragma once

#include "core/Result.h"
#include "core/database/CanMessage.h"

#include <string>
#include <string_view>

namespace torquebus {

class DbcParser final {
public:
    /// Parses the contents of a .dbc file.
    ///
    /// On failure `database` is left untouched, so a reload that fails does not
    /// destroy the database that was working.
    [[nodiscard]] static Result parse(std::string_view text, CanDatabase& database);

    /// Reads and parses a file. Sets `database.sourcePath`.
    [[nodiscard]] static Result parseFile(const std::string& path, CanDatabase& database);

    /// The conventional extension, without the dot.
    [[nodiscard]] static constexpr std::string_view extension() noexcept { return "dbc"; }
};

} // namespace torquebus
