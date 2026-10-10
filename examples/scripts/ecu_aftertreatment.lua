-- ecu_aftertreatment.lua
--
-- J1939 Aftertreatment Control Module (ACM, SA 0x3D / 61)
-- Implements DEF (ARLA 32) management and SCR monitoring.
--
-- Transmits cyclic standard J1939 PGNs:
--   * AT1T1I1 (PGN 65110 / 0xFE56, 1 s, P6) - DEF Level (SPN 1761), DEF Temp (SPN 3031), Low Indicator (SPN 5245)
--   * DM1     (PGN 65226 / 0xFECA, 1 s, P6) - Active Diagnostic Trouble Codes
--
-- Block parameters (all optional):
--   sa, tick_ms                       source address (0x3D), timer period (50 ms)
--   at_interval_ms, dm1_interval_ms   cycle times of the two messages above
--   initial_def_level, initial_def_temp
--   scenario (default "normal"):
--     low_adblue   the tank is at 8 %: the low indicator, the inducement severity, and a DM1 with the
--                  amber lamp

local kSa = parameters.sa or 0x3D
local kTickMs = parameters.tick_ms or 50
local kScenario = parameters.scenario or ""

local kAtIntervalUs = (parameters.at_interval_ms or 1000) * 1000
local kDm1IntervalUs = (parameters.dm1_interval_ms or 1000) * 1000

local next_at_us = 0
local next_dm1_us = 0

-- DEF tank state (0..100 %)
local def_level = parameters.initial_def_level or 68.0
local def_temp = parameters.initial_def_temp or 22.0
local def_tank_capacity_l = 35.0  -- Typical 35 L DEF tank

local last_tick_us = nil

function on_enable()
    log_message(string.format("J1939 Aftertreatment ECU (SA 0x%02X) active", kSa))
    last_tick_us = nil
    set_timer(kTickMs)
end

function on_disable()
    log_message(string.format("J1939 Aftertreatment ECU stopped, DEF at %.1f%%", def_level))
end

function on_timer()
    local now = get_time_us()
    local dt_sec = last_tick_us and math.min((now - last_tick_us) * 1.0e-6, 0.25) or (kTickMs / 1000.0)
    last_tick_us = now

    if kScenario == "low_adblue" then
        def_level = 8.0 -- 8% triggers the amber AdBlue bar and a DTC
    else
        -- Consume DEF gradually (approx 4% of typical diesel burn rate ~2.5 L/h -> 0.1 L/h)
        local def_burn_rate_lh = 0.08
        local def_burned_l = def_burn_rate_lh * (dt_sec / 3600.0)
        local pct_burned = (def_burned_l / def_tank_capacity_l) * 100.0
        def_level = math.max(0.0, def_level - pct_burned)
    end

    local low_indicator = (def_level < 10.0) and 1 or 0
    local inducement_severity = (def_level < 5.0) and 2 or ((def_level < 10.0) and 1 or 0)

    -- AT1T1I1 (PGN 0xFE56, 1 s, Prio 6)
    if now >= next_at_us then
        next_at_us = now + kAtIntervalUs
        local b1 = raw(def_level, 0.4, 0, 1)
        local b2 = raw(def_temp, 1, -40, 1)
        local b34 = raw(def_level * 3.5, 0.1, 0, 2) -- Level in mm (approx)
        local b5 = 0xF0 | (low_indicator & 0x03)
        local b6 = 0xF8 | (inducement_severity & 0x07)
        send(6, 0xFE56, kSa, string.pack("<I1I1I2I1I1", b1, b2, b34, b5, b6))
    end

    -- DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        if def_level < 10.0 then
            -- Amber warning lamp, SPN 1761 (DEF Tank Level), FMI 1 (Low severely), OC 1
            send(6, 0xFECA, kSa, dm1(0, 0, 1, 0, 1761, 1, 1))
        else
            send(6, 0xFECA, kSa, dm1(0, 0, 0, 0, 0, 0, 0))
        end
    end
end
