// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Turning the numbers in a NAME into words.
//
// A NAME says function 3, manufacturer 33. Somebody reading a network panel
// wants "Transmission" and the name of a company. The numbers are useless on
// their own and the words are what the panel exists to show.
//
// --- Why this is a file and not a table in the source -----------------------
//
// Two reasons, and the second is the one that decides it.
//
// The first is correctness over time. Function numbers 0 to 127 are industry
// group independent, but 128 to 255 mean different things depending on the
// industry group *and* the vehicle system - the same number is one device on a
// tractor and another on a boat. The manufacturer list is a registry of roughly
// two thousand entries that gains more every year. Compiled in, it is wrong the
// month after release and stays wrong until somebody rebuilds.
//
// The second is that **this data is not ours to ship.** It lives in the SAE
// J1939 Digital Annex, which is a licensed commercial product. Extracting it
// into a GPL repository would be redistributing it, whatever the repository
// says about its own licence. So TorqueBus ships the *mechanism* and no data,
// and somebody who has a licence to the Digital Annex converts their own copy
// on their own machine. See tools/j1939-names.py and the format below.
//
// A build with no file is not a broken build: every number is still shown, and
// the panel says "function 3" instead of "Transmission". That is worse and it
// is honest, which is the trade this file is here to make available rather than
// to make for anybody.
//
// --- The format -------------------------------------------------------------
//
// One entry per line, three fields, comma separated. Blank lines and lines
// starting with # are ignored.
//
//     industry,1,On-Highway Equipment
//     manufacturer,33,Some Manufacturer
//     function,0,Engine
//     function,1/1/129,Some Industry Specific Function
//
// A `function` key is either a bare number - the industry-group-independent
// range, 0 to 127 - or `industryGroup/vehicleSystem/function` for 128 to 255.
// Anything else on that line is the name, commas included, so a company called
// "Acme, Inc." needs no escaping.

#pragma once

#include "core/Result.h"
#include "core/j1939/J1939Name.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace torquebus {

/// The words behind the numbers in a NAME, loaded from a file.
class J1939NameTables final {
public:
    /// Reads `text` in the format described above.
    ///
    /// A malformed line is refused with its number, rather than skipped: a
    /// table that quietly drops half its rows is a table that answers some
    /// questions and not others, and nobody can tell which.
    [[nodiscard]] Result load(std::string_view text);

    /// Reads a file. A path that does not exist is **not** an error - it is
    /// what a machine with no licensed copy of the Digital Annex looks like,
    /// and the program works without one.
    [[nodiscard]] Result loadFile(const std::filesystem::path& path);

    /// The name of an industry group, when the file gives one.
    [[nodiscard]] std::optional<std::string_view> industryGroup(
        std::uint8_t group) const;

    /// The name of a manufacturer, when the file gives one.
    [[nodiscard]] std::optional<std::string_view> manufacturer(std::uint16_t code) const;

    /// The name of a function.
    ///
    /// Takes the whole NAME rather than the function number, because below 128
    /// the number is enough and at or above it the answer depends on the
    /// industry group and the vehicle system too. Passing just the number would
    /// be an interface that quietly gives the wrong answer for half the range.
    [[nodiscard]] std::optional<std::string_view> function(const J1939Name& name) const;

    [[nodiscard]] bool empty() const noexcept
    {
        return m_industryGroups.empty() && m_manufacturers.empty() && m_functions.empty();
    }

    [[nodiscard]] std::size_t industryGroupCount() const noexcept
    {
        return m_industryGroups.size();
    }

    [[nodiscard]] std::size_t manufacturerCount() const noexcept
    {
        return m_manufacturers.size();
    }

    [[nodiscard]] std::size_t functionCount() const noexcept { return m_functions.size(); }

    /// Where it was loaded from, for a panel to show and for a message to name.
    [[nodiscard]] const std::string& sourcePath() const noexcept { return m_sourcePath; }

    void clear();

private:
    /// Industry-group-independent functions are keyed on the number alone;
    /// the dependent ones on all three, packed.
    [[nodiscard]] static std::uint32_t functionKey(std::uint8_t group,
                                                   std::uint8_t vehicleSystem,
                                                   std::uint8_t function) noexcept
    {
        return (static_cast<std::uint32_t>(group) << 16U)
               | (static_cast<std::uint32_t>(vehicleSystem) << 8U) | function;
    }

    std::unordered_map<std::uint8_t, std::string> m_industryGroups;
    std::unordered_map<std::uint16_t, std::string> m_manufacturers;
    std::unordered_map<std::uint32_t, std::string> m_functions;

    std::string m_sourcePath;
};

/// The lowest function number whose meaning depends on the industry group and
/// the vehicle system. Below this, the number alone is the answer.
inline constexpr std::uint8_t kJ1939FirstDependentFunction = 128U;

} // namespace torquebus
