-- ecu_switches.lua
--
-- J1939 Operator External Light & Switch Controller (COMMANDS, SA 0x37 / 55)
-- Simulates driver controls: hazard button, turn signal stalk, headlight switch.
--
-- The body controller (ecu_body.lua) listens to this and lights the lamps, and the cluster's hazard
-- lamp reads the hazard button from here directly: the arrows flash on the cluster because a stalk
-- moved on this panel, which is what the two ECUs are in the project for.
--
-- Transmits cyclic standard J1939 PGNs:
--   * OEL (PGN 64972 / 0xFDCC, 200 ms, P6) - Lighting Command (SPN 2872), Hazard (SPN 2875), Turn Switch (SPN 2876)
--   * DM1 (PGN 65226 / 0xFECA, 1 s, P6)    - Active Diagnostic Trouble Codes
--
-- Block parameters (all optional):
--   sa, tick_ms                    source address (0x37), timer period (50 ms)
--   oel_interval_ms, dm1_interval_ms
--   lighting (2)                   the light switch: 0 off, 1 park, 2 headlamps
--   high_beam (0)                  the high beam: 0 off, 1 on
--   hazard (0)                     the hazard button: 0 off, 1 on
--   turn (0)                       the turn stalk: 0 neutral, 1 left, 2 right
--   scenario (default "normal"):
--     flasher_cycle   left for 5 s, right for 5 s, hazard for 5 s, and round again
--     turn_left, turn_right, hazard    the stalk, or the button, held

local kSa = parameters.sa or 0x37
local kTickMs = parameters.tick_ms or 50
local kScenario = parameters.scenario or ""

local kOelIntervalUs = (parameters.oel_interval_ms or 200)  * 1000
local kDm1IntervalUs = (parameters.dm1_interval_ms or 1000) * 1000

local next_oel_us = 0
local next_dm1_us = 0

-- Switch positions
local lighting_cmd = parameters.lighting or 2   -- 0: Off, 1: Park, 2: Headlamps
local high_beam_cmd = parameters.high_beam or 0 -- 0: Off, 1: On
local hazard_cmd = parameters.hazard or 0       -- 0: Off, 1: On
local turn_signal_cmd = parameters.turn or 0    -- 0: None, 1: Left, 2: Right

-- Automated scenario cycling (alternates Left, Right, Hazard)
local scenario_start_us = 0

function on_enable()
    log_message(string.format("J1939 Switch Panel (SA 0x%02X) active", kSa))
    scenario_start_us = get_time_us()
    set_timer(kTickMs)
end

function on_disable()
    log_message("J1939 Switch Panel stopped")
end

function on_timer()
    local now = get_time_us()

    if kScenario == "flasher_cycle" then
        local t = ((now - scenario_start_us) * 1.0e-6) % 15.0
        if t < 5.0 then
            turn_signal_cmd = 1 -- Left
            hazard_cmd = 0
        elseif t < 10.0 then
            turn_signal_cmd = 2 -- Right
            hazard_cmd = 0
        else
            turn_signal_cmd = 0
            hazard_cmd = 1      -- Hazard
        end
    elseif kScenario == "turn_left" then
        turn_signal_cmd = 1
        hazard_cmd = 0
    elseif kScenario == "turn_right" then
        turn_signal_cmd = 2
        hazard_cmd = 0
    elseif kScenario == "hazard" then
        turn_signal_cmd = 0
        hazard_cmd = 1
    end

    -- 1. OEL (PGN 0xFDCC, 200 ms, Prio 6). Byte 1: bits 1-4 light switch, bits 5-6 high beam.
    -- Byte 2: bits 1-2 hazard button, bits 3-6 turn stalk.
    if now >= next_oel_us then
        next_oel_us = now + kOelIntervalUs
        local b1 = (lighting_cmd & 0x0F) | ((high_beam_cmd & 0x03) << 4)
        local b2 = (hazard_cmd & 0x03) | ((turn_signal_cmd & 0x0F) << 2)
        send(6, 0xFDCC, kSa, string.pack("<I1I1", b1, b2))
    end

    -- 2. DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        send(6, 0xFECA, kSa, dm1(0, 0, 0, 0, 0, 0, 0))
    end
end
