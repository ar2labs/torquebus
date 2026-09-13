// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939NameTables.h"

#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

namespace torquebus {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }

    return text;
}

/// Parses a whole decimal number, and only a whole one: "12x" is refused
/// rather than read as 12, because a key that was mistyped must not silently
/// name a different thing.
[[nodiscard]] bool parseNumber(std::string_view text, std::uint32_t& out) noexcept
{
    text = trim(text);
    if (text.empty()) {
        return false;
    }

    const char* const begin = text.data();
    const char* const end = begin + text.size();

    const std::from_chars_result result = std::from_chars(begin, end, out);

    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

Result J1939NameTables::load(std::string_view text)
{
    clear();

    std::istringstream stream{std::string{text}};
    std::string line;
    std::size_t number = 0;

    while (std::getline(stream, line)) {
        ++number;

        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const std::size_t firstComma = trimmed.find(',');
        const std::size_t secondComma =
            firstComma == std::string_view::npos ? std::string_view::npos
                                                 : trimmed.find(',', firstComma + 1);

        if (secondComma == std::string_view::npos) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Line {}: expected three fields separated by commas", number));
        }

        const std::string_view kind = trim(trimmed.substr(0, firstComma));
        const std::string_view key =
            trim(trimmed.substr(firstComma + 1, secondComma - firstComma - 1));

        // Everything after the second comma, commas included: a company called
        // "Acme, Inc." needs no escaping and nobody has to remember a rule.
        const std::string_view name = trim(trimmed.substr(secondComma + 1));

        if (name.empty()) {
            return Result::error(ErrorCode::InvalidArgument,
                                 std::format("Line {}: no name", number));
        }

        std::uint32_t value = 0;

        if (kind == "industry") {
            if (!parseNumber(key, value) || value > 7U) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Line {}: '{}' is not an industry group, which is 0 to 7",
                                number, key));
            }

            m_industryGroups[static_cast<std::uint8_t>(value)] = std::string{name};
            continue;
        }

        if (kind == "manufacturer") {
            if (!parseNumber(key, value) || value > 0x7FFU) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Line {}: '{}' is not a manufacturer code, which is 0 to 2047",
                                number, key));
            }

            m_manufacturers[static_cast<std::uint16_t>(value)] = std::string{name};
            continue;
        }

        if (kind == "function") {
            // A bare number is the industry-group-independent range.
            if (parseNumber(key, value)) {
                if (value >= kJ1939FirstDependentFunction) {
                    return Result::error(
                        ErrorCode::InvalidArgument,
                        std::format("Line {}: function {} is at or above {}, so it needs an "
                                    "industryGroup/vehicleSystem/function key - the number "
                                    "alone means different things on different machines",
                                    number, value, kJ1939FirstDependentFunction));
                }

                m_functions[functionKey(0U, 0U, static_cast<std::uint8_t>(value))] =
                    std::string{name};
                continue;
            }

            const std::size_t firstSlash = key.find('/');
            const std::size_t secondSlash =
                firstSlash == std::string_view::npos ? std::string_view::npos
                                                     : key.find('/', firstSlash + 1);

            std::uint32_t group = 0;
            std::uint32_t vehicleSystem = 0;
            std::uint32_t function = 0;

            if (secondSlash == std::string_view::npos
                || !parseNumber(key.substr(0, firstSlash), group)
                || !parseNumber(key.substr(firstSlash + 1, secondSlash - firstSlash - 1),
                                vehicleSystem)
                || !parseNumber(key.substr(secondSlash + 1), function) || group > 7U
                || vehicleSystem > 0x7FU || function > 0xFFU) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Line {}: '{}' is not a function key. Use a number below "
                                "{}, or industryGroup/vehicleSystem/function",
                                number, key, kJ1939FirstDependentFunction));
            }

            m_functions[functionKey(static_cast<std::uint8_t>(group),
                                    static_cast<std::uint8_t>(vehicleSystem),
                                    static_cast<std::uint8_t>(function))] = std::string{name};
            continue;
        }

        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Line {}: '{}' is not industry, manufacturer or function", number,
                        kind));
    }

    return Result::ok();
}

Result J1939NameTables::loadFile(const std::filesystem::path& path)
{
    clear();

    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) {
        // Not an error. It is what a machine without a licensed copy of the
        // Digital Annex looks like, and everything still works - with numbers
        // where the words would be.
        return Result::ok();
    }

    std::ifstream file{path};
    if (!file) {
        return Result::error(ErrorCode::InvalidArgument,
                             std::format("Could not open {}", path.string()));
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    if (Result result = load(buffer.str()); result.failed()) {
        // The line number is in the message already; the file name is what
        // turns it into something somebody can open.
        return Result::error(result.code(),
                             std::format("{}: {}", path.string(), result.message()));
    }

    m_sourcePath = path.string();
    return Result::ok();
}

std::optional<std::string_view> J1939NameTables::industryGroup(std::uint8_t group) const
{
    const auto found = m_industryGroups.find(group);

    return found == m_industryGroups.end() ? std::nullopt
                                           : std::optional<std::string_view>{found->second};
}

std::optional<std::string_view> J1939NameTables::manufacturer(std::uint16_t code) const
{
    const auto found = m_manufacturers.find(code);

    return found == m_manufacturers.end() ? std::nullopt
                                          : std::optional<std::string_view>{found->second};
}

std::optional<std::string_view> J1939NameTables::function(const J1939Name& name) const
{
    // Below 128 the number is the whole answer; at or above it the same number
    // is a different device on a different kind of machine, so the industry
    // group and vehicle system are part of the question.
    const std::uint32_t key =
        name.function < kJ1939FirstDependentFunction
            ? functionKey(0U, 0U, name.function)
            : functionKey(name.industryGroup, name.vehicleSystem, name.function);

    const auto found = m_functions.find(key);

    return found == m_functions.end() ? std::nullopt
                                      : std::optional<std::string_view>{found->second};
}

void J1939NameTables::clear()
{
    m_industryGroups.clear();
    m_manufacturers.clear();
    m_functions.clear();
    m_sourcePath.clear();
}

} // namespace torquebus
