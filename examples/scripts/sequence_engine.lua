-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A test sequence: what a person checking an engine ECU would check by hand,
-- written down so that it can be checked a hundred times instead of once.
--
-- Drop a **Test Sequence** block on the canvas, point it at this file, and wire
-- the channel into it. The verdict appears in the Test panel and in the Output
-- panel, and the measurement carries on either way - a test observes and
-- reports, it does not stop the thing it is measuring.
--
-- Everything here is documented in docs/development/testing.md.

local ENGINE_ID = parameters.engine_id or 0x100
local LAMP_ID = parameters.lamp_id or 0x300
local TESTER_ID = parameters.tester_id or 0x7E0
local ECU_ID = parameters.ecu_id or 0x7E8

test("The engine message appears at all", function()
    -- The first thing anybody checks, and the one most easily forgotten in a
    -- suite that goes straight to decoding payloads.
    expect_frame(ENGINE_ID, { within = 500 })
end)

test("The engine message keeps its cycle time", function()
    local first = expect_frame(ENGINE_ID, { within = 500 })
    local second = expect_frame(ENGINE_ID, { within = 500 })

    local gap = (second.timestamp_us - first.timestamp_us) / 1000

    -- A range, not a number. A cycle time held to the microsecond is a
    -- simulation, and a test that demands one fails on real hardware for
    -- reasons that have nothing to do with the ECU.
    assert_between(gap, 5, 40, "cycle time in ms")
end)

test("The engine message is the length the database says", function()
    local frame = expect_frame(ENGINE_ID, { within = 500 })
    assert_equal(#frame.data, 8, "DLC")
end)

test("The rolling counter increments", function()
    -- Nearly every real message carries one so that a receiver can tell a
    -- repeated frame from a fresh one - and a counter that is stuck is the
    -- fault this catches and a human eye does not.
    local first = expect_frame(ENGINE_ID, { within = 500 })
    local second = expect_frame(ENGINE_ID, { within = 500 })

    local before = first.data:byte(2) & 0x0F
    local after = second.data:byte(2) & 0x0F

    assert_equal(after, (before + 1) % 16, "counter increments and wraps")
end)

test("The ECU answers a request for the VIN", function()
    send(TESTER_ID, "\x02\x22\xF1\x90\x55\x55\x55\x55")

    -- The answer comes back on the response identifier. Single frame or the
    -- first of several: this checks that something came back at all, which is
    -- the question that matters when a tester sees nothing.
    local answer = expect(ECU_ID, { within = 200 })

    assert_true(answer ~= nil, "an answer within 200 ms")
    assert_true(answer.data:byte(2) ~= 0x7F, "not a negative response")
end)

test("The lamp stays out while nothing is wrong", function()
    -- The other way round, and the one most tools cannot express: nothing on
    -- this identifier for this long.
    expect_silence(LAMP_ID, 300)
end)
