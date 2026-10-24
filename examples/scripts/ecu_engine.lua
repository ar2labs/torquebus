-- ecu_engine.lua
--
-- J1939 Virtual Engine Management System (EMS, SA 0x00)
-- Implements Phase 1 of the J1939 Simulator Instrument Cluster specification.
--
-- Transmits cyclic standard J1939 PGNs:
--   * EEC1   (PGN 61444 / 0xF004, 20 ms, P3) - Engine Speed (SPN 190), Torque (SPN 513)
--   * EEC2   (PGN 61443 / 0xF003, 50 ms, P3) - Accelerator Pedal (SPN 91), Engine Load (SPN 92)
--   * ETC2   (PGN 61445 / 0xF005, 100 ms, P3) - Transmission Current Gear (SPN 523). A transmission
--                                              ECU sends this on a real vehicle; here the engine ECU
--                                              also plays the gearbox, so the cluster has a gear
--   * CCVS1  (PGN 65265 / 0xFEF1, 100 ms, P6) - Vehicle Speed (SPN 84), Brake / Cruise (SPN 70, 595, 597)
--   * LFE1   (PGN 65266 / 0xFEF2, 100 ms, P6) - Fuel Rate (SPN 183), Fuel Economy (SPN 184, 185)
--   * EFL_P1 (PGN 65263 / 0xFEEF, 500 ms, P6) - Oil Pressure (SPN 100), Oil Level (SPN 98), Coolant Level (SPN 111)
--   * ET1    (PGN 65262 / 0xFEEE, 1 s, P6)   - Coolant Temp (SPN 110), Fuel Temp (SPN 174), Oil Temp (SPN 175)
--   * AMB    (PGN 65269 / 0xFEF5, 1 s, P6)   - Ambient Temp (SPN 171), Barometric Press (SPN 108), Intake Temp (SPN 172)
--   * HOURS  (PGN 65253 / 0xFEE5, 1 s, P6)   - Engine Hours (SPN 247), Engine Revolutions (SPN 249)
--   * DM1    (PGN 65226 / 0xFECA, 1 s, P6)   - Active Diagnostic Trouble Codes
--
-- And once, at power-up, an Address Claimed (PGN 60928 / 0xEE00) with a NAME whose function is
-- Engine, as a J1939 ECU does: it is what puts a name to this address in the J1939 Network panel
-- and on the Dashboard's fault list.
--
-- Block parameters (all optional):
--   sa, tick_ms                          source address (0), timer period (10 ms)
--   *_interval_ms                        cycle time of each message above (eec1, eec2, etc2, ccvs1, ...)
--   initial_speed, initial_hours, initial_revs, initial_coolant_temp, initial_oil_temp,
--   initial_ambient_temp                 where the vehicle starts
--   scenario                             what the vehicle does (default "normal"):
--     normal            the 90 s drive cycle: standstill, 45, 80, 100 km/h, and back
--     highway           steady 100 km/h with cruise control set
--     parked            standing still with the parking brake set
--     cold_start        standing still, ambient 5 degC, the engine warming up from it
--     overheating       80 km/h and the coolant climbing to 122 degC: DM1 with MIL and red stop lamp
--     low_oil_pressure  60 km/h and 50 kPa of oil pressure: DM1 with the red stop lamp
--     sensor_error      the coolant sensor fails: ET1 says "not available", DM1 with the amber lamp

local kSa = parameters.sa or 0x00
local kTickMs = parameters.tick_ms or 10
local kScenario = parameters.scenario or ""

-- What this ECU is, as J1939-81 numbers it: function 0 is the engine.
local kFunction = 0

-- Timing intervals in microseconds
local kEec1IntervalUs  = (parameters.eec1_interval_ms  or 20)   * 1000
local kEec2IntervalUs  = (parameters.eec2_interval_ms  or 50)   * 1000
local kEtc2IntervalUs  = (parameters.etc2_interval_ms  or 100)  * 1000
local kCcvs1IntervalUs = (parameters.ccvs1_interval_ms or 100)  * 1000
local kLfe1IntervalUs  = (parameters.lfe1_interval_ms  or 100)  * 1000
local kEflIntervalUs   = (parameters.efl_interval_ms   or 500)  * 1000
local kEt1IntervalUs   = (parameters.et1_interval_ms   or 1000) * 1000
local kAmbIntervalUs   = (parameters.amb_interval_ms   or 1000) * 1000
local kHoursIntervalUs = (parameters.hours_interval_ms or 1000) * 1000
local kDm1IntervalUs   = (parameters.dm1_interval_ms   or 1000) * 1000

local next_eec1_us  = 0
local next_eec2_us  = 0
local next_etc2_us  = 0
local next_ccvs1_us = 0
local next_lfe1_us  = 0
local next_efl_us   = 0
local next_et1_us   = 0
local next_amb_us   = 0
local next_hours_us = 0
local next_dm1_us   = 0

-- Vehicle & powertrain physical parameters
local kWheelRadiusM = 0.315   -- 205/55R16 typical rolling radius
local kFinalDriveRatio = 3.42
local kGearRatios = { 3.82, 2.15, 1.42, 1.03, 0.79, 0.65 }
local kIdleRpm = 750.0
local kMaxRpm = 6500.0

-- Simulation state
local vehicle_speed = parameters.initial_speed or 0.0  -- km/h
local engine_rpm = kIdleRpm
local current_gear = 0        -- -1 reverse, 0 neutral, 1..n
local accelerator_pedal = 0.0 -- 0..100 %
local engine_load = 15.0      -- 0..100 %
local actual_torque = 15.0    -- -125..125 %

-- Fluid & thermal state
local coolant_temp = parameters.initial_coolant_temp or 85.0 -- degC; nil is "sensor failed"
local oil_temp = parameters.initial_oil_temp or 88.0         -- degC
local fuel_temp = 32.0                                       -- degC
local ambient_temp = parameters.initial_ambient_temp or 22.0 -- degC
local baro_press = 101.3                                     -- kPa
local intake_temp = ambient_temp + 5.0                       -- degC

local oil_pressure = 360.0    -- kPa
local oil_level = 95.0        -- %
local coolant_level = 98.0    -- %

local fuel_rate = 1.2         -- L/h
local inst_fuel_econ = 0.0    -- km/L
local avg_fuel_econ = 14.5    -- km/L

-- Counters & persistence
local engine_hours = parameters.initial_hours or 124.5       -- hours
local engine_revs = parameters.initial_revs or 12500000      -- revolutions
local parking_brake = 0       -- 0: off, 1: on
local brake_switch = 0        -- 0: off, 1: on
local cruise_active = 0       -- 0: off, 1: on
local cruise_set_speed = 0    -- km/h

-- Drive cycle: table-driven speed schedule (elapsed seconds -> target speed km/h)
local kDriveCycle = {
    { t = 0,   v = 0.0 },
    { t = 4,   v = 0.0 },
    { t = 14,  v = 45.0 },
    { t = 22,  v = 80.0 },
    { t = 30,  v = 100.0 },
    { t = 66,  v = 100.0 },  -- 36 seconds steady at 100 km/h (1.000 km test)
    { t = 76,  v = 60.0 },
    { t = 84,  v = 0.0 },
    { t = 90,  v = 0.0 }
}
local kCycleDurationSec = 90.0
local cycle_start_us = 0
local last_tick_us = nil

local last_sent_dm1 = nil
local override_dm1_payload = nil
local override_dm1_until_us = nil

local last_sent_ccvs1 = nil
local override_ccvs1_payload = nil
local override_ccvs1_until_us = nil

local last_sent_efl = nil
local override_efl_payload = nil
local override_efl_until_us = nil

local last_sent_et1 = nil
local override_et1_payload = nil
local override_et1_until_us = nil

local last_sent_eec1 = nil
local override_eec1_payload = nil
local override_eec1_until_us = nil

local last_sent_etc2 = nil
local override_etc2_payload = nil
local override_etc2_until_us = nil

local last_sent_amb = nil
local override_amb_payload = nil
local override_amb_until_us = nil

local last_sent_hours = nil
local override_hours_payload = nil
local override_hours_until_us = nil

local function get_target_speed(elapsed_sec)
    local t = elapsed_sec % kCycleDurationSec
    for i = 1, #kDriveCycle - 1 do
        local p1 = kDriveCycle[i]
        local p2 = kDriveCycle[i + 1]
        if t >= p1.t and t <= p2.t then
            local span = p2.t - p1.t
            if span <= 0.001 then return p2.v end
            local alpha = (t - p1.t) / span
            return p1.v + alpha * (p2.v - p1.v)
        end
    end
    return 0.0
end

local function select_gear_and_rpm(v_kmh)
    if v_kmh < 1.0 then
        return 0, kIdleRpm -- standing still: neutral at idle
    end

    -- The lowest gear that keeps the engine at or under 2600 rpm: first gear at a crawl, and not,
    -- as it once did, second gear from the first km/h with the tachometer pinned at idle.
    local best_gear = 1
    local best_rpm = kIdleRpm
    local v_ms = v_kmh / 3.6

    for g = 1, #kGearRatios do
        local ratio = kGearRatios[g] * kFinalDriveRatio
        local wheel_rad_per_sec = v_ms / kWheelRadiusM
        local rpm = (wheel_rad_per_sec * 60.0 / (2.0 * math.pi)) * ratio
        best_gear = g
        best_rpm = rpm
        if rpm <= 2600.0 then
            break
        end
    end

    if best_rpm < kIdleRpm then
        best_rpm = kIdleRpm
    elseif best_rpm > kMaxRpm then
        best_rpm = kMaxRpm
    end

    return best_gear, best_rpm
end

function on_enable()
    log_message(string.format("J1939 Engine ECU (SA 0x%02X) active%s", kSa,
        kScenario ~= "" and (", scenario " .. kScenario) or ""))

    -- Power-up: say who this is. An Address Claimed carries a NAME, and its function is what the
    -- J1939 Network panel and the Dashboard's fault list turn into a word - "Engine" - beside the
    -- address. No manufacturer code: a simulated ECU should not borrow a real company's.
    j1939_claim(kSa, kFunction, 0, nil, 1, false)

    cycle_start_us = get_time_us()
    last_tick_us = nil

    if kScenario == "cold_start" then
        -- The engine starts as cold as the day.
        ambient_temp = 5.0
        intake_temp = ambient_temp
        coolant_temp = ambient_temp
        oil_temp = ambient_temp
    end

    if parameters.enable_uds then
        uds_did(0xF190, "1M8GDM9AXKP042788")
        uds_did(0xF187, "23418902-EMS")
        uds_did(0xF195, "J1939-EMS-2.4")
        uds_did(0x0105, string.char(math.floor((coolant_temp or 85) + 40) & 0xFF))
        uds_did(0x2001, string.char(0x03, 0xE8), { writable = true, session = 3, security = true })
        uds_dtc(0x006401, 0x09)
        uds_dtc(0x006E0F, 0x08)
    end

    set_timer(kTickMs)
end

function on_security_seed(seed)
    local key = ""
    for index = 1, #seed do
        key = key .. string.char(255 - seed:byte(index))
    end
    return key
end

function on_uds_request(request)
    local service = request:byte(1)
    if service == 0x31 and #request >= 4 then
        return string.char(0x71, request:byte(2), request:byte(3), request:byte(4))
    end
end

function on_message(id, data, channel, extended)
    if extended ~= 1 or #data < 6 then
        return
    end

    local now = get_time_us()
    local dm1_id = j1939_id(6, 0xFECA, kSa)
    local ccvs1_id = j1939_id(6, 0xFEF1, kSa)
    local efl_id = j1939_id(6, 0xFEEF, kSa)
    local et1_id = j1939_id(6, 0xFEEE, kSa)
    local eec1_id = j1939_id(3, 0xF004, kSa)
    local etc2_id_ems = j1939_id(3, 0xF005, kSa)
    local etc2_id_trans = j1939_id(3, 0xF005, 0x03)
    local amb_id = j1939_id(6, 0xFEF5, kSa)
    local hours_id = j1939_id(6, 0xFEE5, kSa)

    if id == dm1_id and data ~= last_sent_dm1 then
        if data == dm1(0, 0, 0, 0, 0, 0, 0) then
            override_dm1_payload = nil
            override_dm1_until_us = nil
        else
            override_dm1_payload = data
            override_dm1_until_us = now + 10000000
        end
    elseif id == ccvs1_id and data ~= last_sent_ccvs1 then
        override_ccvs1_payload = data
        override_ccvs1_until_us = now + 5000000
    elseif id == efl_id and data ~= last_sent_efl then
        override_efl_payload = data
        override_efl_until_us = now + 5000000
    elseif id == et1_id and data ~= last_sent_et1 then
        override_et1_payload = data
        override_et1_until_us = now + 5000000
    elseif id == eec1_id and data ~= last_sent_eec1 then
        override_eec1_payload = data
        override_eec1_until_us = now + 5000000
    elseif (id == etc2_id_ems or id == etc2_id_trans) and data ~= last_sent_etc2 then
        override_etc2_payload = data
        override_etc2_until_us = now + 5000000
    elseif id == amb_id and data ~= last_sent_amb then
        override_amb_payload = data
        override_amb_until_us = now + 5000000
    elseif id == hours_id and data ~= last_sent_hours then
        override_hours_payload = data
        override_hours_until_us = now + 5000000
    end
end

function on_disable()
    log_message(string.format("J1939 Engine ECU stopped at %.1f km/h, %.0f rpm", vehicle_speed, engine_rpm))
end

function on_timer()
    local now = get_time_us()

    -- The time since the last tick, from the clock and not from the timer period: on_timer runs at
    -- most once per dispatch pass, so the real period is the timer's plus however late the pass was,
    -- and a model that counted ticks would run slow by exactly that. Capped, so a stall of the
    -- executor is a long tick and not a leap.
    local dt_sec = last_tick_us and math.min((now - last_tick_us) * 1.0e-6, 0.25) or (kTickMs / 1000.0)
    last_tick_us = now

    local target_v = 0.0

    if kScenario == "cold_start" then
        target_v = 0.0
        coolant_temp = math.min(88.0, coolant_temp + dt_sec * 0.4)
        oil_temp = math.min(90.0, oil_temp + dt_sec * 0.3)
    elseif kScenario == "highway" then
        target_v = 100.0
        cruise_active = 1
        cruise_set_speed = 100
    elseif kScenario == "parked" then
        target_v = 0.0
        parking_brake = 1
    elseif kScenario == "overheating" then
        target_v = 80.0
        coolant_temp = math.min(122.0, coolant_temp + dt_sec * 0.8)
        oil_temp = math.min(125.0, oil_temp + dt_sec * 0.6)
    elseif kScenario == "low_oil_pressure" then
        target_v = 60.0
        oil_pressure = 50.0 -- 50 kPa triggers the oil lamp and a DTC
    elseif kScenario == "sensor_error" then
        target_v = 50.0
        coolant_temp = nil  -- nil encodes as J1939 "not available"
    else
        local elapsed_sec = (now - cycle_start_us) * 1.0e-6
        target_v = get_target_speed(elapsed_sec)
    end

    -- Smooth speed update towards target
    local dv = target_v - vehicle_speed
    local max_accel = 15.0 * dt_sec -- km/h per tick
    if dv > max_accel then
        vehicle_speed = vehicle_speed + max_accel
        accelerator_pedal = math.min(100.0, (dv / 20.0) * 100.0)
        brake_switch = 0
    elseif dv < -max_accel then
        vehicle_speed = vehicle_speed - max_accel
        accelerator_pedal = 0.0
        brake_switch = 1
    else
        vehicle_speed = target_v
        accelerator_pedal = vehicle_speed > 5.0 and 25.0 or 0.0
        brake_switch = 0
    end

    -- Gear and RPM derivation
    current_gear, engine_rpm = select_gear_and_rpm(vehicle_speed)

    -- Engine load & torque
    engine_load = math.max(12.0, math.min(100.0, accelerator_pedal * 0.85 + (engine_rpm / kMaxRpm) * 20.0))
    actual_torque = engine_load

    -- Fuel consumption model (SPN 183 in L/h)
    fuel_rate = (engine_rpm / 1000.0) * (engine_load / 100.0) * 4.2 + 0.8
    if vehicle_speed > 5.0 and fuel_rate > 0.01 then
        inst_fuel_econ = vehicle_speed / fuel_rate
    else
        inst_fuel_econ = 0.0
    end

    -- Oil pressure follows RPM when not faulted
    if kScenario ~= "low_oil_pressure" then
        oil_pressure = 180.0 + (engine_rpm / 3000.0) * 220.0
    end

    -- Thermal model normal response
    if kScenario ~= "overheating" and kScenario ~= "cold_start" and kScenario ~= "sensor_error" then
        local target_coolant = 88.0 + (engine_load / 100.0) * 6.0
        coolant_temp = coolant_temp + (target_coolant - coolant_temp) * (dt_sec * 0.05)
        oil_temp = oil_temp + (coolant_temp + 3.0 - oil_temp) * (dt_sec * 0.03)
    end

    -- Accumulate hours and revolutions
    if engine_rpm > 50.0 then
        engine_hours = engine_hours + (dt_sec / 3600.0)
        engine_revs = engine_revs + (engine_rpm * (dt_sec / 60.0))
    end

    ----------------------------------------------------------------------------
    -- CAN Broadcasts
    ----------------------------------------------------------------------------

    -- 1. EEC1 (PGN 0xF004, 20 ms, Prio 3)
    if now >= next_eec1_us then
        next_eec1_us = now + kEec1IntervalUs
        local eec1_payload
        if override_eec1_payload ~= nil and override_eec1_until_us ~= nil and now < override_eec1_until_us then
            eec1_payload = override_eec1_payload
        else
            override_eec1_payload = nil
            local b1 = accelerator_pedal > 0.0 and 1 or 0 -- Torque mode: 1 accelerator pedal, 0 low idle governor
            local b2 = raw(engine_load, 1, -125, 1) -- Driver demand torque
            local b3 = raw(actual_torque, 1, -125, 1) -- Actual torque
            local b45 = raw(engine_rpm, 0.125, 0, 2) -- Engine speed
            eec1_payload = string.pack("<I1I1I1I2I1", b1, b2, b3, b45, kSa)
        end
        last_sent_eec1 = eec1_payload
        send(3, 0xF004, kSa, eec1_payload)
    end

    -- 2. EEC2 (PGN 0xF003, 50 ms, Prio 3)
    if now >= next_eec2_us then
        next_eec2_us = now + kEec2IntervalUs
        local b2 = raw(accelerator_pedal, 0.4, 0, 1)
        local b3 = raw(engine_load, 1, 0, 1)
        send(3, 0xF003, kSa, string.pack("<I1I1I1", 0xFF, b2, b3))
    end

    -- 3. ETC2 (PGN 0xF005, 100 ms, Prio 3): the gear, which is -1 reverse, 0 neutral, 1.. forward
    if now >= next_etc2_us then
        next_etc2_us = now + kEtc2IntervalUs
        local etc2_payload
        if override_etc2_payload ~= nil and override_etc2_until_us ~= nil and now < override_etc2_until_us then
            etc2_payload = override_etc2_payload
        else
            override_etc2_payload = nil
            local gear = raw(current_gear, 1, -125, 1)
            etc2_payload = string.pack("<I1I2I1", gear, 0xFFFF, gear)
        end
        last_sent_etc2 = etc2_payload
        send(3, 0xF005, kSa, etc2_payload)
    end

    -- 4. CCVS1 (PGN 0xFEF1, 100 ms, Prio 6)
    if now >= next_ccvs1_us then
        next_ccvs1_us = now + kCcvs1IntervalUs
        local ccvs1_payload
        if override_ccvs1_payload ~= nil and override_ccvs1_until_us ~= nil and now < override_ccvs1_until_us then
            ccvs1_payload = override_ccvs1_payload
        else
            override_ccvs1_payload = nil
            -- Byte 1: bits 3-4 parking brake switch. Byte 4: bits 1-2 cruise control active, bits 5-6
            -- brake switch; the bits in between and above are the ones this ECU does not report, and are
            -- set - "not available" - without touching the two fields it does.
            local b1 = 0xF3 | ((parking_brake & 0x03) << 2)
            local b23 = raw(vehicle_speed, 1.0 / 256.0, 0, 2)
            local b4 = 0xCC | (cruise_active & 0x03) | ((brake_switch & 0x03) << 4)
            local b6 = raw(cruise_set_speed, 1, 0, 1)
            ccvs1_payload = string.pack("<I1I2I1I1I1I1I1", b1, b23, b4, 0xFF, b6, 0xFF, 0xFF)
        end
        last_sent_ccvs1 = ccvs1_payload
        send(6, 0xFEF1, kSa, ccvs1_payload)
    end

    -- 5. LFE1 (PGN 0xFEF2, 100 ms, Prio 6)
    if now >= next_lfe1_us then
        next_lfe1_us = now + kLfe1IntervalUs
        local b12 = raw(fuel_rate, 0.05, 0, 2)
        local b34 = raw(inst_fuel_econ, 1.0 / 512.0, 0, 2)
        local b56 = raw(avg_fuel_econ, 1.0 / 512.0, 0, 2)
        send(6, 0xFEF2, kSa, string.pack("<I2I2I2", b12, b34, b56))
    end

    -- 6. EFL_P1 (PGN 0xFEEF, 500 ms, Prio 6)
    if now >= next_efl_us then
        next_efl_us = now + kEflIntervalUs
        local efl_payload
        if override_efl_payload ~= nil and override_efl_until_us ~= nil and now < override_efl_until_us then
            efl_payload = override_efl_payload
        else
            override_efl_payload = nil
            local b3 = raw(oil_level, 0.4, 0, 1)
            local b4 = raw(oil_pressure, 4, 0, 1)
            local b8 = raw(coolant_level, 0.4, 0, 1)
            efl_payload = string.pack("<I1I1I1I1I1I1I1I1", 0xFF, 0xFF, b3, b4, 0xFF, 0xFF, 0xFF, b8)
        end
        last_sent_efl = efl_payload
        send(6, 0xFEEF, kSa, efl_payload)
    end

    -- 7. ET1 (PGN 0xFEEE, 1 s, Prio 6)
    if now >= next_et1_us then
        next_et1_us = now + kEt1IntervalUs
        local et1_payload
        if override_et1_payload ~= nil and override_et1_until_us ~= nil and now < override_et1_until_us then
            et1_payload = override_et1_payload
        else
            override_et1_payload = nil
            local b1 = raw(coolant_temp, 1, -40, 1)
            local b2 = raw(fuel_temp, 1, -40, 1)
            local b34 = raw(oil_temp, 0.03125, -273, 2)
            et1_payload = string.pack("<I1I1I2", b1, b2, b34)
        end
        last_sent_et1 = et1_payload
        send(6, 0xFEEE, kSa, et1_payload)
    end

    -- 8. AMB (PGN 0xFEF5, 1 s, Prio 6)
    if now >= next_amb_us then
        next_amb_us = now + kAmbIntervalUs
        local amb_payload
        if override_amb_payload ~= nil and override_amb_until_us ~= nil and now < override_amb_until_us then
            amb_payload = override_amb_payload
        else
            override_amb_payload = nil
            local b1 = raw(baro_press, 0.5, 0, 1)
            local b45 = raw(ambient_temp, 0.03125, -273, 2)
            local b6 = raw(intake_temp, 1, -40, 1)
            amb_payload = string.pack("<I1I1I1I2I1", b1, 0xFF, 0xFF, b45, b6)
        end
        last_sent_amb = amb_payload
        send(6, 0xFEF5, kSa, amb_payload)
    end

    -- 9. HOURS (PGN 0xFEE5, 1 s, Prio 6)
    if now >= next_hours_us then
        next_hours_us = now + kHoursIntervalUs
        local hours_payload
        if override_hours_payload ~= nil and override_hours_until_us ~= nil and now < override_hours_until_us then
            hours_payload = override_hours_payload
        else
            override_hours_payload = nil
            local b14 = raw(engine_hours, 0.05, 0, 4)
            local b58 = raw(engine_revs / 1000.0, 1, 0, 4)
            hours_payload = string.pack("<I4I4", b14, b58)
        end
        last_sent_hours = hours_payload
        send(6, 0xFEE5, kSa, hours_payload)
    end

    -- 10. DM1 (PGN 0xFECA, 1 s, Prio 6)
    if now >= next_dm1_us then
        next_dm1_us = now + kDm1IntervalUs
        local dm1_payload
        if override_dm1_payload ~= nil and override_dm1_until_us ~= nil and now < override_dm1_until_us then
            dm1_payload = override_dm1_payload
        else
            override_dm1_payload = nil
            if kScenario == "low_oil_pressure" then
                -- MIL=0, Red=1 (Red Stop Lamp), Amber=0, Protect=0, SPN=100 (Oil Pressure), FMI=1 (Low severely), OC=1
                dm1_payload = dm1(0, 1, 0, 0, 100, 1, 1)
            elseif kScenario == "overheating" and coolant_temp >= 105.0 then
                -- MIL=1, Red=1, Amber=0, Protect=0, SPN=110 (Coolant Temp), FMI=0 (High severely), OC=1
                dm1_payload = dm1(1, 1, 0, 0, 110, 0, 1)
            elseif kScenario == "sensor_error" then
                -- Amber=1, SPN=110 (Coolant Temp), FMI=2 (Data erratic or incorrect), OC=1
                dm1_payload = dm1(0, 0, 1, 0, 110, 2, 1)
            else
                -- Clean DM1: all lamps 00, no active DTC
                dm1_payload = dm1(0, 0, 0, 0, 0, 0, 0)
            end
        end
        last_sent_dm1 = dm1_payload
        send(6, 0xFECA, kSa, dm1_payload)
    end
end
