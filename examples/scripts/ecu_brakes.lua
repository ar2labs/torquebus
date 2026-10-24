-- ecu_brakes.lua
--
-- J1939 Electronic Braking System ECU (EBS, SA 0x0B / 11)
-- Implements brake pressure, ABS control, and pneumatic circuits.
--
-- Transmits cyclic standard J1939 PGNs:
--   * EBC1 (PGN 61441 / 0xF001, 100 ms, P6) - ABS Active (SPN 563), ABS/EBS Amber and EBS Red Warning (SPN 1438, 1439)
--   * AIR1 (PGN 65198 / 0xFEAE, 1 s, P6)    - Supply Pressure (SPN 46), Circuit 1 & 2 (SPN 1087, 1088)
--   * DM1  (PGN 65226 / 0xFECA, 1 s, P6)    - Active Diagnostic Trouble Codes
--
-- Block parameters (all optional):
--   sa, tick_ms                              source address (0x0B), timer period (50 ms)
--   *_interval_ms                            cycle time of each message above (ebc1, air1, dm1)
--   initial_supply_air, initial_circuit1_air, initial_circuit2_air     the pressures, in bar
--   scenario (default "normal"):
--     abs_fault   the ABS has failed: the amber warning signal, and a DM1 with the amber lamp
--     air_leak    circuit 1 loses a bar a second: below 4.5 bar the red warning signal and a DM1 with
--                 the red stop lamp

local kSa = parameters.sa or 0x0B
local kTickMs = parameters.tick_ms or 50
local kScenario = parameters.scenario or ""

-- What this ECU is, as J1939-81 numbers it: function 9 is the brakes' system controller.
local kFunction = 9

local kEbc1IntervalUs = (parameters.ebc1_interval_ms or 100)  * 1000
local kAir1IntervalUs = (parameters.air1_interval_ms or 1000) * 1000
local kDm1IntervalUs  = (parameters.dm1_interval_ms  or 1000) * 1000

local next_ebc1_us = 0
local next_air1_us = 0
local next_dm1_us  = 0

-- Braking state. abs_active is the ABS regulating the wheels right now, which it only does under
-- braking on a slippery road, and nothing here brakes that hard.
local abs_active = 0
local asr_engine_active = 0
local asr_brake_active = 0
local amber_warning = 0
local red_warning = 0

-- Pneumatic circuit pressures in bar (scale 0.08 bar/bit = 8 kPa/bit)
local kSupplyBar = parameters.initial_supply_air or 8.5
local kCircuit1Bar = parameters.initial_circuit1_air or 8.5
local kCircuit2Bar = parameters.initial_circuit2_air or 8.4
local air_supply_bar = kSupplyBar
local air_circuit1_bar = kCircuit1Bar
local air_circuit2_bar = kCircuit2Bar

local last_tick_us = nil
local last_sent_ebc1 = nil
local override_ebc1_payload = nil
local override_ebc1_until_us = nil

local last_sent_air1 = nil
local override_air1_payload = nil
local override_air1_until_us = nil

local last_sent_dm1 = nil
local override_dm1_payload = nil
local override_dm1_until_us = nil

function on_enable()
    log_message(string.format("J1939 Brake ECU (SA 0x%02X) active", kSa))

    -- Power-up: say who this is (see ecu_engine.lua).
    j1939_claim(kSa, kFunction, 0, nil, 1, false)

    last_tick_us = nil
    set_timer(kTickMs)
end

function on_disable()
    log_message(string.format("J1939 Brake ECU stopped, circuit 1 at %.1f bar", air_circuit1_bar))
end

function on_message(id, data, channel, extended)
    if extended ~= 1 or #data < 8 then
        return
    end
    local now = get_time_us()
    if id == j1939_id(6, 0xF001, kSa) and data ~= last_sent_ebc1 then
        override_ebc1_payload = data
        override_ebc1_until_us = now + 5000000
    elseif id == j1939_id(6, 0xFEAE, kSa) and data ~= last_sent_air1 then
        override_air1_payload = data
        override_air1_until_us = now + 5000000
    elseif id == j1939_id(6, 0xFECA, kSa) and data ~= last_sent_dm1 then
        if data == dm1(0, 0, 0, 0, 0, 0, 0) then
            override_dm1_payload = nil
            override_dm1_until_us = nil
        else
            override_dm1_payload = data
            override_dm1_until_us = now + 10000000
        end
    end
end

function on_timer()
    local now = get_time_us()
    local dt_sec = last_tick_us and math.min((now - last_tick_us) * 1.0e-6, 0.25) or (kTickMs / 1000.0)
    last_tick_us = now

    if kScenario == "abs_fault" then
        abs_active = 0
        amber_warning = 1 -- the amber ABS lamp on
    elseif kScenario == "air_leak" then
        air_circuit1_bar = math.max(2.5, air_circuit1_bar - 1.0 * dt_sec)
        air_circuit2_bar = math.max(2.8, air_circuit2_bar - 0.8 * dt_sec)
        if air_circuit1_bar < 4.5 then
            red_warning = 1
        end
    else
        amber_warning = 0
        red_warning = 0
        air_supply_bar = kSupplyBar
        air_circuit1_bar = kCircuit1Bar
        air_circuit2_bar = kCircuit2Bar
    end

    -- 1. EBC1 (PGN 0xF001, 100 ms, Prio 6)
    if now >= next_ebc1_us then
        next_ebc1_us = now + kEbc1IntervalUs
        local ebc1_payload
        if override_ebc1_payload ~= nil and override_ebc1_until_us ~= nil and now < override_ebc1_until_us then
            ebc1_payload = override_ebc1_payload
        else
            override_ebc1_payload = nil
            local b1 = (asr_engine_active & 0x03) | ((asr_brake_active & 0x03) << 2) | ((abs_active & 0x03) << 4)
            local b6 = (amber_warning & 0x03) | ((red_warning & 0x03) << 2)
            ebc1_payload = string.pack("<I1I1I1I1I1I1I1I1", b1, 0xFF, 0xFF, 0xFF, 0xFF, b6, 0xFF, 0xFF)
        end
        last_sent_ebc1 = ebc1_payload
        send(6, 0xF001, kSa, ebc1_payload)
    end

    -- 2. AIR1 (PGN 0xFEAE, 1 s, Prio 6)
    if now >= next_air1_us then
        next_air1_us = now + kAir1IntervalUs
        local air1_payload
        if override_air1_payload ~= nil and override_air1_until_us ~= nil and now < override_air1_until_us then
            air1_payload = override_air1_payload
        else
            override_air1_payload = nil
            local b1 = raw(air_supply_bar, 0.08, 0, 1)
            local b3 = raw(air_circuit1_bar, 0.08, 0, 1)
            local b4 = raw(air_circuit2_bar, 0.08, 0, 1)
            air1_payload = string.pack("<I1I1I1I1", b1, 0xFF, b3, b4)
        end
        last_sent_air1 = air1_payload
        send(6, 0xFEAE, kSa, air1_payload)
    end

    -- 3. DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        local dm1_payload
        if override_dm1_payload ~= nil and override_dm1_until_us ~= nil and now < override_dm1_until_us then
            dm1_payload = override_dm1_payload
        else
            override_dm1_payload = nil
            if kScenario == "abs_fault" then
                -- Amber warning lamp, SPN 563 (Anti-Lock Braking Active), FMI 12 (Bad intelligent device), OC 1
                dm1_payload = dm1(0, 0, 1, 0, 563, 12, 1)
            elseif red_warning > 0 then
                -- Red stop lamp, SPN 1087 (Service Brake Circuit 1 Air Pressure), FMI 1 (Low severely), OC 1
                dm1_payload = dm1(0, 1, 0, 0, 1087, 1, 1)
            else
                dm1_payload = dm1(0, 0, 0, 0, 0, 0, 0)
            end
        end
        last_sent_dm1 = dm1_payload
        send(6, 0xFECA, kSa, dm1_payload)
    end
end
