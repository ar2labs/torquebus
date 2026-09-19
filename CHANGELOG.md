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
- **The CI had never run, and three of its parts were broken.** There is no
  git remote, so no workflow in `.github/` has ever executed — and the
  workflows trigger on `main` while the branch here is `master`, so they would
  not have run even with one. The formatting step globbed 27 vendored Lua
  headers, which are someone else's C and can never match our config. The
  "static analysis" step ran `echo` behind `continue-on-error`: a check that
  could not fail because it checked nothing. `tools/check-before-push.ps1` now
  runs the same gates where the code actually is.
- **An exception could leave four thread bodies and two destructors**, and
  every one of those is a `std::terminate` — the process gone with no message
  and nothing flushed to the recording in progress. The worst was the dispatch
  loop, which runs Lua scripts, database decoders and node types registered by
  a plugin: a plugin's node throwing at frame 40,000 took the application with
  it, even though the loader is careful about exceptions during registration.
  Found by the first run of `clang-tidy`, which had never executed before.
- **`CMAKE_AR` was not pinned, and a GNU archiver on `PATH` won.** The presets
  pinned the compiler and the linker against exactly this — an embedded
  toolchain's `ar.exe` taking MSVC flags — and missed the archiver, which fails
  later still: it only runs when a static library needs rebuilding, and almost
  everything here is a static library.
- **The tests trusted `PATH` for their Qt, and a wrong one hung them for ever.**
  Every test binary links Qt, so the loader searches `PATH` — and on the machine
  this was found on, STM32CubeProgrammer's own Qt 6.10.2 sat ahead of the 6.11.2
  the binaries were built against. The failure is `STATUS_ENTRYPOINT_NOT_FOUND`,
  which Windows reports with a *modal dialog*: four test processes waited at zero
  CPU for twelve minutes with an empty log. The tests now carry the Qt they were
  built against, and the unit suite — the only one with no timeout — has one.
- **The pre-push script packaged a Debug build and deployed release Qt onto it**,
  producing a staging tree whose executable could not start (`Qt6Guid.dll was
  not found`). `check-package.ps1` passed it, because it asked for `Qt6Core.dll`
  by name and the release `Qt6Core.dll` was right there. It now reads what the
  executable actually imports and requires each one; packaging happens from the
  release preset, because nobody ships a Debug build.
- **The headroom test demanded zero loss from an unthrottled producer**, which a
  bounded queue cannot give — the architecture counts `softwareOverruns`
  precisely because a producer can outrun a consumer. It passed in Debug and
  failed in RelWithDebInfo, which is the giveaway: it was measuring how the
  optimiser balanced producer against consumer, not the pipeline.
- **CI configured without the presets**, so it built a configuration no preset
  produces (`Release`, where `windows-msvc-release` is `RelWithDebInfo`) and got
  none of the toolchain pins the presets exist to carry.
- **Five entries in the shipped J1939 function table answered the wrong name.**
  AgIsoStack has two On-Highway blocks whose headings both read "Non-specific
  system (Device class 0) industry group 1" — the second adds the word "Tractor"
  in prose and nothing else — so functions 128 to 132 appeared twice with
  different names, and the loader returned whichever came last. The converter
  now refuses a key that two entries claim, and says which two: one of them is
  right, the source does not say which, and a key that answers two things
  answers neither. The table went from 170 entries to 160, and its header now
  gives both reasons an entry can be left out.
- **The Visual Studio preset did not build.** `windows-msvc-vs` is the one the
  README calls "start here" and the one meant for somebody who has just cloned
  and has no developer prompt — and it had never been built. MSBuild compiles a
  target's translation units in parallel and all of them write the same compiler
  PDB, which without `/FS` is `error C1041` and a stopped build. Ninja never hit
  it, because it compiles one file per process; so the preset everybody here
  uses was fine and the one newcomers are pointed at was not.
- **`-misc-include-cleaner` was never actually disabled.** The justification was
  written as `#` lines inside `Checks:`, which is a folded block scalar — inside
  one, `#` is literal text, so the comment joined the comma-separated list and
  took the entry after it along. The exclusion silently did nothing, and the
  evidence (a run still reporting 199 of them) was visible and explained away
  once before it was understood.
- **The package shipped a PEAK plugin whose Qt was not in it.** `windeployqt`
  asks the binary it is given what it needs, and it was given only the
  executable — which does not link Qt6::SerialBus, that being the point of PEAK
  being a plugin. So `Qt6SerialBus.dll` never entered the package and the plugin
  could not load from it. `check-package.ps1` passed it, because its import
  check read the executable's imports and nothing else; it now reads every
  binary in the package, which is what found this.
- **The PEAK plugin could not find Qt, on any machine without Qt on `PATH`.**
  The loader used `LOAD_WITH_ALTERED_SEARCH_PATH`, chosen so a plugin finds a
  vendor SDK sitting beside it — and that flag *replaces* the executable's
  directory rather than adding to it. The shared Qt lives beside the executable,
  one level up from `plugins`, so the plugin that links Qt6::SerialBus came up
  with nothing. The interface list showed Kvaser and no PEAK. It now searches
  both the plugin's directory and the application's, which also drops `PATH`
  from the search — one fewer way for a foreign DLL to be loaded in place of the
  intended one.
- **A Visual Studio build had no Kvaser or PEAK support.** The vendor plugins
  were sent to `${CMAKE_BINARY_DIR}/bin/plugins`, which is beside the executable
  under Ninja and is not under a multi-config generator, where the executable
  lands in `bin/Debug`. The loader looks beside the executable, so the plugins
  were on disk in a directory nothing reads — the same shape as the release that
  shipped without them, found the same way: by actually running the thing.
- **The built application would not start without Qt on `PATH`.** Running
  `build/<preset>/bin/TorqueBusStudio.exe` needed a Qt-aware prompt, which
  everybody here has and a newcomer following the README does not: they get a
  successful build and a Windows dialog. The Qt runtime and the four plugins Qt
  loads by path — platform, both SVG ones, the modern style — are now copied
  beside the executable, so the thing that was just built runs.

- **`databases.md` paired both example databases with the wrong script.** It
  said `vehicle.dbc` and `ecu.dbc` both match what `ecu_vehicle.lua` puts on the
  bus. `vehicle.dbc` does; `ecu.dbc` carries identifiers 1 and 255 and pairs
  with `ecu_motor.lua`, which is a different example. A test now runs each
  script and asks its database about every identifier it actually emitted.

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
- **`src/core/ThreadGuard.h`**, the net under every thread entry point and every
  destructor that flushes, with six tests — including the one that would
  otherwise be embarrassing: a reporter that throws while reporting a crash.
- **`-Tidy` on the pre-push script**, running clang-tidy over the files a change
  touches. Scoped rather than whole-tree on purpose: twenty seconds a file turns
  a two-minute gate into a thirty-minute one, and a gate that long stops being
  run.
- **Tests that run the example scripts.** Five ship in `examples/scripts/`, the
  README and the scripting guide point at them, and they are the first thing
  somebody evaluating the tool opens — and four of the five were executed by
  nothing. They are data, the compiler never sees them, and the Lua API is ours
  and moves; a stale example would surface as an error on the machine of
  somebody trying TorqueBus for the first time. Each is now loaded and run, and
  the suite refuses to let a sixth example arrive uncovered.
- **Tests for the name table that actually ships.** The loader had tests against
  input written for it; the generated file in `data/` that goes into every
  package had none, and that is where the duplicate keys above were hiding. The
  packaging check confirms the file is present and says nothing about what is in
  it.

- **The canvas has a context menu.** It had none at all — the only way to add a
  block was a double click in the palette. Right-clicking empty canvas now
  offers every block type, grouped as the palette groups them, and puts the one
  you pick **where you clicked**. Right-clicking a node names it and offers its
  settings and deletion.
- **"Attach simulated ECU" on a CAN Channel block.** CANoe's Simulation Setup
  lets a node be dropped on a bus and be on it, both directions, with no wire
  drawn; here that was an ECU block, a transmit block and two wires — six
  actions, twenty-four for a rest bus of eight. One menu entry now does it,
  matching the transmit block's channel to the source's and reusing one that is
  already there. Still two edges in the graph afterwards, visible and deletable
  like any others.
- **"Edit script" on a Lua block**, which brings the Script panel forward. The
  editor already followed selection; what it did not do was come to the front,
  and an editor behind another tab is an editor the user does not know they
  have. CANoe puts a pencil on the node for the same reason.

### Changed

- **The README's throughput figure now has a source, and the right one.** It
  claimed 190k frames/s — a number that appears nowhere else in the repository,
  with no measurement cited. The first correction replaced it with ~600,000,
  which was measured in a **Debug** build and presented without saying so;
  nobody ships Debug. An optimised build delivers ~2,100,000 frames/s on an
  i7-11700K, dropping what a bounded queue cannot hold and counting it. The
  asserted requirement stays separate: 150,000 frames/s with zero loss, at a
  throttled rate.
- **One `PLAN.md`.** There were two — the one at the root stopped at v0.6 and
  the README linked to it, which is where the roadmap drift came from. Neither
  was a superset, so section 28 was ported across before the stale copy went.
- **The J1939 protocol constants were checked line by line** against SAE
  J1939-21 (MAY2022), J1939-73 (AUG2022) and ISO 11783-3 (2018). The ETP PGNs
  were wrong from memory, which meant extended transfers were never recognised
  and fell through to ordinary decoding.
- **The tree is formatted, and the formatting check is a gate.** 250 of 267
  files disagreed with `.clang-format`, which made the check unfailable and the
  config a description of nothing. Two of its inherited WebKit settings were
  corrected first, by counting what the code had actually decided: assignment
  stays at the end of a wrapped line (107 places to nil) and a class brace stays
  on the declaration's line (102 to nil). Then one mechanical commit, 210 files.
  Verified inert — 172 files byte-identical with whitespace stripped, the other
  38 differing only by reordered includes, reflowed comments and one block of
  `using` declarations — and `.git-blame-ignore-revs` keeps `git blame` pointing
  at whoever wrote each line.

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
