-- ecu_cluster_core.lua
--
-- J1939 Instrument Cluster Core (IC, SA 0x17 / 23)
-- Implements odometer integration from vehicle speed, trip distance, and clock.
--
-- It listens to the bus for CCVS1 (the vehicle speed, which the odometer integrates), so the project
-- wires the CAN source into this block. The cluster on the Dashboard reads the odometer from the
-- bus like any other signal; the lamps of the whole bus are the J1939 block's business, not this
-- ECU's.
--
-- Transmits cyclic standard J1939 PGNs:
--   * VDHR (PGN 65217 / 0xFEC1, 1 s, P6) - Total Distance (SPN 917), Trip Distance (SPN 918)
--   * TD   (PGN 65254 / 0xFEE6, 1 s, P6) - Time & Date Broadcast (SPN 959..964, 1601, 1602)
--   * DM1  (PGN 65226 / 0xFECA, 1 s, P6) - Active Diagnostic Trouble Codes
--
-- The odometer is kept in the system variable cluster.odometer_km, so that it survives Stop and
-- Start the way a real one survives the ignition: the second run of a session goes on from where
-- the first one stopped, and initial_odometer_km is only where the very first one begins.
--
-- Block parameters (all optional):
--   sa, tick_ms                                source address (0x17), timer period (50 ms)
--   vdhr_interval_ms, td_interval_ms, dm1_interval_ms
--   initial_odometer_km, initial_trip_km, initial_speed

local kSa = parameters.sa or 0x17
local kTickMs = parameters.tick_ms or 50

local kVdhrIntervalUs = (parameters.vdhr_interval_ms or 1000) * 1000
local kTdIntervalUs   = (parameters.td_interval_ms   or 1000) * 1000
local kDm1IntervalUs  = (parameters.dm1_interval_ms  or 1000) * 1000

local next_vdhr_us = 0
local next_td_us   = 0
local next_dm1_us  = 0

-- Odometer state in meters
local total_distance_m = (parameters.initial_odometer_km or 14852.0) * 1000.0
local trip_distance_m  = (parameters.initial_trip_km or 124.6) * 1000.0
local current_speed_kmh = parameters.initial_speed or 0.0

-- Simulated Clock (starting at 14:35:00, 2026-10-07)
local sim_seconds = 0.0
local clock_sec = 0
local clock_min = 35
local clock_hour = 14
local clock_day = 7
local clock_month = 10
local clock_year = 2026

local last_tick_us = nil

local function extract_pgn(id)
    local pf = (id >> 16) & 0xFF
    if pf < 240 then
        return (id >> 8) & 0x3FF00
    else
        return (id >> 8) & 0x3FFFF
    end
end

function on_enable()
    log_message(string.format("J1939 Cluster Core ECU (SA 0x%02X) active", kSa))
    last_tick_us = nil

    -- Restore the odometer from where the last run left it, if there was one: a variable nobody has
    -- written reads as zero, and a real odometer never does.
    if var_get then
        local saved = var_get("cluster.odometer_km")
        if saved and type(saved) == "number" and saved > 0 then
            total_distance_m = saved * 1000.0
        end
    end
    set_timer(kTickMs)
end

function on_disable()
    log_message(string.format("J1939 Cluster Core stopped, odometer at %.3f km", total_distance_m / 1000.0))
    if var_set then
        var_set("cluster.odometer_km", total_distance_m / 1000.0)
    end
end

function on_message(id, data, channel, is_extended)
    -- is_extended is 1 or 0, and 0 is true in Lua.
    if is_extended ~= 1 or not data or #data < 3 then return end

    -- CCVS1 (PGN 0xFEF1 = 65265): bytes 2-3 carry SPN 84 Wheel-Based Vehicle Speed (1/256 km/h)
    if extract_pgn(id) == 0xFEF1 then
        local raw_speed = string.unpack("<I2", data:sub(2, 3))
        -- Above 0xFAFF is "error" and "not available": not a speed, and the odometer keeps the last one.
        if raw_speed <= 0xFAFF then
            current_speed_kmh = raw_speed / 256.0
        end
    end
end

function on_timer()
    local now = get_time_us()

    -- From the clock and not the timer period (see ecu_engine.lua): 100 km/h for 36 s is
    -- 1.000 km only if 36 s is what passed.
    local dt_sec = last_tick_us and math.min((now - last_tick_us) * 1.0e-6, 0.25) or (kTickMs / 1000.0)
    last_tick_us = now

    -- Integrate vehicle speed into distance
    if current_speed_kmh > 0.0 then
        local delta_m = (current_speed_kmh / 3.6) * dt_sec
        total_distance_m = total_distance_m + delta_m
        trip_distance_m = trip_distance_m + delta_m
    end

    -- Advance clock
    sim_seconds = sim_seconds + dt_sec
    if sim_seconds >= 1.0 then
        local advance = math.floor(sim_seconds)
        sim_seconds = sim_seconds - advance
        clock_sec = clock_sec + advance
        if clock_sec >= 60 then
            clock_min = clock_min + math.floor(clock_sec / 60)
            clock_sec = clock_sec % 60
            if clock_min >= 60 then
                clock_hour = (clock_hour + math.floor(clock_min / 60)) % 24
                clock_min = clock_min % 60
            end
        end
        -- Persist periodically
        if var_set then
            var_set("cluster.odometer_km", total_distance_m / 1000.0)
        end
    end

    -- 1. VDHR (PGN 0xFEC1, 1 s, Prio 6)
    if now >= next_vdhr_us then
        next_vdhr_us = now + kVdhrIntervalUs
        -- SPN 917 Total Vehicle Distance: 5 m/bit = 0.005 km/bit
        local odo_raw = math.floor(total_distance_m / 5.0 + 0.5)
        local trip_raw = math.floor(trip_distance_m / 5.0 + 0.5)
        send(6, 0xFEC1, kSa, string.pack("<I4I4", odo_raw & 0xFFFFFFFF, trip_raw & 0xFFFFFFFF))
    end

    -- 2. TD (PGN 0xFEE6, 1 s, Prio 6)
    if now >= next_td_us then
        next_td_us = now + kTdIntervalUs
        local b1 = raw(clock_sec, 0.25, 0, 1)
        local b2 = raw(clock_min, 1, 0, 1)
        local b3 = raw(clock_hour, 1, 0, 1)
        local b4 = raw(clock_month, 1, 0, 1)
        local b5 = raw(clock_day, 0.25, 0, 1)
        local b6 = raw(clock_year, 1, 1985, 1)
        local b7 = raw(0, 1, -125, 1) -- Local minute offset: 0
        local b8 = raw(-3, 1, -125, 1) -- Local hour offset: -3 (BRT)
        send(6, 0xFEE6, kSa, string.pack("<I1I1I1I1I1I1I1I1", b1, b2, b3, b4, b5, b6, b7, b8))
    end

    -- 3. DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        send(6, 0xFECA, kSa, dm1(0, 0, 0, 0, 0, 0, 0))
    end
end
