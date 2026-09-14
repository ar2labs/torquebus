# Changelog

Every notable change to TorqueBus Studio, newest first.

**Nothing has been released yet.** There are no tags and no published builds.
What follows are development milestones, dated by the day each version number
landed in `CMakeLists.txt`. They are written down because the work is finished
and verifiable, not because anybody has downloaded it — and saying so is the
point. The first release will be v1.0, and what stands between here and it is
not a feature: it is validation against real hardware
(`docs/development/validation.md`).

Dates are ISO-8601. Versions follow [semantic versioning](https://semver.org)
from 1.0 onwards; below 1.0 the minor number is the milestone, as
`docs/PLAN.md` numbers them.

---

## Unreleased

Since 0.17.0. Mostly a pass of checking what the project claims about itself
against what it does, which turned up more than expected.

### Fixed

- **The package did not contain the vendor plugins.** v0.17 moved the Kvaser
  and PEAK backends out of the executable and the install rules were never
  told, so for three milestones a release would have carried no hardware
  support at all — with an empty interface list as the only symptom. The build
  was green throughout.
- **The package contained 148 files of somebody else's SDK.** KDDockWidgets and
  QtNodes arrive through `FetchContent`, so their install rules ran as part of
  ours, writing their headers and CMake config into an application meant for
  end users. Everything TorqueBus installs is now in a named component that the
  packaging step asks for by name.
- **The virtual bus timestamped its first frame as zero.** The clock documented
  itself as "nanoseconds since the process started" and measured from a
  function-local static initialised on the first call. On a clock whose tick is
  coarser than that gap the answer was exactly 0, which reads as a timestamp
  never applied. It failed an integration test intermittently, depending on
  which frame happened to be first in a test process.
- **The J1939 ETP abort table was the wrong one.** Extended transport aborts
  are ISO 11783-3 Table 9, not SAE J1939-21; they differ at value 9 and the ISO
  table adds 10 to 15.
- **T1 was applied before the first packet of a negotiated transfer.**
  J1939-21 §5.10.2(a) counts T1 from the receipt of the last packet, so a legal
  transfer could time out before it began.
- **`signalsIn` allocated once per frame**, against the pipeline's own rule that
  `process()` must not allocate. Now fills a caller-owned buffer: +14% decode
  throughput, with a test that would notice if it came back.

### Added

- **The J1939 function-name table ships.** Derived from AgIsoStack++ under the
  MIT licence, so it is ours to pass on with its notice attached. The network
  panel reads `Engine (0)` out of the box instead of `0`. Manufacturer names
  still do not ship — that registry is licensed — and `tools/j1939-names.py`
  builds that half on the machine of whoever wants it, from a Digital Annex or
  from the public ISO 11783 registry.
- **`tools/check-package.ps1`**, which inspects a packaged build before it
  becomes a zip. Its refusals matter as much as its requirements: no licensed
  name table, no third-party headers, no vendor SDK DLLs. Both workflows run it.
- **Layering checks at configure time.** Rules 1, 2 and 3 of `ARCHITECTURE.md`
  were enforced by review, which does not catch a `target_link_libraries` line
  that compiles. `cmake/TorqueBusLayering.cmake` walks each library's link
  closure transitively and fails the configure naming the chain.
- **A throughput headroom measurement**, separate from the throughput
  requirement, because "does it meet the requirement" and "by how much" are
  different questions.

### Changed

- **The README's throughput figure now has a source.** It claimed 190k
  frames/s; that number appears nowhere else in the repository and no
  measurement was cited. The requirement asserted on every pull request is
  150,000 frames/s with zero loss; unthrottled, the same rig measures ~600,000
  frames/s on an i7-11700K, printed by the test itself.
- **One `PLAN.md`.** There were two — the one at the root stopped at v0.6 and
  the README linked to it, which is where the roadmap drift came from. Neither
  was a superset, so section 28 was ported across before the stale copy went.
- **The J1939 protocol constants were checked line by line** against SAE
  J1939-21 (MAY2022), J1939-73 (AUG2022) and ISO 11783-3 (2018). The ETP PGNs
  were wrong from memory, which meant extended transfers were never recognised
  and fell through to ordinary decoding.

### Removed

- 18 `.gitkeep` files, `PlaceholderPanel` (nothing constructed it any more), and
  a static palette preview showing an accent colour the product no longer has.

---

## 0.17.0 — Plugins — 2026-09-12

The vendor SDKs leave the binary. Kvaser and PEAK become plugins, loaded from a
`plugins` directory beside the executable rather than linked in, so a GPL
distribution carries no proprietary SDK in its own image and a machine without
the driver is a machine with one fewer file. Verified rather than asserted:
`dumpbin /DEPENDENTS` finds neither `canlib32` nor Qt SerialBus in the
application.

What crosses the boundary is C++ — `std::function`, `std::span`, `Result` — so
the loader refuses any plugin whose build key does not match byte for byte, and
says so with the file name. Only the entry point is `extern "C"`. A plugin that
fails to load never fails silently: absence is the ordinary symptom of a plugin
problem, and absence diagnoses nothing.

The virtual backend stays built in, so an application with zero plugins still
opens, builds a graph and runs a measurement.

## 0.16.0 — J1939 and ISOBUS — 2026-09-11

29-bit identifiers stop being numbers a database translates and become
structure: who sent it, to whom, which message. Address claiming with NAME
arbitration, transport (BAM, RTS/CTS and ETP), DM1 and DM2 diagnostics, and a
Network panel that shows what is on the bus without joining it.

The signal decoder was not rewritten — an SPN is a signal, and `CanSignal`
already does start bit, length, byte order, factor and offset.

## 0.15.0 — Bus simulation — 2026-09-11

The rest bus: the traffic an ECU under test expects to hear from everything
that is not on the bench.

## 0.14.0 — Dashboard Designer — 2026-09-11

Gauge, Numeric, Lamp, Button, Switch, Slider, Knob and Label, bound to CAN
signals for reading and to system variables for both. Graph was left out on
purpose: the Graph panel already does it better.

## 0.13.0 — Lua — 2026-09-11

Simulated ECUs that answer UDS and are allowed to decline; many timers, cyclic
messages and signal generators; hot reload; and test sequences with a verdict.
Python left the plan here — two embedded languages would be two APIs to
maintain, two sandboxes to audit and two halves of the documentation always out
of date.

## 0.12.0 — UDS — 2026-09-07

A UDS client, the vocabulary that makes its answers readable, the Diagnostic
Console, and a service editor so a request is a form rather than a memory test.

## 0.11.0 — ISO-TP — 2026-09-06

The transport under every diagnostic that does not fit in one frame, on the
canvas, with the `Events` payload it needed.

## 0.10.0 — Projects and workspaces — 2026-09-06

Hardware configuration — which interfaces are CAN 1..N, and how — and
workspaces saved with the project.

## 0.9.0 — Logger, Playback and the Graph panel — 2026-09-06

`.tblog`, a measurement written down; recording that outlives the panel that
started it; and replay through the same graph the live bus feeds, driven —
pause, speed, seek and a position. Export to ASC and CSV, because a log only
this tool can read is stuck here. The Graph panel landed in the same run.

This is also where the version number started telling the truth: it had said
0.5.0 since the v0.5 commit, through four milestones, while reaching the Output
panel, the About box and the installer filename. The convention written down
next to it is that the version names the last milestone that is *finished*, and
it is bumped by the commit that finishes one.

---

## Before the version number tracked — 2026-09-01 to 2026-09-05

`CMakeLists.txt` carried 0.5.0 from the first build-system commit until 0.9.0,
so these milestones have no version bump to date them. In the order they were
built:

- **Foundation** — the docking workbench, themes, panels, the driver API and
  the virtual bus.
- **CAN core** — engine, channels, bounded lock-free queues, filtering and
  statistics.
- **Kvaser CANlib backend.**
- **The pipeline graph as the data path** — not a view of the pipeline; the
  pipeline.
- **CAN Trace** — a one-million-row store and its panel.
- **Lua 5.5.0, vendored and built with the project** — one sandboxed
  interpreter per ECU, errors as `Result`, and two `cansim` ECUs ported.
- **The canvas** — QtNodes over the graph description, editable, saved as
  `.tbsproj`.
- **DBC** — bit extraction for Motorola and Intel, the parser, a decoder node,
  signal encoding, and the DBC Explorer.
- **Transmit** — entries, scheduling, a source node, and a panel that edits a
  row by signal name.
- **Statistics** — what the buses and the pipeline have each counted, including
  the dropped frames nobody wants to be the one to hide.

The Lua engine and the canvas were deliberately built out of milestone order:
the engine because it could be measured, and that meant the canvas would edit a
graph that already ran rather than being the only way to find out whether it
ran.
