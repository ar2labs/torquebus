-- ecu_motor.lua
--
-- A reactive automotive powertrain & throttle actuator ECU: takes commands from
-- a central vehicle gateway, reports status on a timer, and answers a
-- firmware-version request.
--
-- Frame protocol on the control bus:
--
--   byte 1  source module id
--   byte 2  destination (0xFF = broadcast)
--   byte 3  command
--   byte 4+ arguments
--
-- TorqueBus passes the CAN payload to `on_message` as a Lua byte string.
-- `string.byte(data, n)` reads single bytes with 1-based indexing, and
-- `string.unpack` decodes multi-byte integers and IEEE 754 floats directly.

-- An automotive powertrain network can host multiple actuator controllers
-- (throttle body, cooling fan, turbo wastegate), each with its own module id,
-- so the id is a parameter rather than a constant.
local kModuleId = parameters.module_id or 0x01
local kBroadcastId = parameters.broadcast_id or 0xFF

-- Commands
local kCmdTargetRpm = 0x01
local kCmdThrottleLimit = 0x03
local kCmdSensorInterval = 0x0F
local kCmdGearRatioConfig = 0x17
local kCmdSportMode = 0x19
local kCmdFirmwareVersion = 0x1A
local kCmdModuleStatus = 0x05

local target_rpm = 850.0      -- rpm
local throttle_limit = 100.0  -- %
local sensor_interval = 1000  -- ms
local gear_ratio = 1.0
local limp_timeout = 0        -- ms
local limp_guard_enabled = false
local sport_mode = false
local module_status = 1       -- 1 = running normally

local firmware_version = {1, 0, 0}

local kTickMs = parameters.tick_ms or 20
local kStatusIntervalUs = (parameters.status_interval_ms or 500) * 1000

-- Against the clock, not a tick counter: on_timer runs at most once per
-- dispatch pass, so counting ticks drifts slow whenever a tick is late. See
-- the note in ecu_vehicle.lua.
local next_status_us = 0

local function send(...)
    -- One place that knows the header, so a command handler below is one line.
    emit(kModuleId, string.pack("<I1I1", kModuleId, kBroadcastId) .. string.pack(...))
end

local function send_module_status()
    send("<I1I1", kCmdModuleStatus, module_status)
end

local function send_firmware_version()
    -- table.unpack, not the global unpack: 5.4 removed the global, and it had
    -- been deprecated since 5.1.
    send("<I1I1I1I1", kCmdFirmwareVersion, table.unpack(firmware_version))
end

local function handle_command(data)
    local command = string.byte(data, 3)

    if command == kCmdTargetRpm then
        -- Four bytes of IEEE 754, straight out of string.unpack.
        target_rpm = string.unpack("<f", data, 4)
        log_message(string.format("engine target speed at %.1f rpm", target_rpm))

    elseif command == kCmdThrottleLimit then
        throttle_limit = string.unpack("<f", data, 4)
        log_message(string.format("throttle actuator limit at %.1f %%", throttle_limit))

    elseif command == kCmdSensorInterval then
        sensor_interval = string.unpack(">I2", data, 4)
        log_message(string.format("sensor interval %d ms", sensor_interval))

    elseif command == kCmdGearRatioConfig then
        local whole, tenths, hundredths = string.byte(data, 4, 6)
        gear_ratio = whole + tenths / 10 + hundredths / 100
        limp_timeout = string.unpack(">I2", data, 7)
        limp_guard_enabled = limp_timeout > 0
        log_message(string.format("gear ratio %.2f, watchdog timeout %d ms",
                                  gear_ratio, limp_timeout))

    elseif command == kCmdSportMode then
        sport_mode = string.byte(data, 4) ~= 0
        log_message(sport_mode and "sport drive mode on" or "sport drive mode off")

    elseif command == kCmdFirmwareVersion then
        send_firmware_version()
    end
end

function on_enable()
    log_message(string.format("powertrain actuator ECU %d.%d.%d ready", table.unpack(firmware_version)))
    set_timer(kTickMs)
end

function on_disable()
    log_message("powertrain actuator ECU stopped")
end

function on_timer()
    local now = get_time_us()

    if now >= next_status_us then
        next_status_us = now + kStatusIntervalUs
        send_module_status()
    end
end

function on_message(id, data)
    -- Short frames are not for us and indexing past the end would be an error,
    -- which after five of them would take this ECU out of the measurement.
    if #data < 3 then
        return
    end

    local destination = string.byte(data, 2)
    if destination == kModuleId or destination == kBroadcastId then
        handle_command(data)
    end
end
