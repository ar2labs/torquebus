# TorqueBus Studio

**Open Automotive Network & Diagnostics Workbench**

An open source platform for analysing, simulating, diagnosing and automating
automotive networks — a community alternative to TSMaster, CANalyzer/CANoe and
PCAN-Explorer.

> **Status: v0.6 — the visual canvas.** Connect an interface, press Start, read
> the bus: the trace holds **1,000,000 frames** and fills itself, with no wiring
> to do first. Beyond that, the Pipeline panel is where you draw one — blocks
> and wires, edited straight into the project. Simulated ECUs are Lua scripts
> that live on that pipeline as nodes, at ~260 ns per frame, so dozens fit on a
> bus that carries 4,000 frames/s.

---

## What it is

The ergonomics of professional automotive tooling — the panel layout, the
information density, the workflow an engineer already knows — with its own
visual identity, an open architecture and a GPLv3 licence.

- **A pipeline you draw, not a fixed set of panels.** Sources, decoders,
  filters and sinks are blocks you wire together on the Pipeline canvas:
  `[Kvaser CAN 1] → [DBC Decoder] → [J1939 Decoder] → [PGN Filter] → [Signal Plot]`.
  The graph *is* the data path, not a picture of it — and it builds itself for
  the simple case, so reading a bus never costs a canvas visit.
- **Simulated ECUs in Lua.** A script, a lifecycle, and frames on the wire.
  Editing a script and pressing Start is the whole edit-run loop.
- **Vendor-neutral core.** One frame type, one driver interface. Kvaser, PEAK
  and every future adapter are implementations, not special cases.
- **Built for throughput.** 190k+ frames/s with no loss, enforced by a test on
  every pull request rather than claimed afterwards.
- **Recording outlives the UI.** Close the Trace panel; the log keeps writing.

---

## Requirements

| | |
|---|---|
| Platform | Windows 11 x64 |
| Compiler | MSVC 2022 (x64), C++23 |
| Qt | **6.11.2 exactly**, with the **Qt SerialBus** module |
| Build | CMake ≥ 3.24, Ninja (both ship with Visual Studio) |

KDDockWidgets, Catch2 and Lua are built for you — nothing else to install.

The Qt version is an exact pin, not a minimum: KDDockWidgets uses Qt's private
modules, which tie the binary to the Qt build it was compiled against.

**Hardware is optional.** The built-in virtual bus runs the whole application
with no adapter attached. Kvaser needs CANlib and PEAK needs PCAN-Basic; without
them those backends still build and simply report themselves unavailable. Vendor
SDKs are never bundled.

---

## Build

```bat
tools\torquebus-prompt.bat

cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

`torquebus-prompt.bat` opens a prompt with **both** halves of the environment:
`vcvars64.bat` for MSVC and `qtenv2.bat` for Qt. A Qt prompt alone is not
enough — it sets up Qt and nothing else, and the build then fails at link time
with errors that look like a broken Windows SDK.

From *any* prompt, with no setup at all:

```bat
cmake --preset windows-msvc-vs
cmake --build --preset windows-msvc-vs
```

The Visual Studio generator finds its own toolchain, and CMake finds Qt at
`C:\Qt\6.11.2\msvc2022_64` or wherever `QTDIR` points. Slower to build than
Ninja; immune to whatever is on your `PATH`.

The executable lands in `build/<preset>/bin/`. Run it with `--reset-layout` if a
panel ends up somewhere unreachable.

Build options, CANlib in a custom location, and what to do when something goes
wrong: [`docs/development/getting-started.md`](docs/development/getting-started.md).

---

## Writing an ECU

A simulated ECU is a Lua script. Four optional callbacks, four calls back:

```lua
function on_enable()
    set_timer(100)                       -- on_timer every 100 ms
end

function on_timer()
    emit(0x101, string.pack("<I2", 850)) -- 85.0 km/h, 0.1 factor
end

function on_message(id, data, channel, extended)
    if id == 0x7DF then
        emit(0x7E8, "\2\1\0")            -- answer a diagnostic request
    end
end
```

Two ported examples live in [`examples/scripts/`](examples/scripts), and
[`examples/projects/virtual-vehicle.tbsproj`](examples/projects) is a working
pipeline you can open straight away — `File > Open Project` — with a simulated
vehicle ECU transmitting on CAN 1. The full contract — the sandbox, error handling, measured cost, and how to migrate
scripts from `cansim` — is in
[`docs/development/scripting.md`](docs/development/scripting.md).

---

## Architecture

Nothing in the UI talks to hardware; nothing in a driver knows the UI exists.
Between them sits a trivially copyable `CanFrame` that belongs to no vendor, and
an `ICanBackend` every adapter implements. Each channel owns a bounded lock-free
queue; one thread drains them all and runs a **typed dataflow graph** whose
nodes are the sources, decoders, filters, ECUs and panels. The graph is compiled
before a measurement starts and moves *batches*, never single frames — the only
way a visual pipeline survives bus speed. The canvas edits that graph; it does
not own it, and a headless run needs no GUI at all.

```
src/core/       vendor-neutral, Qt-free domain model
src/drivers/    ICanBackend and one implementation per vendor
src/services/   settings, projects, workspaces
src/ui/         widgets, theming, docking
tests/          unit / integration / hardware
```

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before your first pull
request — it is short, and it is the contract.

TorqueBus takes its workbench from TSMaster and CANoe, and its visual pipeline
from [CANdevStudio](https://github.com/GENIVI/CANdevStudio), which solved that
idea first. No code is shared — CANdevStudio is Qt5/C++17 on QtNodes 2.x, an
incompatible API generation — but the debt is real and worth naming.

---

## Roadmap

| | Milestone | Status |
|---|---|---|
| v0.1 | Foundation: shell, docking, themes, driver API, virtual bus | done |
| v0.2 | CAN core: engine, channels, queues, filtering, statistics | done |
| v0.3 | Kvaser CANlib backend | done |
| v0.4 | Pipeline graph: executor, typed ports, compiled topology | done |
| v0.5 | CAN Trace, the first real consumer node | done |
| v0.7 | Lua ECU blocks | done |
| v0.6 | QtNodes canvas — the graph becomes visible and editable | done |
| **v0.8** | DBC decoder node, `Signals` port | **next** |
| v0.9 | PEAK-System and SocketCAN backends | |
| v0.10 | Transmit and Graph nodes | |
| v0.11 | J1939 and **ISOBUS** (ISO 11783) | |
| v0.12 | Logger and Playback (`.tblog`, ASC, CSV) | |
| v0.13 | Workspaces; the `.tbsproj` already carries the pipeline | |
| v0.14 | ISO-TP and UDS, `Events` port | |
| v0.15 | Dashboard Designer (QML) | |
| v0.16 | Lua automation: headless runner, test scripting, reports | |
| v0.17+ | LIN, XCP/CCP, A2L, ARXML, DoIP, FlexRay | |

v0.7 was built before v0.6 deliberately: the Lua engine could be measured, and
it meant the canvas would edit a graph that already ran rather than being the
only way to find out whether it ran. The full plan is in [`PLAN.md`](PLAN.md).

---

## Contributing

Pull requests are welcome. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first — most review feedback here
is about layering, not style.

## Licence

GNU General Public License v3.0 or later — see [`LICENSE`](LICENSE).

TorqueBus Studio is not affiliated with, endorsed by, or derived from TOSUN,
Vector Informatik, PEAK-System or Kvaser. Product names are the trademarks of
their respective owners and are referenced only to describe interoperability.
