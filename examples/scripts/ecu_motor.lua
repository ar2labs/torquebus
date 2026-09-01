-- ecu_motor.lua
--
-- A reactive ECU: takes commands from a central node, reports status on a
-- timer, and answers a firmware-version request. Ported from cansim's
-- ecu_motor.lua, which drives a seed and fertiliser motor on a planter.
--
-- The protocol is cansim's, unchanged, because it is a real one:
--
--   byte 1  source module id
--   byte 2  destination (0xFF = broadcast)
--   byte 3  command
--   byte 4+ arguments
--
-- See ecu_vehicle.lua for what changed between the cansim and TorqueBus
-- script contracts. One more difference shows up here: cansim's on_message
-- took a table with .data indexed from 1, and TorqueBus passes the payload as
-- a byte string. string.byte(data, n) replaces msg.data[n], with the same
-- 1-based indexing, and string.unpack replaces the hand-written byte shuffling.

-- A planter has several of these, each with its own module id, so the id is a
-- parameter and not a constant - the same script is every motor on the bus.
local kModuleId = parameters.module_id or 0x01
local kBroadcastId = parameters.broadcast_id or 0xFF

-- Commands
local kCmdSeedSpeed = 0x01
local kCmdFertiliserSpeed = 0x03
local kCmdSensorInterval = 0x0F
local kCmdFertiliserConfig = 0x17
local kCmdPlantingMode = 0x19
local kCmdFirmwareVersion = 0x1A
local kCmdModuleStatus = 0x05

local seed_speed = 0.0        -- steps/s
local fertiliser_speed = 0.0  -- steps/s
local sensor_interval = 1000  -- ms
local reduction = 1.0
local fertiliser_timeout = 0  -- ms
local fertiliser_enabled = false
local planting_mode = false
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

    if command == kCmdSeedSpeed then
        -- Four bytes of IEEE 754, straight out of string.unpack. The original
        -- did this with math.frexp and forty lines; the wire format is the same.
        seed_speed = string.unpack("<f", data, 4)
        log_message(string.format("seed motor at %.2f steps/s", seed_speed))

    elseif command == kCmdFertiliserSpeed then
        fertiliser_speed = string.unpack("<f", data, 4)
        log_message(string.format("fertiliser motor at %.2f steps/s", fertiliser_speed))

    elseif command == kCmdSensorInterval then
        sensor_interval = string.unpack(">I2", data, 4)
        log_message(string.format("sensor interval %d ms", sensor_interval))

    elseif command == kCmdFertiliserConfig then
        local whole, tenths, hundredths = string.byte(data, 4, 6)
        reduction = whole + tenths / 10 + hundredths / 100
        fertiliser_timeout = string.unpack(">I2", data, 7)
        fertiliser_enabled = fertiliser_timeout > 0
        log_message(string.format("fertiliser reduction %.2f, timeout %d ms",
                                  reduction, fertiliser_timeout))

    elseif command == kCmdPlantingMode then
        planting_mode = string.byte(data, 4) ~= 0
        log_message(planting_mode and "planting mode on" or "planting mode off")

    elseif command == kCmdFirmwareVersion then
        send_firmware_version()
    end
end

function on_enable()
    log_message(string.format("motor ECU %d.%d.%d ready", table.unpack(firmware_version)))
    set_timer(kTickMs)
end

function on_disable()
    log_message("motor ECU stopped")
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
