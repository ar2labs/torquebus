// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A small standard library, written in Lua and run before every script.
//
// It is Lua rather than C++ on purpose. Everything here is arithmetic over the
// measurement clock - a ramp, a sine, a rolling counter - and a binding for
// each would be thirty lines of stack juggling to express three lines of maths.
// In Lua they are readable by the person using them, copyable into a script
// that wants a variant, and cost nothing to maintain.
//
// The rule for what belongs here: it must be *only* arithmetic and it must use
// nothing but what the node already exposes. Anything needing to reach into the
// node - emit, a DBC, a timer - is a binding, because a prelude that could do
// those things would be a second place where the node's behaviour lives.

#pragma once

#include <string_view>

namespace torquebus {

/// Run before the script itself, in the same interpreter.
inline constexpr std::string_view kLuaPrelude = R"LUA(
-- TorqueBus prelude. Everything here is plain Lua over get_time_us().

tb = tb or {}

--- Seconds since the measurement began, as a float.
function tb.now()
    return get_time_us() / 1e6
end

--- A value that sweeps from `low` to `high` and jumps back, once per `period`
--- seconds. The shape of a temperature climbing, a tank draining, a sweep test.
function tb.ramp(low, high, period)
    period = period or 10
    return function()
        local phase = (tb.now() % period) / period
        return low + (high - low) * phase
    end
end

--- A value that sweeps up and back down again, smoothly. Engine speed while
--- somebody blips the throttle, a temperature under thermostat control.
function tb.sine(low, high, period)
    period = period or 10
    local middle = (low + high) / 2
    local swing = (high - low) / 2

    return function()
        return middle + swing * math.sin(2 * math.pi * tb.now() / period)
    end
end

--- Low for half the period and high for the other half. A switch, a lamp, a
--- request that comes and goes.
function tb.square(low, high, period)
    period = period or 10
    return function()
        return ((tb.now() % period) < (period / 2)) and low or high
    end
end

--- A value that wanders rather than jumping: each call moves at most `step`
--- towards a new random target. Noise that looks like a sensor rather than
--- like a random number generator, which is the difference between a trace
--- somebody believes and one they do not.
function tb.drift(low, high, step)
    step = step or (high - low) / 20
    local value = (low + high) / 2

    return function()
        value = value + (math.random() * 2 - 1) * step

        if value < low then value = low end
        if value > high then value = high end

        return value
    end
end

--- Walks a list of values, one per `period` seconds. Gear positions, a state
--- machine's states, a sequence somebody wants repeated exactly.
function tb.steps(values, period)
    period = period or 1
    return function()
        local index = math.floor(tb.now() / period) % #values + 1
        return values[index]
    end
end

--- A counter that wraps, which nearly every real message carries so that a
--- receiver can tell a repeated frame from a fresh one.
function tb.counter(bits)
    bits = bits or 4
    local limit = 1 << bits
    local value = -1

    return function()
        value = (value + 1) % limit
        return value
    end
end

--- Reads a generator or a plain value, so a caller can accept either.
--- tb.value(42) is 42; tb.value(tb.ramp(0, 100)) is where the ramp is now.
function tb.value(source)
    if type(source) == "function" then
        return source()
    end
    return source
end
)LUA";

} // namespace torquebus
