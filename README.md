# TorqueBus Studio

**Open automotive network and diagnostics workbench**

[![CI](https://github.com/ar2labs/torquebus/actions/workflows/ci.yml/badge.svg)](https://github.com/ar2labs/torquebus/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Qt 6.12](https://img.shields.io/badge/Qt-6.12.0-brightgreen.svg)](https://www.qt.io/)

TorqueBus Studio is an open-source workbench for working with automotive
networks. It brings CAN capture and transmission, database decoding,
diagnostics, simulation, automation, and dashboards into one extensible
application. The project is a community alternative to tools such as
TSMaster, CANalyzer/CANoe, and PCAN-Explorer.

The application can run against its built-in virtual CAN bus without physical
hardware. SAE J2534 (Pass-Thru) and PEAK interfaces are optional plugins loaded from the
`plugins` directory beside the executable.

## What you can do

- Capture, inspect, filter, record, replay, and transmit CAN traffic, including importing and replaying Vector ASCII (.asc) and PEAK-System (.trc) logs.
- Decode DBC databases and use decoded signals in pipelines and dashboards.
- Build visual dataflow pipelines from sources, decoders, filters, simulated
  ECUs, and output blocks.
- Simulate ECUs and automate test sequences with sandboxed Lua scripts.
- Work with ISO-TP, UDS, SAE J1939, and ISOBUS features.
- Create dashboards with signal widgets, trends, alarms, and J1939 fault
  information.
- Extend the application with plugins for hardware backends and pipeline block
  types.

## Requirements

| Component | Requirement |
| --- | --- |
| Platform | Windows 11, x64 |
| Compiler | Visual Studio 2022 / MSVC 2022, x64, C++23 |
| Qt | Qt 6.12.0 for MSVC 2022 x64, including Qt SerialBus |
| Build tools | CMake 3.25 or newer and Ninja, or the Visual Studio CMake generator |

KDDockWidgets, GoogleTest, and Lua are provided by the build. See the
[Getting Started guide](docs/development/getting-started.md) for installation,
configuration, optional hardware SDKs, and troubleshooting.

## Build

Open a Visual Studio developer environment with Qt configured, then run:

```bat
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

The repository also provides `tools\torquebus-prompt.bat` to open a prompt
configured for both MSVC and Qt. The executable is created under
`build\windows-msvc-debug\bin\`. For other presets and first-run instructions,
see [Getting Started](docs/development/getting-started.md). The ordered
[validation checklist](docs/development/validation.md) covers the application
and packaged build.

## Try the examples

- Open [`examples/projects/virtual-vehicle.tbsproj`](examples/projects/virtual-vehicle.tbsproj)
  to run a virtual vehicle with Lua ECUs and a TinyML ECU.
- Open [`examples/projects/j1939-vehicle.tbsproj`](examples/projects/j1939-vehicle.tbsproj)
  to explore a simulated J1939 vehicle, instrument cluster, and fault display.
- Browse sample [Lua scripts](examples/scripts/) and [DBC databases](examples/databases/).

The [Lua ECU scripting guide](docs/development/scripting.md) explains the
script API and lifecycle. The [J1939 simulation guide](docs/development/j1939-pipeline.md)
and [instrument cluster guide](docs/development/cluster.md) describe the truck
example.

## Documentation

Start at the **[TorqueBus Studio Documentation Hub](docs/INDEX.md)** for the
complete guide catalog. Key references:

- [Architecture](docs/ARCHITECTURE.md) — layering, threading, dataflow, drivers,
  persistence, and architectural decisions.
- [Pipeline canvas](docs/development/pipeline-canvas.md) — graph concepts,
  CAN bus wiring, and project representation.
- [Databases](docs/development/databases.md) — DBC parsing, signal decoding,
  bit numbering, and multiplexing.
- [Diagnostics](docs/development/dtc-diagnostics.md) — OBD-II, UDS, and J1939
  trouble-code workflows.
- [Dashboard](docs/development/dashboard.md) — dashboard widgets, signal
  binding, alarms, trends, and fault lists.
- [Testing framework](docs/development/testing-framework.md) — test structure,
  local execution, reports, and CI.
- [Robustness](docs/development/robustness.md) — failure boundaries, limits,
  and robustness coverage.
- [Validation](docs/development/validation.md) — hands-on acceptance steps for
  a build and packaged application.

## Project status

The repository is in active development. The current development milestone is
v0.17, which introduced plugin loading for the Kvaser and PEAK backends. No
public release has been published yet; the project changelog records completed
work and the remaining validation goals. See the [changelog](CHANGELOG.md) and
[engineering plan](docs/PLAN.md) for project history and roadmap context.

## Contributing

Contributions are welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md) and
read the [architecture contract](docs/ARCHITECTURE.md) before changing the
codebase.

## License

TorqueBus Studio is licensed under the GNU General Public License v3.0 or later;
see [LICENSE](LICENSE). TorqueBus Studio is not affiliated with, endorsed by,
or derived from TOSUN, Vector Informatik, PEAK-System, or Kvaser. Product names
are trademarks of their respective owners and are referenced only to describe
interoperability.
