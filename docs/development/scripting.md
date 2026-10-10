# Lua ECU scripting

A Lua ECU is one node on the pipeline graph whose behaviour is a script. It
receives frames on its input port and puts frames on its output port; where
those come from and where they go is the graph's business, not the script's.

```
[CAN 1] ──► [Lua ECU] ──► [CAN 1 transmit]   a simulated node answering the bus
            [Lua ECU] ──► [CAN 1 transmit]   a cyclic node, no input at all
[CAN 1] ──► [Lua ECU]                        an observer that emits nothing
```

TorqueBus vendors **Lua 5.5.0** and builds it as part of the project — there is
no external Lua to install and no version to keep in step.

---


## Speaking in signals instead of bytes

Give the block a **Database** and two more functions appear.

```lua
emit_signal("VehicleSpeed", { SpeedKmh = 85.0, Gear = 3 })

local name, signals = decode(id, data)
if name == "VehicleSpeed" then
    log_message(string.format("%.1f km/h", signals.SpeedKmh))
end
```

The alternative is `string.pack("<I2", math.floor(speed / 0.1 + 0.5) & 0xFFFF)`,
which carries the identifier, the byte order and the scaling in the script - the
three things the database already knows, in the one notation here that is wrong
without ever looking wrong. A mis-packed frame transmits perfectly: right
length, right identifier, plausible number.

`examples/scripts/ecu_vehicle_dbc.lua` is the same ECU as `ecu_vehicle.lua`
written this way; the two are worth reading side by side.

**A wrong name stops the script. A wrong value does not.** A misspelled message
or signal is an error that names what was misspelled, because a typo never
becomes correct and the run should stop. A value the field cannot hold saturates
and is counted instead - a control loop briefly asking for 300% torque has a bug
worth seeing, but taking the ECU down over it would take the rest of the
simulation with it.

`decode` returns `nil` for an identifier the database does not describe, which
on a shared bus is most of them - so a script can look at everything and act on
the few messages it owns.

Signals a frame does not carry are **absent from the table, not zero**. On a
multiplexed message that is the difference between "this page has no voltage
reading" and "the voltage is zero", and the same holds for a frame that arrived
shorter than the database expects.


## Lifecycle

Four functions, all optional. Define the ones you need.

| Function | When it runs |
|---|---|
| `on_enable()` | Once, when the measurement starts. Set up state, call `set_timer`. |
| `on_message(id, data, channel, extended)` | Once per received frame. |
| `on_timer()` | Every `set_timer(ms)` milliseconds. |
| `on_disable()` | Once, when the measurement stops. |

The script body itself runs before `on_enable`, so anything at the top level —
constants, helper functions, initial state — is already in place.

**Every start reloads the script.** Pressing Start recreates the node, which
recompiles the source and resets every global. Edit a script, press Start, and
the new version runs; nothing carries over from the previous measurement.

## What a script can call

### `emit(id, data [, options])`

Puts one frame on the node's output port.

- `id` — the CAN identifier. 29-bit identifiers select the extended format
  automatically.
- `data` — the payload as a **byte string**, not a table. `string.pack` builds
  one; a literal like `"\x02\x01\x00"` is also a byte string.
- `options` — optional table: `extended`, `fd`, `brs`.

```lua
emit(0x7E8, string.pack("<I1I1I1", 0x02, 0x41, 0x0C))
emit(0x18FEE500, string.pack("<f", 12.5))
emit(0x100, payload, {fd = true, brs = true})
```

Nine bytes into a classic frame, or an identifier that does not fit the format,
is an error rather than a silent truncation — a shortened frame is a bug you
then chase on the bus. `emit` does not reach the wire on its own: wire the
node's output to a transmit node to put frames on a real channel.

### `set_timer(milliseconds)`

How often `on_timer` runs. `0` stops it.

The engine checks timers once per dispatch pass, every 5 ms by default, so that
is the practical resolution — asking for 1 ms gets you 5. Cyclic ECU messages
run at tens of milliseconds, where this does not bite.

**`on_timer` runs at most once per pass**, so a tick can be late and a tick is
never repeated to catch up. Counting ticks therefore drifts slow:

```lua
-- Drifts, silently, and further the busier the machine
elapsed = elapsed + 20
if elapsed >= 500 then elapsed = 0; send_status() end

-- Does not drift
local now = get_time_us()
if now >= next_status_us then
    next_status_us = now + 500 * 1000
    send_status()
end
```

### `log_message(text)`

One line in the Output panel, prefixed with this node's name.

### `get_time_us()`

Microseconds since the measurement started — the same clock the frame
timestamps are on, which is what makes comparing them meaningful.

## Globals a script starts with

| Name | Value |
|---|---|
| `node_name` | The node's name, as it appears in the UI |
| `channel` | The application channel `emit` stamps onto frames |
| `parameters` | The node's settings, as a table |

### `parameters` — what makes a script reusable

Every setting a script needs should come from `parameters`, not from a constant
at the top of the file. Otherwise one temperature sensor is one sensor, and four
of them are four copies of the same file that have to be edited together.

```lua
local can_id   = parameters.can_id or 0x100
local interval = parameters.update_interval or 1000
```

`parameters` is always defined, even when empty, so the `or` form works without
first testing that the table exists — which also means the script runs
unconfigured while you are writing it.

Anything set on the node lands here, except the three names the node itself
consumes: `script`, `scriptPath` and `channel`. Booleans, integers, reals and
strings all survive the crossing with their types intact.

## Timing: several rates at once

A real ECU sends a 10 ms message and a 100 ms one and a 1 s one, all at the same
time. `set_timer` gives a script one rate, which means faking the rest with a
counter - and a counter that divides an interval is a place for an off-by-one to
live for months.

### `every(milliseconds, function)`

As many as the script wants, each at its own rate.

```lua
function on_enable()
    every(10,   function() emit(0x100, engine_state()) end)
    every(100,  function() emit(0x200, temperatures()) end)
    every(1000, function() log_message("still here") end)
end
```

**It fires immediately and then on its period.** An ECU that went quiet for its
first cycle is a difference somebody notices, and a script that wants the delay
can check `tb.now()`. Jobs due in the same pass run in the order they were
declared.

The period is milliseconds and has to be positive; the second argument has to be
a function. Both are refused where they were written rather than at the first
tick.

### `cyclic(id, milliseconds, data_or_function [, options])`

A message that sends itself, with no timer body and no bookkeeping - an ECU's
periodic traffic is a list of facts rather than a program.

```lua
function on_enable()
    cyclic(0x123, 20, "\xAA\xBB")            -- fixed bytes

    local counter = tb.counter(4)
    cyclic(0x321, 10, function()              -- fresh bytes each time
        return string.char(counter(), engine_temperature())
    end)
end
```

A provider that returns nothing **skips that cycle**, which is how a message
that only goes out while a condition holds is written. Declaring the same
identifier twice replaces the first declaration rather than sending it twice.

### `stop_cyclic(id [, running])`

Silences a message, and brings it back:

```lua
stop_cyclic(0x123)         -- quiet
stop_cyclic(0x123, true)   -- talking again
```

Resuming fires on the next cycle rather than sending everything that was missed:
a burst looks like a fault in the tool rather than the one being injected.
Calling it with the state it is already in does nothing at all - a script saying
`stop_cyclic(id, condition)` from a fast timer must not keep pushing the message
it is trying to keep alive.

## The `tb` prelude

A small standard library, written in Lua and loaded before every script. All of
it is arithmetic over the measurement clock:

| Function | What it gives |
|---|---|
| `tb.now()` | Seconds since the measurement began. |
| `tb.ramp(low, high, period)` | Sweeps up and jumps back. |
| `tb.sine(low, high, period)` | Sweeps up and back down, smoothly. |
| `tb.square(low, high, period)` | Half the period low, half high. |
| `tb.drift(low, high, step)` | Wanders rather than jumping - noise that looks like a sensor. |
| `tb.steps({...}, period)` | Walks a list, one entry per period. |
| `tb.counter(bits)` | A counter that wraps, as nearly every real message carries. |
| `tb.value(x)` | `x()` if it is a function, `x` otherwise. |

Each generator returns a **function**: call it to get the value now.

```lua
local speed = tb.sine(800, 3000, 20)   -- engine speed over twenty seconds

cyclic(0x0C0, 10, function()
    return string.pack("<I2", math.floor(speed()))
end)
```

They are Lua rather than C++ on purpose: a binding for each would be thirty
lines of stack juggling to express three lines of maths, and in Lua they can be
read, copied and changed by the person using them.


## J1939 helpers

An ECU on a J1939 bus is mostly arithmetic of one kind: a physical value turned into the raw number
the standard puts on the wire, an identifier built from a priority, a PGN and an address, a message
padded the way J1939 pads one. They are in the prelude, and the example ECUs in
`examples/scripts/ecu_*.lua` are written with them.

| Function | What it gives |
|---|---|
| `j1939_id(prio, pgn, sa)` | The 29-bit identifier. `prio` defaults to 6. |
| `j1939_raw(value, res, offset, nbytes)` | `round((value - offset) / res)`, held to the biggest **valid** value of an `nbytes` field (`0xFA`, `0xFAFF`, `0xFAFFFF`, `0xFAFFFFFF`) and not wrapped into the ones J1939 keeps for "error". `nil` - and a number that is not one - is **not available** (`0xFF`, `0xFFFF`...), which is how a script says a sensor has failed. |
| `j1939_send(prio, pgn, sa, payload)` | `emit()` of that identifier, always as a 29-bit frame, with the payload padded with `0xFF` to eight bytes. |
| `j1939_dm1(mil, red, amber, protect, spn, fmi, oc)` | The payload of a DM1 with one trouble code; lamps are 0 off, 1 on. `j1939_dm1(0, 0, 0, 0, 0, 0, 0)` is J1939's "no active fault". |

`raw`, `send` and `dm1` are short names for the same functions, as the example ECUs write them.
They are globals, so a script of your own that defines one of those names uses its own.

**`send` means two things, and which depends on where it is.** In an ECU it is the J1939 message
above - or, called the old way, `send(id, payload [, options])`, exactly `emit()`. In a
[test sequence](testing.md#sending) it is the node's own `send(id, payload [, options])` and is left
alone; `j1939_send` works there as well.

```lua
function on_timer()
    -- 85.0 km/h as SPN 84 (1/256 km/h per bit), in bytes 2 and 3 of CCVS1
    local speed = raw(85.0, 1 / 256, 0, 2)
    send(6, 0xFEF1, 0x00, string.pack("<I1I2", 0xFF, speed))
end
```

The cycle time of each message is the script's to keep. Count from the **clock** and not from the
timer: `on_timer` runs at most once per dispatch pass, so its real period is the timer's plus
however late the pass was, and a model that adds `tick_ms` each time runs slow by exactly that. The
example ECUs measure the time since the last tick (`get_time_us()`) and cap it, so a stall of the
executor is one long tick and not a leap.

`on_message(id, data, channel, extended)` gets `extended` as `1` or `0`, and **`0` is true in Lua** -
compare it, `extended == 1`.

## Asking about the bus

`on_message` says what arrived on this node's input. These say what the
*measurement* has seen - every identifier on every channel, whether or not it is
wired into this block.

### `bus_last(id [, channel])`

The payload of the most recent frame with that identifier, and a table about it:

```lua
local data, info = bus_last(0x123)

if data then
    log_message(("last seen %d us ago, cycle %d us, %d times")
        :format(get_time_us() - info.timestamp_us, info.cycle_us, info.count))
end
```

| Field | Meaning |
|---|---|
| `count` | How many have been seen. |
| `channel` | Which application channel it was on. |
| `cycle_us` | Microseconds since the previous one. |
| `min_cycle_us`, `max_cycle_us` | The extremes so far. |
| `timestamp_us` | When the last one arrived. |
| `changed_bytes` | Bitmask of the bytes that differed from the frame before. |

**Nil when nothing has been seen yet** - not empty bytes, because "no frame" and
"a frame with no payload" are different things and a script writing `if data
then` has to be able to tell them apart.

### `bus_stats()`

`frames`, `identifiers`, `retained` and `discarded` - the last being what the
trace's ring has overwritten, which is what a script deciding something from a
count needs in order to know when the count stopped being all of them.

Both are refused, with the name of the function, in a graph that has no trace.

## Being deliberately wrong

A tool that only ever sends correct traffic tests half of a receiver: the half
that works. What a stuck counter, a stale checksum or a DLC that lies do to the
rest of the network is the question a bench exists to answer.

### `fault(id, spec)` and `fault(id)`

Applied to every frame leaving this node with that identifier - `emit`, `cyclic`
and even the diagnostic responses, so corrupting a UDS answer to see what a
tester does is one line.

```lua
fault(0x123, { freeze = true })                    -- a stuck ECU
fault(0x123, { dlc = 8 })                          -- claims eight, sends three
fault(0x123, { truncate = 2 })                     -- sends two of eight
fault(0x123, { flip = { [1] = 0xFF, [4] = 0x01 } })-- invert bits, one-based
fault(0x123)                                       -- back to normal
```

`freeze` repeats the last payload that went out. It stops a rolling counter and
stales a checksum **without this code knowing which byte is which**, which is
the only way to do it without a database describing the message - and it is what
a genuinely stuck ECU looks like on the wire.

A fault declared again keeps what it has *seen*: writing `fault(id, { freeze =
true })` from a timer means "freeze from now on", and resetting the remembered
payload on every call would mean it never froze at all.

The Statistics panel counts corrupted frames separately, because **an injected
fault left switched on is the likeliest reason a later measurement makes no
sense.**

### Checksums, so that breaking one means something

`tb.crc8(bytes)` is CRC-8/SAE-J1850 - polynomial 0x1D, initial 0xFF, final XOR
0xFF - which is what AUTOSAR's end-to-end profiles 1 and 2 use and what most
vehicle messages carry. `tb.e2e(payload, counter)` builds the usual shape: the
checksum in the first byte, over everything after it, and the counter in the low
nibble of the second.

```lua
local counter = tb.counter(4)
local speed = tb.sine(800, 3000, 20)

cyclic(0x0C0, 10, function()
    return tb.e2e(string.pack("<I2", math.floor(speed())), counter())
end)
```

They are here so a script can build a message that is **correct**: flipping a
bit in a checksum nobody computed proves nothing.


## Answering diagnostics

Give the block a **Diagnostic request ID** and a **Diagnostic response ID** and
it stops being only a frame generator: it carries an ISO-TP transport and a UDS
server, and the script says what the ECU knows.

The identifiers are the ECU's way round. It **listens** on what a tester
transmits - request 0x7E0, response 0x7E8 for the usual pair - and getting that
backwards is the commonest reason a simulated ECU is never heard from.

```lua
function on_enable()
    uds_did(0xF190, "WVWZZZ1KZAW000001")     -- readable by anybody
    uds_did(0x2001, string.char(0x00, 0x64),
            { writable = true, session = 3, security = true })

    uds_dtc(0x012800, 0x2F)                  -- a stored fault
end
```

Everything ordinary then works without another line: reads, writes, the DTC
list, clearing it, session changes, TesterPresent, and the refusals - a DID that
does not exist, one that needs a session the tester is not in, one that needs
the ECU unlocked. **The refusals are the point.** An ECU that answers everything
is a mirror, and a tester that passes against a mirror fails on the bench.

### `uds_did(identifier, value [, options])`

`identifier` is the two-byte DID; `value` is a byte string. Options:

| Option | Meaning |
|---|---|
| `writable` | WriteDataByIdentifier may change it. Off by default - a part number is not writeable. |
| `session` | The session it needs: 1 default, 2 programming, 3 extended. |
| `security` | Whether the ECU has to be unlocked first. |

Call it again with the same identifier to change the value, which is how a
value that moves is published - see `ecu_uds.lua`.

### `uds_dtc(code [, status])` and `uds_clear_dtc()`

`code` is the 24-bit trouble code: the first two bytes are what a workshop
manual indexes (0x0128 is P0128) and the third is the failure type. `status`
defaults to 0x08, "confirmed", which is what a scan tool shows as stored.

Setting the same code twice updates it rather than storing it twice.

### `uds_session()`

Returns the session the ECU is in and whether it is unlocked:

```lua
local session, unlocked = uds_session()
```

Both, because a script that keeps its own copy will be wrong every time the
session expires underneath it - which it does, after five seconds of silence,
exactly as a real ECU's does.

### `function on_security_seed(seed)`

Returns the key the ECU will accept for that seed. Without this function the
ECU is **locked**, not open: answering a seed no key can match would leave a
tester trying for ever.

### `function on_uds_request(request)`

First refusal on every request, before the server sees it. Three answers,
because a real ECU has three:

| Return | Meaning |
|---|---|
| a byte string | This is the response. |
| nothing | Not the script's business - the server answers. |
| `false` | **Say nothing at all.** |

That last one is a dead ECU, a busy one, a wire that fell off. It is the case a
tester has to survive and the only one nothing else can simulate.

A script that throws inside this is reported like any other error, and the
server answers as it would have - a tester meeting a broken script should see
the ECU that was configured, not silence.


## The sandbox

Scripts get `base`, `table`, `string`, `math`, `utf8` and `coroutine`.

`os`, `io` and `package` are **absent**, which also means no `require`. A
simulation script has no business opening files or spawning processes, and
`require` would let one reach outside by path. Scripts that need to share code
will get an explicit mechanism rather than a filesystem search path.

## When a script goes wrong

An error inside a callback is caught, reported with its file and line, and the
measurement carries on — one broken simulated ECU should not end a recording.

After **five consecutive errors** the node says so once and goes quiet. A
script that throws on every frame would otherwise write 150,000 log lines a
second and bury everything useful. A single failure among successes resets the
count, so a transient fault costs one log line, not the node.

A script that fails to *compile* fails the start instead, with the Lua message
and its line number — better than discovering it three seconds into a
recording.

## Editing a script while it runs

Stop, edit, Start throws away the measurement — the trace, the state, the fault
you had just provoked — to change a cycle time from 10 to 20. So the **Script**
panel does not need any of that: select a Lua ECU block, edit, press **Reload**
(or F5), and the node picks the new script up on its next pass. The measurement
keeps running, the clock keeps counting, and the other ECUs never notice.

The rule that makes this usable rather than merely quick:

> **A script that fails to load leaves the running one alone.**

An edit is usually broken — that is what editing is. A syntax error, or an
`on_enable` that throws, is refused: the offending line is marked in the editor,
the reason appears under it and in the Output panel, and the ECU on the bus goes
on sending exactly what it was sending. Nothing about the measurement changed.

What a reload does and does not reset:

| | On reload |
|---|---|
| The interpreter, globals, timers, `cyclic` messages, injected faults | Rebuilt from the new script |
| `get_time_us()` and everything in `tb` built on it | **Keeps counting from Start** |
| The trace, the statistics, the other nodes | Untouched |
| A node that had gone quiet after five errors | Runs again — the edit is usually the fix |

The old script's `on_disable` runs *after* the new one has loaded, against its
own interpreter, so a teardown means what it meant while that script was
running. A refused reload runs no `on_disable` at all: nothing happened.

Where the text is written back depends on where the script lives. A script kept
inline in the project is written into the block's `script` setting; a script
kept in a file is written to the file — which also means you can keep editing it
in your own editor and press Reload here.

While a measurement is *not* running the same button says **Save**, because
that is all it can honestly do.

## Values a dashboard shares

```lua
var_get("brake_pedal")          -- what somebody's hand is doing to a slider
var_set("engine_speed", 2400)   -- what a gauge will show
```

A **system variable** is a named number that is not a CAN signal and not a
diagnostic result — it belongs to the simulation rather than to the wire. A
slider writes one, a script reads it and puts it on the bus; a script writes
one and a gauge shows it. That is what makes a dashboard more than a second
trace window: the person turns a knob and the simulated vehicle responds.

- Values are **numbers**, always. A switch is 0 or 1, a lamp is on above its
  threshold.
- A variable nobody has written reads as **zero**, so no script needs
  `var_get(x) or 0`.
- They are created on first mention, by whoever mentions them first — a script
  and a widget name the same variable without either having to declare it.
- **They survive Start and Stop.** A setpoint dialled in before a measurement is
  still there during it and after it.
- Reading and writing one takes no lock, so a variable can be touched inside
  `on_message` without the frame path ever waiting on a window.

Both bindings are absent in a graph with no dashboard behind it, and a script
calling one is told so — reading zeroes off a table that does not exist would
look like a pedal nobody is pressing, which is a fault that looks like data.

## Checking, rather than simulating

An ECU script answers "what does this network look like". The other half of the
question — "is it right" — belongs to a **Test Sequence** block, which is a Lua
script of a different shape: cases, `expect`, `assert_*`, and a verdict.

See [testing.md](testing.md). The `tb` prelude above is available there too.

## Cost

Measured on the pipeline, one ECU, 600,000 frames:

| Script | Per frame | Frames/s |
|---|---|---|
| Empty `on_message` | 83 ns | 12.0 M |
| Read payload with `string.byte` | 132 ns | 7.6 M |
| Read, unpack, emit one frame | 262 ns | 3.8 M |

A saturated 500 kbit/s bus carries about 4,000 frames per second, and 1 Mbit/s
CAN FD at 64 bytes under 15,000. A typical ECU script costs roughly 260 ns per
frame, which leaves room for dozens of them on one bus with the budget barely
touched.

---

## Modern Lua 5.4 / 5.5 idioms for CAN payloads

TorqueBus embeds Lua 5.5, so scripts use standard `string.pack` / `string.unpack`
instead of legacy Lua 5.1 bit-twiddling:

| Legacy Lua 5.1 pattern | TorqueBus (Lua 5.5) | Why |
|---|---|---|
| Table of bytes `{b1, b2, ...}` | `emit(id, data)` with `string.pack` | A byte string avoids allocating a Lua table on every frame. `#data` is the payload length automatically. |
| Manual byte shifting | `string.byte(data, n)` / `string.unpack` | 1-based indexing for single bytes and native IEEE 754 / endian unpacking for multi-byte values. |
| `print(...)` | `log_message(...)` | Reaches the Output panel with the node's name rather than stdout. |
| Tick accumulators | `get_time_us()` | Monotonic clock comparison prevents drift when a timer tick arrives late. |

And three things Lua removed between 5.1 and 5.4, which TorqueBus's 5.5 will
not accept:

| Removed | Use instead |
|---|---|
| `math.frexp` / `math.ldexp` | `string.pack("<f", value)` / `string.unpack("<f", data, offset)` |
| global `unpack` | `table.unpack` |
| `setfenv`, `module`, `loadstring`, `table.getn`, `math.pow` | `load`, `#t`, `^` |

## Switching a node off

A node can be disabled rather than deleted. It stays in the project with its
settings and its position; it is skipped when the graph is built, along with
every wire that touches it, and anything downstream of it simply receives
nothing.

Use it to try a measurement without one ECU. Deleting the node and drawing it
again is the same experiment, minus its configuration.

## Examples

`examples/projects/j1939-vehicle.tbsproj` is a J1939 truck: an engine, a brake, a body and a
switch-panel ECU, an aftertreatment module and the cluster's own core, all in Lua; the TinyML Virtual ECU
reading the same bus; a J1939 block and a Signal Plot feeding the instrument cluster on the Dashboard.
Each ECU takes a `scenario` parameter - `low_oil_pressure`, `overheating`, `abs_fault`, `hazard`... -
documented at the top of its script; set it on the block and press Start. It is described in
[cluster.md](cluster.md#the-j1939-example).

`examples/projects/virtual-vehicle.tbsproj` is a whole pipeline, ready to open:
a CAN channel, a simulated vehicle ECU with its script inline, a TinyML Virtual
ECU, and transmit/plot blocks, plus a filtered second channel. Nothing to
install — open it and press Start.

`examples/scripts/` holds reference automotive ECUs and test sequences:

- **`ecu_vehicle.lua`** — cyclic: sends vehicle speed and engine coolant
  temperature on a timer, never reads the bus.
- **`ecu_engine.lua`, `ecu_aftertreatment.lua`, `ecu_body.lua`, `ecu_brakes.lua`,
  `ecu_switches.lua`, `ecu_cluster_core.lua`** — the J1939 vehicle of
  `j1939-vehicle.tbsproj`, described by `examples/databases/j1939.dbc`. The body ECU listens to the
  switch panel and lights the lamps; the cluster core listens to the vehicle speed and integrates
  the odometer.
- **`ecu_motor.lua`** — reactive powertrain actuator ECU: takes target RPM,
  throttle limit, and drive mode commands from a central gateway, reports status
  periodically, and answers a firmware-version request.
