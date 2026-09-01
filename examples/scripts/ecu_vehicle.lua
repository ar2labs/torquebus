-- ecu_vehicle.lua
--
-- A cyclic ECU: sends vehicle speed and engine temperature on a timer and
-- never looks at the bus. This is the shape most simulated ECUs have.
--
-- Ported from cansim's ecu_vehicle.lua. What changed, and why it is worth
-- knowing if you are bringing your own scripts over:
--
--   * float_to_bytes()  ->  string.pack("<f", value)
--     The original hand-rolled IEEE 754 in twenty-odd lines with math.frexp
--     and math.ldexp, both of which Lua 5.4 removed. string.pack has been in
--     the standard library since 5.3, is correct for denormals and infinities,
--     and is faster. It is also how a DBC-decoded float will be written.
--
--   * emit{id=..., len=..., data={bytes}}  ->  emit(id, data)
--     Frames carry a byte string, not a table of numbers: string.pack builds
--     one directly, and a table of eight numbers costs an allocation per frame.
--     The length is #data, so there is nothing to keep in step.
--
--   * print()  ->  log_message()
--     Goes to the Output panel with this node's name in front of it, rather
--     than to a console nobody is watching.
--
--   * tick accumulators  ->  get_time_us()
--     cansim's scripts add the timer interval to a counter on every on_timer
--     and compare it against a period. That assumes on_timer arrives exactly
--     on schedule. TorqueBus checks the timer once per dispatch pass, so a
--     tick can be late, and a late tick makes an accumulator drift slow -
--     silently, and further the busier the machine. Reading the clock instead
--     costs one call and cannot drift.

-- Settings come from the node, not from constants here, so one script can be
-- two vehicles on two identifiers. `or` gives each a default, which means the
-- script also runs unconfigured - useful while you are writing it.
local kVehicleSpeedId = parameters.speed_id or 0x101
local kEngineTempId = parameters.temp_id or 0x102

local kTickMs = parameters.tick_ms or 100
local kSpeedIntervalUs = (parameters.speed_interval_ms or 100) * 1000
local kTempIntervalUs = (parameters.temp_interval_ms or 1000) * 1000

local kAcceleration = parameters.acceleration or 1.5   -- km/h per tick
local kTopSpeed = parameters.top_speed or 90.0

local vehicle_speed = parameters.initial_speed or 0.0  -- km/h
local engine_temp = parameters.initial_temp or 70.0    -- degrees C

local next_speed_us = 0
local next_temp_us = 0

function on_enable()
    log_message(string.format("vehicle ECU ready, speed on 0x%03X", kVehicleSpeedId))
    set_timer(kTickMs)
end

function on_disable()
    log_message(string.format("vehicle ECU stopped at %.1f km/h", vehicle_speed))
end

function on_timer()
    local now = get_time_us()

    -- A gentle drive cycle, so the plot has something to show: accelerate to
    -- 90 km/h, then coast back down.
    vehicle_speed = vehicle_speed + kAcceleration
    if vehicle_speed > kTopSpeed then
        vehicle_speed = 0.0
    end

    -- Engine temperature follows load, loosely and with a lag.
    engine_temp = engine_temp + (vehicle_speed > 45.0 and 0.05 or -0.02)
    if engine_temp < 70.0 then
        engine_temp = 70.0
    end

    if now >= next_speed_us then
        next_speed_us = now + kSpeedIntervalUs
        -- 16-bit raw with a 0.1 km/h factor - the usual DBC encoding, and the
        -- reason to round rather than truncate.
        local raw = math.floor(vehicle_speed / 0.1 + 0.5)
        emit(kVehicleSpeedId, string.pack("<I2", raw & 0xFFFF))
    end

    if now >= next_temp_us then
        next_temp_us = now + kTempIntervalUs
        -- Offset -40, factor 1, one byte: the classic temperature encoding.
        local raw = math.floor(engine_temp + 40.0 + 0.5)
        emit(kEngineTempId, string.pack("<I1", raw & 0xFF))
    end
end
