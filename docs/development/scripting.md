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

This is cansim's convention, unchanged, so scripts move across without editing
that part.

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

## Bringing cansim scripts over

The lifecycle is the same, deliberately — `on_enable`, `on_disable`,
`on_timer`, `on_message` come from cansim, which has twenty working ECUs behind
it. Four things changed.

| cansim | TorqueBus | Why |
|---|---|---|
| `emit{id=…, len=…, data={bytes}}` | `emit(id, data)` | A byte string, not a table of numbers: `string.pack` builds one directly, and a table of eight numbers costs an allocation per frame. `#data` is the length, so there is nothing to keep in step. |
| `msg.data[n]` | `string.byte(data, n)` | Same 1-based indexing; `string.unpack` replaces the byte shuffling entirely. |
| `print(...)` | `log_message(...)` | Reaches the Output panel instead of a console nobody is watching. |
| tick accumulators | `get_time_us()` | See `set_timer` above. |

And three things Lua removed between 5.1 and 5.4, which TorqueBus's 5.5 will
not accept:

| Removed | Use instead |
|---|---|
| `math.frexp` / `math.ldexp` | `string.pack("<f", value)` / `string.unpack("<f", data, offset)` |
| global `unpack` | `table.unpack` |
| `setfenv`, `module`, `loadstring`, `table.getn`, `math.pow` | `load`, `#t`, `^` |

The forty lines of hand-rolled IEEE 754 that open several cansim scripts
collapse to one `string.pack` call, which is also correct for denormals and
infinities and faster than the arithmetic version.

## Switching a node off

A node can be disabled rather than deleted. It stays in the project with its
settings and its position; it is skipped when the graph is built, along with
every wire that touches it, and anything downstream of it simply receives
nothing.

Use it to try a measurement without one ECU. Deleting the node and drawing it
again is the same experiment, minus its configuration.

## Examples

`examples/projects/virtual-vehicle.tbsproj` is a whole pipeline, ready to open:
a CAN channel, a simulated vehicle ECU with its script inline, and a transmit
block, plus a filtered second channel. Nothing to install — open it and press
Start.

`examples/scripts/` holds two ported ECUs, commented with what changed:

- **`ecu_vehicle.lua`** — cyclic: sends speed and engine temperature on a
  timer, never reads the bus.
- **`ecu_motor.lua`** — reactive: takes commands from a central node, reports
  status periodically, answers a firmware-version request.
