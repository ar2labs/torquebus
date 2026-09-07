-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- An engine ECU that answers diagnostics.
--
-- Drop a Lua ECU block, point it at this file, and set its two diagnostic
-- identifiers: Request 0x7E0 and Response 0x7E8 - the ECU *listens* on what a
-- tester transmits, which is the way round that catches everybody once.
--
-- Then wire CAN Channel -> Lua ECU -> CAN Transmit, press Start, and ask it
-- things from the Diagnostics panel. Everything below is what a real ECU does
-- and a script has to be allowed to get wrong.

local temperature = 87
local faults_set = false

function on_enable()
    -- What a tester can read. Anything not declared here is refused with
    -- "request out of range", which is what a real ECU says and what makes a
    -- tester that guesses identifiers fail honestly.
    uds_did(0xF190, "WVWZZZ1KZAW000001")  -- VIN
    uds_did(0xF187, "04E906016AD")        -- part number
    uds_did(0xF195, "0004")               -- software version

    -- A calibration: writeable, but only in the extended session and only once
    -- the ECU has been unlocked. Three different refusals for three different
    -- mistakes, and a tester has to work through them in order.
    uds_did(0x2001, string.char(0x00, 0x64),
            { writable = true, session = 3, security = true })

    -- Reading engine temperature every time it is asked would be better done
    -- from on_timer, which is what the update below does.
    uds_did(0x0105, string.char(temperature))

    set_timer(500)
    log_message("Engine ECU ready - try 22 F1 90 from the Diagnostics panel")
end

function on_timer()
    -- A value that moves, so a tester reading it twice sees two answers. This
    -- is what makes a simulated ECU worth reading rather than a fixture.
    temperature = 80 + math.floor(math.sin(get_time_us() / 5e6) * 10)
    uds_did(0x0105, string.char(temperature))

    -- Set a fault after ten seconds, so a workflow that reads DTCs, clears
    -- them and reads again has something to find.
    if not faults_set and get_time_us() > 10e6 then
        uds_dtc(0x012800, 0x2F)   -- P0128, confirmed and failed this cycle
        uds_dtc(0xC03500)         -- U0035, confirmed
        faults_set = true

        log_message("Two faults stored - try 19 02 FF")
    end
end

-- The key this ECU will accept, computed from the seed it gave out.
--
-- Not security: this is a simulation, and the point is that a tester's own
-- seed/key routine can be exercised end to end. A real algorithm lives in a
-- supplier's DLL and is somebody else's secret.
function on_security_seed(seed)
    local key = ""

    for index = 1, #seed do
        key = key .. string.char(255 - seed:byte(index))
    end

    return key
end

-- Anything the built-in server does not do.
--
-- Three answers, and all three are useful:
--   * return bytes    -- this is the response
--   * return nothing  -- not my business, let the server deal with it
--   * return false    -- say nothing at all
function on_uds_request(request)
    local service = request:byte(1)

    -- RoutineControl: start routine 0x0203 and answer that it started.
    if service == 0x31 and #request >= 4 then
        local routine = request:byte(3) * 256 + request:byte(4)

        if routine == 0x0203 then
            log_message("Routine 0x0203 started")
            return string.char(0x71, request:byte(2), request:byte(3), request:byte(4))
        end
    end

    -- A deliberately dead identifier: 0x2010 is never answered at all. A tester
    -- has to survive silence, and this is the only way to try that without
    -- unplugging something.
    if service == 0x22 and #request >= 3
        and request:byte(2) == 0x20 and request:byte(3) == 0x10 then
        return false
    end

    -- Everything else: not this script's business.
end
