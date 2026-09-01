// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A node's settings, as data.
//
// Why not just constructor arguments: because a canvas cannot call a C++
// constructor, and neither can a project file. The moment the user is the one
// choosing what a node is - which channel, which script, which identifier range
// - the settings have to survive a round trip through a file and a properties
// panel, and that means they have to be named values rather than positional
// arguments.
//
// Deliberately not QVariant: the core is Qt-free (rule #3), and a headless
// measurement run from a script must not drag in Qt::Core. Four types cover
// every parameter a node has needed so far, and a fifth would be a sign that
// something belongs in its own structure rather than in a parameter bag.

#pragma once

#include "core/Result.h"

#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// One setting: a boolean, an integer, a real number or a string.
class ParameterValue final {
public:
    enum class Type : std::uint8_t { Boolean, Integer, Real, Text };

    ParameterValue() = default;

    static ParameterValue fromBoolean(bool value)
    {
        ParameterValue result;
        result.m_type = Type::Boolean;
        result.m_integer = value ? 1 : 0;
        return result;
    }

    static ParameterValue fromInteger(std::int64_t value)
    {
        ParameterValue result;
        result.m_type = Type::Integer;
        result.m_integer = value;
        return result;
    }

    static ParameterValue fromReal(double value)
    {
        ParameterValue result;
        result.m_type = Type::Real;
        result.m_real = value;
        return result;
    }

    static ParameterValue fromText(std::string value)
    {
        ParameterValue result;
        result.m_type = Type::Text;
        result.m_text = std::move(value);
        return result;
    }

    [[nodiscard]] Type type() const noexcept { return m_type; }

    [[nodiscard]] bool asBoolean() const noexcept { return m_integer != 0; }
    [[nodiscard]] std::int64_t asInteger() const noexcept { return m_integer; }
    [[nodiscard]] double asReal() const noexcept
    {
        // An integer read as a real is the common case of a user typing "5"
        // where a rate was expected, and refusing it would be pedantry.
        return m_type == Type::Real ? m_real : static_cast<double>(m_integer);
    }
    [[nodiscard]] const std::string& asText() const noexcept { return m_text; }

    [[nodiscard]] friend bool operator==(const ParameterValue&, const ParameterValue&) = default;

private:
    Type m_type{Type::Integer};
    std::int64_t m_integer{0};
    double m_real{0.0};
    std::string m_text;
};

/// What a node type accepts, so the properties panel can build itself and the
/// canvas can show a new node's settings before anything is instantiated.
struct ParameterDescriptor final {
    std::string_view name;
    std::string_view displayName;
    ParameterValue::Type type{ParameterValue::Type::Integer};

    /// False when the node cannot be built without it. A missing required
    /// parameter fails the build with a message naming the node and the
    /// parameter, rather than silently constructing something inert.
    bool required{true};

    /// Shown in the properties panel. One sentence, in the user's terms.
    std::string_view description;
};

/// A node's settings, by name.
class NodeParameters final {
public:
    NodeParameters() = default;

    NodeParameters(std::initializer_list<std::pair<const std::string, ParameterValue>> values)
        : m_values{values}
    {
    }

    void set(std::string name, ParameterValue value)
    {
        m_values.insert_or_assign(std::move(name), std::move(value));
    }

    [[nodiscard]] bool contains(const std::string& name) const
    {
        return m_values.find(name) != m_values.end();
    }

    /// The value, or `fallback` when absent. For optional parameters.
    [[nodiscard]] bool boolean(const std::string& name, bool fallback = false) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? fallback : it->second.asBoolean();
    }

    [[nodiscard]] std::int64_t integer(const std::string& name, std::int64_t fallback = 0) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? fallback : it->second.asInteger();
    }

    [[nodiscard]] double real(const std::string& name, double fallback = 0.0) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? fallback : it->second.asReal();
    }

    [[nodiscard]] std::string text(const std::string& name, std::string fallback = {}) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? std::move(fallback) : it->second.asText();
    }

    /// Fails naming the node and the parameter, because "missing parameter" on
    /// its own sends the user looking through a graph of forty nodes.
    [[nodiscard]] Result require(const std::string& name, std::string_view nodeId) const
    {
        return contains(name)
            ? Result::ok()
            : Result::error(ErrorCode::InvalidArgument,
                            std::format("Node '{}' needs a value for '{}'", nodeId, name));
    }

    [[nodiscard]] const std::map<std::string, ParameterValue>& values() const noexcept
    {
        return m_values;
    }

    [[nodiscard]] friend bool operator==(const NodeParameters&, const NodeParameters&) = default;

private:
    /// Ordered, not hashed: a serialised project should produce the same bytes
    /// twice, so that a diff of two saved graphs shows what the user changed
    /// rather than what the allocator did.
    std::map<std::string, ParameterValue> m_values;
};

} // namespace torquebus
