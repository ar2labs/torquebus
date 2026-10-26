# TorqueBus Studio Documentation

Use this page to find the architecture reference, setup instructions, and
development guides for TorqueBus Studio. The root [README](../README.md) is the
project overview and quick start; this page is the complete catalog of
documentation maintained in `docs/`.

## Start here

1. [Getting Started](development/getting-started.md) — install the toolchain,
   configure and build the application, and troubleshoot common issues.
2. [Architecture](ARCHITECTURE.md) — learn the system boundaries and design
   rules before making changes.
3. [Validation](development/validation.md) — verify the build, application,
   hardware workflows, and packaged output.

## Architecture and project direction

- [Architecture](ARCHITECTURE.md) — core rules, layers, frame processing,
  pipeline graphs, drivers, UI, persistence, and design decisions.
- [Engineering plan](PLAN.md) — historical project plan and roadmap notes.
  This document is currently written in Portuguese.

## Setup and validation

- [Getting Started](development/getting-started.md) — Windows prerequisites,
  build presets, first run, optional hardware setup, and troubleshooting.
- [Validation](development/validation.md) — an end-to-end manual acceptance
  checklist, including plugins and packaging.

## Pipeline, data, and simulation

- [Pipeline canvas](development/pipeline-canvas.md) — canvas concepts, CAN bus
  wiring, frame routing, rendering, and project persistence.
- [Databases](development/databases.md) — DBC parsing, bit numbering,
  multiplexing, decoding, and encoding.
- [Lua ECU scripting](development/scripting.md) — script lifecycle, available
  APIs, sandbox limits, timing, diagnostics, and examples.
- [Pipeline test sequences](development/testing.md) — Lua-based test sequences
  for sending frames and checking bus behavior.
- [TinyML Virtual ECU](development/tinyml.md) — model inputs and outputs,
  custom model files, and visual simulation.

## Protocols, diagnostics, and dashboards

- [SAE J1939 pipeline simulation](development/j1939-pipeline.md) — supported
  J1939 behavior, simulated vehicle ECUs, network dataflow, and diagnostics.
- [Instrument cluster](development/cluster.md) — cluster profiles, gauges,
  indicators, the J1939 example, and dashboard integration.
- [Dashboard](development/dashboard.md) — signal widgets, images, alarms,
  trends, tables, fault lists, and project storage.
- [DTC diagnostics](development/dtc-diagnostics.md) — OBD-II, UDS, and J1939
  diagnostic concepts and workflows.

## Testing and reliability

- [Testing framework](development/testing-framework.md) — Google Test and CTest
  structure, labels, local runs, reports, and GitHub Actions.
- [Robustness](development/robustness.md) — error containment, known limits,
  fuzzing, and failure-path coverage.

## Related project documents

These documents live at the repository root and complement the guides above:

- [Contributing](../CONTRIBUTING.md) — contribution workflow, build gates, and
  review expectations.
- [Code of Conduct](../CODE_OF_CONDUCT.md) — community participation
  expectations.
- [Changelog](../CHANGELOG.md) — development milestones and recorded changes.
- [License](../LICENSE) — GNU General Public License version 3 or later.
