# TorqueBus Studio

**Open Automotive Network & Diagnostics Workbench**

An open source platform for the analysis, simulation, diagnostics and
automation of automotive networks — a community alternative to tools like
TSMaster, CANalyzer/CANoe and PCAN-Explorer.

> **Status: v0.5 — CAN Trace.** The tool is now usable for its main job:
> connect an interface, press Start, and read the bus. The trace holds
> **1,000,000 frames** with delta and cycle times computed on arrival, follows
> the tail until you scroll up to read, and says plainly when older frames have
> scrolled off. It fills itself — no canvas visit, no wiring.

---

## What it is

TorqueBus Studio borrows the *ergonomics* of professional automotive tooling —
the panel layout, the information density, the workflow an engineer already
knows — and pairs them with its own visual identity, an open architecture and a
GPLv3 licence.

- **A visual CAN pipeline, not a fixed set of panels.** Sources, decoders,
  filters and sinks are nodes you wire together — `[Kvaser CAN 1] → [DBC
  Decoder] → [J1939 Decoder] → [PGN Filter] → [Signal Plot]`. The graph is the
  data path, not a picture of it; and it builds itself for the simple case, so
  reading a bus never costs a canvas visit.
- **Vendor-neutral core.** One frame type, one driver interface. Kvaser, PEAK
  and every future adapter are implementations, not special cases.
- **Built for throughput.** The frame pipeline sustains 100k+ frames/s with no
  loss — a target that is enforced by a test on every pull request, not a claim
  retrofitted later.
- **Recording outlives the UI.** Close the Trace panel; the log keeps writing.
- **Extensible on purpose.** Drivers, database formats, protocols and scripting
  are all API boundaries a third party can implement.

---

## Requirements

| | |
|---|---|
| Platform | Windows 11 x64 |
| Compiler | MSVC 2022 (x64), C++23 |
| Qt | 6.11.2 exactly (Community / Open Source), with the **Qt SerialBus** module |
| Build | CMake ≥ 3.24 and Ninja |

`KDDockWidgets` and `Catch2` are fetched and built automatically at configure
time — nothing to install by hand.

The Qt version is an exact pin rather than a minimum: KDDockWidgets uses Qt's
private modules, which tie the binary to the Qt build it was compiled against.
See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#the-qt-version-is-pinned-not-preferred).

### Optional hardware support

| Interface | Needs | Without it |
|---|---|---|
| Kvaser | Kvaser drivers + CANlib SDK | Backend shows as *not installed* |
| PEAK-System | PCAN-Basic (`PCANBasic.dll`) + device driver | Backend shows as *not installed* |
| **Virtual bus** | **nothing** | **Always available** |

Vendor SDKs are never bundled — see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#proprietary-sdks).
You can develop, demo and test the entire application with no CAN adapter at
all, thanks to the built-in virtual bus.

---

## Building

From any command prompt:

```powershell
cmake --preset windows-msvc-vs
cmake --build --preset windows-msvc-vs
ctest --preset windows-msvc-vs
```

Or, from an **x64 Native Tools Command Prompt for VS 2022**, the faster Ninja
route:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

The executable lands under `build/<preset>/bin/`.

MSVC is pinned on purpose: Qt for Windows is built with it, so a different
compiler that happens to be first on your `PATH` means an ABI mismatch and a
local build CI cannot reproduce. See
[`docs/development/getting-started.md`](docs/development/getting-started.md).

### Useful options

| Option | Default | Effect |
|---|---|---|
| `TORQUEBUS_BUILD_TESTS` | `ON` | Build the Catch2 suites |
| `TORQUEBUS_WARNINGS_AS_ERRORS` | `OFF` | What CI uses |
| `TORQUEBUS_ENABLE_KVASER` | `ON` | Look for CANlib at configure time |
| `TORQUEBUS_ENABLE_PEAK` | `ON` | Enable the Qt SerialBus PEAK path |

CMake looks for CANlib in the usual install roots, including
`C:/Tools/Kvaser/Canlib` and `C:/Program Files/Kvaser/Canlib`. If yours is
somewhere else, point the build at it with `-DKVASER_CANLIB_DIR=<path>` or the
`KVASER_CANLIB_DIR` environment variable. Without the SDK the Kvaser backend
still builds, as a stub that reports itself unavailable.

### Command line

```
TorqueBusStudio.exe [--reset-layout] [project.tbsproj]
```

`--reset-layout` discards the saved window arrangement — useful after moving a
panel somewhere unreachable.

---

## Repository layout

```
src/core/       vendor-neutral, Qt-free domain model
src/drivers/    ICanBackend and one implementation per vendor
src/services/   settings, projects, workspaces
src/ui/         widgets, theming, docking
tests/          unit / integration / hardware
docs/           architecture and protocol notes
resources/      icons, themes
```

---

## Architecture in one paragraph

Nothing in the UI talks to hardware; nothing in a driver knows the UI exists.
Between them sits a plain, trivially copyable `CanFrame` that belongs to no
vendor, and an `ICanBackend` interface that every adapter implements. Each
channel owns a bounded lock-free queue; one thread drains them all and runs a
**typed dataflow graph** whose nodes are the sources, decoders, filters and
panels. The graph is compiled before a measurement starts and moves *batches*,
never single frames — which is the only way a visual pipeline survives 150k
frames/s. The canvas edits that graph; it does not own it, and a headless run
needs no GUI at all. Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before
your first pull request — it is short, and it is the contract.

TorqueBus takes its workbench from TSMaster and CANoe, and its visual pipeline
from [CANdevStudio](https://github.com/GENIVI/CANdevStudio), which solved that
idea first. No code is shared — CANdevStudio is Qt5/C++17 on QtNodes 2.x, an
incompatible API generation — but the debt is real and worth naming.

---

## Roadmap

| Milestone | Contents | Status |
|---|---|---|
| v0.1 | Foundation: shell, docking, themes, layout persistence, driver API, virtual bus | done |
| v0.2 | CAN Core: engine, channels, threaded RX/TX, queues, filtering, counters, live statistics | done |
| v0.3 | Kvaser CANlib backend | done |
| v0.4 | Pipeline graph: executor, typed ports, compiled topology | done |
| **v0.5** | CAN Trace, as the first real consumer node | **current** |
| v0.6 | QtNodes canvas — the graph becomes visible and editable | next |
| v0.7 | Lua ECU blocks — simulated ECUs you draw instead of configure |  |
| v0.8 | DBC decoder node, `Signals` port |  |
| v0.9 | PEAK-System and SocketCAN backends |  |
| v0.10 | Transmit and Graph nodes |  |
| v0.11 | J1939 and **ISOBUS** (ISO 11783) |  |
| v0.12 | Logger and Playback (`.tblog`, ASC, CSV) |  |
| v0.13 | Projects and Workspaces (`.tbsproj`), graph included |  |
| v0.14 | ISO-TP and UDS, `Events` port |  |
| v0.15 | Dashboard Designer (QML) |  |
| v0.16 | Lua automation: headless runner, test scripting, reports |  |
| v0.17+ | LIN, XCP/CCP, A2L, ARXML, DoIP, FlexRay, more adapters |  |

The full plan, including the definition of 1.0, is in [`PLAN.md`](PLAN.md).

---

## Contributing

Pull requests are welcome. Please read
[`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first — most review feedback on
this project is about layering, not style.

---

## Licence

GNU General Public License v3.0 or later. See [`LICENSE`](LICENSE).

```
TorqueBus Studio
Copyright (C) TorqueBus contributors

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
```

TorqueBus Studio is not affiliated with, endorsed by, or derived from TOSUN,
Vector Informatik, PEAK-System or Kvaser. Product names are the trademarks of
their respective owners and are referenced only to describe interoperability.
