-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The same vehicle ECU as ecu_vehicle.lua, written against a database.
--
-- Compare the two. This one says
--
--     emit_signal("VehicleSpeed", { SpeedKmh = speed })
--
-- where the other says
--
--     emit(kSpeedId, string.pack("<I2", math.floor(speed / 0.1 + 0.5) & 0xFFFF))
--
-- The second line carries three things the database already knows - the
-- identifier, the byte order, and the scaling - and it is the kind of line that
-- is wrong without ever looking wrong. A mis-packed frame transmits perfectly:
-- right length, right identifier, plausible number, silently incorrect.
--
-- Set the block's `database` parameter to `../databases/vehicle.dbc` and this
-- script needs to know none of it.

local kTickMs = parameters.tick_ms or 100
local kSpeedIntervalUs = (parameters.speed_interval_ms or 100) * 1000
local kTempIntervalUs = (parameters.temp_interval_ms or 1000) * 1000

local speed = 0.0
local temperature = 70.0

local next_speed_us = 0
local next_temp_us = 0

function on_enable()
    log_message("vehicle ECU ready, speaking DBC")
    set_timer(kTickMs)
end

function on_timer()
    -- Against the clock, not a tick counter: on_timer runs at most once per
    -- dispatch pass, so counting ticks drifts slow whenever one is late.
    local now = get_time_us()

    speed = speed + 1.5
    if speed > 90.0 then
        speed = 0.0
    end

    temperature = temperature + (speed > 45.0 and 0.05 or -0.02)
    if temperature < 70.0 then
        temperature = 70.0
    end

    if now >= next_speed_us then
        next_speed_us = now + kSpeedIntervalUs
        emit_signal("VehicleSpeed", { SpeedKmh = speed })
    end

    if now >= next_temp_us then
        next_temp_us = now + kTempIntervalUs
        emit_signal("EngineTemp", { EngTemp = temperature })
    end
end

function on_message(id, data, channel, extended)
    -- Everything on the bus arrives here; act only on what the database
    -- describes. decode() returns nil for the rest, which is most of it.
    local name, signals = decode(id, data)
    if name == "VehicleSpeed" and signals.SpeedKmh > 85.0 then
        log_message(string.format("over 85: %.1f km/h", signals.SpeedKmh))
    end
end
