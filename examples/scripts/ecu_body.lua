-- ecu_body.lua
--
-- J1939 Body Control Module (BCM, SA 0x21 / 33)
-- Implements electrical system, fuel tank, lighting, turn signals, and seatbelt.
--
-- It listens as well as talks. The switch panel (ecu_switches.lua, SA 0x37) sends OEL - light
-- switch, high beam, hazard button, turn stalk - and this ECU turns that into the lamps, the way a
-- body controller does: move the stalk on the panel and the arrow lights on the cluster. The block
-- needs a wire from the CAN source to hear it; without one, or while the panel is silent, the block
-- parameters below stand in for the switches.
--
-- Transmits cyclic standard J1939 PGNs:
--   * DD1  (PGN 65276 / 0xFEFC, 1 s, P6) - Fuel Level (SPN 96), Washer Level (SPN 80)
--   * VEP1 (PGN 65271 / 0xFEF7, 1 s, P6) - Battery Voltage (SPN 168), Charging Potential (SPN 167)
--   * LD   (PGN 65089 / 0xFE41, 1 s, and at once when a lamp changes, P6) - Headlights, Turn Signals
--   * BAS  (PGN 64791 / 0xFD17, 1 s, P6) - Seat Belt Switches and the warning lamp they drive
--   * DM1  (PGN 65226 / 0xFECA, 1 s, P6) - Active Diagnostic Trouble Codes
--
-- The turn signals go out steady, 1 for as long as the stalk is in: the instrument cluster does the
-- flashing, and a flasher here as well would make the two beat against each other.
--
-- Block parameters (all optional):
--   sa, tick_ms                          source address (0x21), timer period (50 ms)
--   *_interval_ms                        cycle time of each message above (dd1, vep1, ld, bas, dm1)
--   initial_fuel_level, initial_washer_level, initial_battery, initial_charging
--   lighting (2), high_beam (0)          the light switch (0 off, 1 park, 2 headlamps) and the high beam,
--                                        for when no switch panel is on the bus
--   driver_belt, passenger_belt (1)      1 fastened, 0 not
--   scenario (default "normal"):
--     alternator_failure   battery 11.4 V, charging 11.2 V: DM1 with the amber lamp
--     cold_start           battery 11.8 V
--     turn_left, turn_right, hazard   the turn signals, for when no switch panel is on the bus
--     belt_unbuckled       the driver's belt is not fastened: the warning lamp

local kSa = parameters.sa or 0x21
local kTickMs = parameters.tick_ms or 50
local kScenario = parameters.scenario or ""

local kDd1IntervalUs  = (parameters.dd1_interval_ms  or 1000) * 1000
local kVep1IntervalUs = (parameters.vep1_interval_ms or 1000) * 1000
local kLdIntervalUs   = (parameters.ld_interval_ms   or 1000) * 1000
local kBasIntervalUs  = (parameters.bas_interval_ms  or 1000) * 1000
local kDm1IntervalUs  = (parameters.dm1_interval_ms  or 1000) * 1000

-- The switch panel sends OEL every 200 ms; this many without it and the panel is gone.
local kOelTimeoutUs = 1000000

local next_dd1_us  = 0
local next_vep1_us = 0
local next_ld_us   = 0
local next_bas_us  = 0
local next_dm1_us  = 0

-- Fluid levels
local fuel_level = parameters.initial_fuel_level or 78.0     -- %
local washer_level = parameters.initial_washer_level or 92.0 -- %
local tank_capacity_l = 60.0                                 -- L

-- Electrical state (12 V system)
local kNominalBattery = parameters.initial_battery or 12.6   -- V
local kNominalCharging = parameters.initial_charging or 14.2 -- V
local battery_voltage = kNominalBattery
local charging_voltage = kNominalCharging
local net_current = 5.0                                      -- A
local alt_current = 28.0                                     -- A

-- Seatbelt (0 = unbuckled, 1 = buckled)
local driver_belt = parameters.driver_belt or 1
local passenger_belt = parameters.passenger_belt or 1
if kScenario == "belt_unbuckled" then
    driver_belt = 0
end

-- What the switch panel last said, and when. nil until it has.
local oel_lighting = nil
local oel_high_beam = nil
local oel_hazard = nil
local oel_turn = nil
local oel_heard_us = nil

-- The lamps as last sent, so that a change does not wait for the next second.
local last_ld_payload = nil
local last_tick_us = nil

local function pgn_of(id)
    local pf = (id >> 16) & 0xFF
    if pf < 240 then
        return (id >> 8) & 0x3FF00
    end
    return (id >> 8) & 0x3FFFF
end

function on_enable()
    log_message(string.format("J1939 Body ECU (SA 0x%02X) active", kSa))
    last_tick_us = nil
    set_timer(kTickMs)
end

function on_disable()
    log_message(string.format("J1939 Body ECU stopped, fuel at %.1f%%", fuel_level))
end

-- OEL (PGN 0xFDCC) from the switch panel. Byte 1: bits 1-4 light switch, bits 5-6 high beam.
-- Byte 2: bits 1-2 hazard button, bits 3-6 turn stalk. A field at 14 or 15 is J1939's "error" or
-- "not available" and is not a position, so it is taken as no word at all.
function on_message(id, data, channel, extended)
    if extended ~= 1 or #data < 2 or pgn_of(id) ~= 0xFDCC then
        return
    end

    local b1, b2 = string.unpack("<I1I1", data)

    local lighting = b1 & 0x0F
    local turn = (b2 >> 2) & 0x0F

    oel_lighting = lighting < 14 and lighting or nil
    oel_high_beam = (b1 >> 4) & 0x03
    oel_hazard = b2 & 0x03
    oel_turn = turn < 14 and turn or nil
    oel_heard_us = get_time_us()
end

function on_timer()
    local now = get_time_us()
    local dt_sec = last_tick_us and math.min((now - last_tick_us) * 1.0e-6, 0.25) or (kTickMs / 1000.0)
    last_tick_us = now

    -- Slowly consume fuel (typical ~2.5 L/h burn rate -> 0.0007 L/s)
    local fuel_burn_rate_lh = 2.4
    local fuel_burned_l = fuel_burn_rate_lh * (dt_sec / 3600.0)
    fuel_level = math.max(0.0, fuel_level - (fuel_burned_l / tank_capacity_l) * 100.0)

    -- Scenario handling
    if kScenario == "alternator_failure" then
        charging_voltage = 11.2 -- below the battery: it is not charging
        battery_voltage = 11.4
    elseif kScenario == "cold_start" then
        battery_voltage = 11.8
        charging_voltage = 13.8
    else
        battery_voltage = kNominalBattery
        charging_voltage = kNominalCharging
    end

    -- The switches: the panel's word while it is on the bus, the block's parameters otherwise.
    local panel = oel_heard_us ~= nil and (now - oel_heard_us) <= kOelTimeoutUs

    local lighting = (panel and oel_lighting) or parameters.lighting or 2
    local high_cmd = panel and (oel_high_beam or 0) or parameters.high_beam or 0
    local hazard = panel and (oel_hazard or 0) or 0
    local turn = panel and (oel_turn or 0) or 0

    if not panel then
        if kScenario == "turn_left" then
            turn = 1
        elseif kScenario == "turn_right" then
            turn = 2
        elseif kScenario == "hazard" then
            hazard = 1
        end
    end

    -- The lamps. Headlamps are the low beam and the position lights together; the high beam needs the
    -- headlamps. A hazard button is both arrows.
    local low_beam = lighting == 2 and 1 or 0
    local position_lights = (lighting == 1 or lighting == 2) and 1 or 0
    local high_beam = (lighting == 2 and high_cmd == 1) and 1 or 0
    local left_turn = (turn == 1 or hazard == 1) and 1 or 0
    local right_turn = (turn == 2 or hazard == 1) and 1 or 0
    local stop_light = 0

    -- 1. DD1 (PGN 0xFEFC, 1 s, Prio 6)
    if now >= next_dd1_us then
        next_dd1_us = now + kDd1IntervalUs
        local b1 = raw(washer_level, 0.4, 0, 1)
        local b2 = raw(fuel_level, 0.4, 0, 1)
        send(6, 0xFEFC, kSa, string.pack("<I1I1", b1, b2))
    end

    -- 2. VEP1 (PGN 0xFEF7, 1 s, Prio 6)
    if now >= next_vep1_us then
        next_vep1_us = now + kVep1IntervalUs
        local b1 = raw(net_current, 1, -125, 1)
        local b2 = raw(alt_current, 1, 0, 1)
        local b34 = raw(charging_voltage, 0.05, 0, 2)
        local b56 = raw(battery_voltage, 0.05, 0, 2)
        local b78 = raw(battery_voltage, 0.05, 0, 2)
        send(6, 0xFEF7, kSa, string.pack("<I1I1I2I2I2", b1, b2, b34, b56, b78))
    end

    -- 3. LD (PGN 0xFE41, 1 s and on every change, Prio 6)
    local b1 = (high_beam & 0x03) | ((low_beam & 0x03) << 2) | ((position_lights & 0x03) << 4)
    local b2 = (left_turn & 0x03) | ((right_turn & 0x03) << 2) | ((stop_light & 0x03) << 4)
    local ld_payload = string.pack("<I1I1", b1, b2)

    if now >= next_ld_us or ld_payload ~= last_ld_payload then
        next_ld_us = now + kLdIntervalUs
        last_ld_payload = ld_payload
        send(6, 0xFE41, kSa, ld_payload)
    end

    -- 4. BAS (PGN 0xFD17, 1 s, Prio 6): the switches, and the lamp the body controller drives from
    -- them. A seat belt lamp is lit while the belt is NOT fastened.
    if now >= next_bas_us then
        next_bas_us = now + kBasIntervalUs
        local belt_lamp = (driver_belt == 0) and 1 or 0
        local bas = (driver_belt & 0x03) | ((passenger_belt & 0x03) << 2) | ((belt_lamp & 0x03) << 4)
        send(6, 0xFD17, kSa, string.pack("<I1", bas))
    end

    -- 5. DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        if kScenario == "alternator_failure" then
            -- Amber warning lamp, SPN 167 (Charging System Potential), FMI 1 (Low severely), OC 1
            send(6, 0xFECA, kSa, dm1(0, 0, 1, 0, 167, 1, 1))
        else
            send(6, 0xFECA, kSa, dm1(0, 0, 0, 0, 0, 0, 0))
        end
    end
end
