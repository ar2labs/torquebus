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

--- CRC-8 over a byte string, SAE J1850: polynomial 0x1D, initial value 0xFF,
--- final XOR 0xFF. The one AUTOSAR's end-to-end profiles 1 and 2 use, and the
--- one most vehicle messages carry.
---
--- Here so that a script can build a message that is *correct*, which is the
--- prerequisite for making one deliberately wrong: flipping a bit in a checksum
--- nobody computed proves nothing.
function tb.crc8(bytes, start)
    local crc = start or 0xFF

    for index = 1, #bytes do
        crc = crc ~ bytes:byte(index)

        for _ = 1, 8 do
            if (crc & 0x80) ~= 0 then
                crc = ((crc << 1) ~ 0x1D) & 0xFF
            else
                crc = (crc << 1) & 0xFF
            end
        end
    end

    return crc ~ 0xFF
end

--- The E2E profile 1 shape most messages use: a CRC in the first byte over
--- everything after it, and a counter in the low nibble of the second.
---
--- Returns the whole payload, ready to send:
---     cyclic(0x123, 10, function()
---         return tb.e2e(string.pack("<I2", speed()), counter())
---     end)
function tb.e2e(payload, counter)
    local body = string.char(counter & 0x0F) .. payload
    return string.char(tb.crc8(body)) .. body
end

--- Reads a generator or a plain value, so a caller can accept either.
--- tb.value(42) is 42; tb.value(tb.ramp(0, 100)) is where the ramp is now.
function tb.value(source)
    if type(source) == "function" then
        return source()
    end
    return source
end

--------------------------------------------------------------------------------
-- J1939 helpers
--
-- The vocabulary of a J1939 ECU script: an identifier from a PGN and an address, a physical value
-- as the raw number the standard puts on the wire, a DM1 payload, and a message padded the way
-- J1939 pads one. Pure arithmetic over what emit() already does, which is why they live here.
--------------------------------------------------------------------------------

-- The biggest raw value that is a measurement for a field of that many bytes, and the one that says
-- there is none ("not available"). 0xFB.. to 0xFE.. are the standard's, for error and reserved.
local _J1939_MAXV = { [1] = 0xFA, [2] = 0xFAFF, [3] = 0xFAFFFF, [4] = 0xFAFFFFFF }
local _J1939_NA   = { [1] = 0xFF, [2] = 0xFFFF, [3] = 0xFFFFFF, [4] = 0xFFFFFFFF }

--- The 29-bit identifier of a J1939 message from its priority, PGN and source address.
function j1939_id(prio, pgn, sa)
    return (((prio or 6) & 0x7) << 26) | (((pgn or 0) & 0x3FFFF) << 8) | ((sa or 0) & 0xFF)
end
tb.j1939_id = j1939_id

--- A physical value as the raw number J1939 sends: round((value - offset) / res), held to the
--- biggest valid value of an `nbytes` field rather than wrapped into the ones that mean an error.
--- nil - and a number that is not one - is "not available", which is how a script says a sensor
--- has failed: j1939_raw(nil, 1, -40, 1) is 0xFF.
function j1939_raw(value, res, offset, nbytes)
    nbytes = nbytes or 1
    if value == nil or value ~= value then
        return _J1939_NA[nbytes] or 0xFF
    end
    local r = math.floor((value - (offset or 0)) / (res or 1) + 0.5)
    local maxv = _J1939_MAXV[nbytes] or 0xFA
    if r < 0 then r = 0 elseif r > maxv then r = maxv end
    return r
end
tb.j1939_raw = j1939_raw
raw = j1939_raw -- the short name the example ECUs use

-- What `send` is before this file touches it: nothing in an ECU, the node's own binding in a test
-- sequence. Captured here, before `send` is defined below, and used by j1939_send.
local _native_send = send

--- Sends a J1939 message from an ECU, or from a test sequence: the payload padded with 0xFF up to
--- eight bytes, which is what J1939 calls "not available" and what an unused byte has to say.
function j1939_send(prio, pgn, sa, payload)
    local id = j1939_id(prio, pgn, sa)
    payload = payload or ""
    if #payload < 8 then
        payload = payload .. string.rep("\xFF", 8 - #payload)
    end

    -- Always told it is extended: a high-priority message from a low address can have an identifier
    -- that fits in eleven bits, and would otherwise leave as a standard frame.
    if emit then
        return emit(id, payload, { extended = true })
    end
    return _native_send(id, payload, { extended = true })
end
tb.j1939_send = j1939_send

-- `send` means the node's own binding wherever there is one: in a test sequence it is
-- send(id, payload [, options]) and has been since before any of this, so it is left alone there.
-- An ECU has emit() and no send(), so the name is free in it, and there it is the J1939 message
-- above - or, with the arguments of the old call, emit(): send(id, payload [, options]).
if not _native_send then
    function send(a, b, c, d)
        if d == nil and (c == nil or type(c) == "table") then
            return emit(a, b, c)
        end
        return j1939_send(a, b, c, d)
    end
end
tb.send = send

--- The payload of a DM1 with one trouble code: the four lamps (0 off, 1 on), the flash bytes not
--- used, SPN and FMI, and an occurrence count. dm1(0, 0, 0, 0, 0, 0, 0) is J1939's "no active fault".
function j1939_dm1(mil, red, amber, protect, spn, fmi, oc)
    local lamps = (((mil or 0) & 3) << 6) | (((red or 0) & 3) << 4) | (((amber or 0) & 3) << 2) | ((protect or 0) & 3)
    spn = spn or 0
    fmi = fmi or 0
    oc = oc or 0
    return string.pack("<I1I1I1I1I1I1I1I1",
        lamps, 0xFF,
        spn & 0xFF, (spn >> 8) & 0xFF,
        ((spn >> 11) & 0xE0) | (fmi & 0x1F),
        oc & 0x7F, 0xFF, 0xFF)
end
tb.j1939_dm1 = j1939_dm1
dm1 = j1939_dm1 -- the short name the example ECUs use
tb.dm1 = j1939_dm1
)LUA";

} // namespace torquebus
