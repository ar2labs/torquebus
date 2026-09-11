// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The test framework, written in Lua and run before every sequence.
//
// A test reads sequentially - send this, wait up to 200 ms for that, check the
// counter - and the executor calls a node once per pass and must never be
// blocked. Those two facts are irreconcilable in C++ without a thread per test,
// and they are reconciled for free by the thing Lua already has: **each case
// runs in a coroutine.** `expect` yields, the node resumes it on the next pass,
// and the script is written in the order it happens.
//
// That is why the framework lives here rather than in C++. The alternative -
// a state machine in the node, driven by callbacks - would express the same
// sequence as `if state == 3 then`, which is exactly the shape a test is worth
// having in order to avoid.
//
// The division of labour with the node:
//
//   * Lua owns sequencing: which case is running, what it is waiting for, what
//     to do when a check fails.
//   * C++ owns the bus and the clock: matching a frame against what a case
//     asked for, and knowing when a deadline has passed.
//
// Nothing crosses the boundary except through the `__tb_*` bindings, and no
// frame crosses at all unless a case asked for one. A sequence waiting on a
// 500 ms timeout costs one Lua call per pass and nothing per frame.

#pragma once

#include <string_view>

namespace torquebus {

/// Run before the sequence itself, in the same interpreter.
inline constexpr std::string_view kLuaTestPrelude = R"LUA(
-- TorqueBus test framework.
--
-- Everything public is documented in docs/development/testing.md. The `__tb_`
-- names are the node's, and a sequence that calls one directly is relying on
-- something that is not a promise.

local cases = {}
local current = nil
local index = 0

--- Declares a test case. They run in the order they were declared, one at a
--- time - a network is a shared thing, and two cases driving it at once would
--- make each one's result depend on the other's timing.
function test(name, body)
    if type(name) ~= "string" then
        error("test() wants a name as its first argument", 2)
    end
    if type(body) ~= "function" then
        error("test('" .. tostring(name) .. "') wants a function as its second argument", 2)
    end

    cases[#cases + 1] = { name = name, body = body }
end

--- How many cases were declared. Read once, at Start, so a report can say
--- "3 of 8" while a run is in progress rather than "3 of 3, all passed".
function __tb_case_count()
    return #cases
end

-- --- Checks --------------------------------------------------------------
--
-- Every check is recorded, passed or failed. A report listing only failures
-- cannot be read as evidence that anything was checked, and "17 checks, all
-- passed" is the sentence somebody signs off on.

local function record(passed, description, detail)
    __tb_check(description, passed, detail or "")

    if not passed then
        -- Ends this case and no more. One requirement not being met says
        -- nothing about the others, and a run that stopped at the first
        -- failure would have to be repeated once per bug.
        error({ __tb_failure = true, message = description ..
                (detail ~= nil and detail ~= "" and (" - " .. detail) or "") }, 0)
    end
end

local function shown(value)
    if type(value) == "string" then
        -- Payloads are byte strings and most of their bytes are not letters.
        -- Shown as hex unless every byte is printable, because `\x00\xff` read
        -- back as two boxes on a screen tells nobody anything.
        local printable = #value > 0
        for i = 1, #value do
            local byte = value:byte(i)
            if byte < 0x20 or byte > 0x7E then printable = false break end
        end

        if printable then return "'" .. value .. "'" end

        local parts = {}
        for i = 1, #value do parts[#parts + 1] = string.format("%02X", value:byte(i)) end
        return #parts == 0 and "(empty)" or table.concat(parts, " ")
    end

    return tostring(value)
end

function assert_true(condition, description)
    record(condition and true or false, description or "expected to be true",
           condition and "" or "was " .. shown(condition))
end

function assert_equal(actual, expected, description)
    record(actual == expected, description or "expected " .. shown(expected),
           actual == expected and ""
               or ("expected " .. shown(expected) .. ", saw " .. shown(actual)))
end

function assert_near(actual, expected, tolerance, description)
    tolerance = tolerance or 0.001

    local ok = type(actual) == "number" and math.abs(actual - expected) <= tolerance
    record(ok, description or ("expected about " .. shown(expected)),
           ok and "" or ("expected " .. shown(expected) .. " +/- " .. shown(tolerance) ..
                         ", saw " .. shown(actual)))
end

function assert_between(actual, low, high, description)
    local ok = type(actual) == "number" and actual >= low and actual <= high
    record(ok, description or ("expected between " .. shown(low) .. " and " .. shown(high)),
           ok and "" or ("expected " .. shown(low) .. ".." .. shown(high) ..
                         ", saw " .. shown(actual)))
end

--- Fails the case where it is written. For the branch that should not have
--- been reached - which is a real check and reads badly as assert_true(false).
function fail(message)
    record(false, message or "failed", "")
end

-- --- Waiting -------------------------------------------------------------

--- Waits `milliseconds`, letting the measurement run.
function wait(milliseconds)
    __tb_want_time(milliseconds)
    while not __tb_ready() do coroutine.yield() end
end

--- Waits for a frame, and returns it: `{ id = , data = , channel = ,
--- extended = }`. Returns nil if none arrived within the timeout - which is a
--- fact about the network, not an error, so a case can test for silence:
---
---     local frame = expect(0x100, { within = 200 })
---     assert_true(frame ~= nil, "0x100 arrives within 200 ms")
---
--- `where` filters further: expect(0x100, { where = function(f)
---     return f.data:byte(1) == 0x42 end })
function expect(identifier, options)
    options = options or {}

    __tb_want_frame(identifier, options.within or 1000, options.channel)

    while true do
        if __tb_ready() then
            local frame = __tb_taken()

            if frame == nil then
                return nil                       -- the deadline passed
            end

            if options.where == nil or options.where(frame) then
                return frame
            end

            -- Not the one this case meant. Keep the same deadline: "within
            -- 200 ms" is about the wait, not about each candidate frame.
            __tb_want_again()
        end

        coroutine.yield()
    end
end

--- expect() plus the assertion nearly every use of it is followed by.
function expect_frame(identifier, options)
    options = options or {}

    local frame = expect(identifier, options)

    record(frame ~= nil,
           string.format("0x%X arrives within %d ms", identifier, options.within or 1000),
           frame ~= nil and "" or "nothing arrived")

    return frame
end

--- The other way round, and the one most tools cannot express: nothing on this
--- identifier for this long. A silenced ECU, a message that must stop when the
--- ignition goes off, a request that must not be answered twice.
function expect_silence(identifier, milliseconds, description)
    milliseconds = milliseconds or 200

    local frame = expect(identifier, { within = milliseconds })

    record(frame == nil,
           description or string.format("0x%X stays silent for %d ms", identifier,
                                        milliseconds),
           frame == nil and "" or "a frame arrived")
end

-- --- Sending -------------------------------------------------------------
--
-- `send` is the node's binding; it is named here so the documentation has one
-- place to point at.

-- --- The run -------------------------------------------------------------

--- Starts the run. Called once by the node, after the sequence has declared
--- its cases.
function __tb_begin()
    index = 0
    current = nil
end

local function start_next()
    index = index + 1

    local case = cases[index]
    if case == nil then
        return false
    end

    __tb_case_started(case.name)
    current = coroutine.create(case.body)

    return true
end

--- One pass. Returns "running" while there is anything left to do, and
--- "finished" once the last case has returned.
---
--- Resumed at most once per pass on purpose: a case that never yields would
--- otherwise run the whole sequence inside one dispatch, which is the one way
--- a test could stall the measurement it is testing.
function __tb_pump()
    if current == nil then
        if not start_next() then
            return "finished"
        end
    end

    local ok, err = coroutine.resume(current)

    if coroutine.status(current) ~= "dead" then
        return "running"
    end

    current = nil

    if ok then
        __tb_case_finished(true, "")
    elseif type(err) == "table" and err.__tb_failure then
        __tb_case_finished(false, err.message)
    else
        -- A genuine error in the test itself - a nil index, a bad argument.
        -- Reported differently because it needs a different person: a failure
        -- is about the network, an error is about the test.
        __tb_case_errored(tostring(err))
    end

    if index >= #cases then
        return "finished"
    end

    return "running"
end
)LUA";

} // namespace torquebus
