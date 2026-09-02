# Validating a build on Windows

The acceptance pass for a TorqueBus build, ordered so that the cheapest failures
surface first. Each step says what "worked" looks like, so a partial result is
not mistaken for a pass.

Run it after any change that touches the build, the docking, the canvas or the
project file — and in full before tagging a release.

> **Right now this matters more than usual.** Everything from the canvas onwards
> was written without a compiler on hand: the core was compiled and measured,
> the Qt half was not. Expect step 2 to fail the first time. That is the
> expected outcome of an unbuilt branch, not a sign the design is wrong — see
> [What is likely to break first](#what-is-likely-to-break-first).

---

## 0. The prompt

```bat
tools\torquebus-prompt.bat
```

Opens a prompt with **both** halves of the environment: `vcvars64.bat` for MSVC
and `qtenv2.bat` for Qt, in that order. A Qt prompt alone is not enough — it
sets up Qt and nothing else, and the build then fails at link time with errors
that look like a broken Windows SDK.

**Worked:** the banner ends with `MSVC x64 + Qt 6.11.2 ready`, and `where cl`
and `where link` both answer with paths under Visual Studio.

---

## 1. Configure

```bat
rmdir /s /q build
cmake --preset windows-msvc-debug
```

The first configure clones KDDockWidgets 2.2.5, QtNodes 3.0.16 and Catch2 3.7.1,
so it needs network access and takes a few minutes.

**Worked:** the summary block at the end reads

```
TorqueBus Studio 0.5.0
  Build type ............ Debug
  Compiler .............. MSVC 19.x
  Qt .................... 6.11.2
  Docking ............... KDDockWidgets (FetchContent)
  Tests ................. ON
  Scripting ............. Lua 5.5.0 (vendored)
  Kvaser backend ........ ON
```

`Kvaser backend ........ OFF` is fine — it means CANlib was not found, and the
virtual bus covers everything below except step 6.

**If it fails:** [`getting-started.md`](getting-started.md#troubleshooting) covers
the three failures that actually happen — GNU `ld` instead of `link.exe`, a
stale `CMAKE_LINKER` in the cache, and Qt not being found.

---

## 2. Build

```bat
cmake --build --preset windows-msvc-debug
```

**Worked:** `TorqueBusStudio.exe` appears in
`build\windows-msvc-debug\bin\`.

Warnings from `_deps\` are KDDockWidgets' and QtNodes', not ours. Warnings from
`src\` are ours and are worth reading — the project builds clean today, so a new
one is a new mistake.

---

## 3. Tests

```bat
ctest --preset windows-msvc-debug
```

Runs the unit and integration suites; hardware tests are excluded by label.

**Worked:** every test passes, roughly 60 cases. The ones worth knowing by name,
because each pins something that was got wrong once:

| Suite | What it would catch |
|---|---|
| `[project]` | A field added to a node and forgotten in the project file — the round trip compares whole graphs, not fields |
| `[graph][build]` | A wire the canvas would draw that the engine could not build |
| `[lua][ecu]` | A script that fails on every frame taking the measurement down with it |
| `[pipeline]` | Fan-out reaching only the last consumer |
| Qt macro guard | A core header using `emit` or `signals` as an identifier |

A failure here is a real defect: these all pass in CI on Linux, so a Windows-only
failure is a portability bug worth reporting rather than working around.

---

## 4. First run

```bat
build\windows-msvc-debug\bin\TorqueBusStudio.exe --reset-layout
```

`--reset-layout` matters on the first run after this branch: the panel set
changed, and a layout saved by an older build simply would not contain the new
panels.

**Worked:**

- The window opens with **Project Explorer** left, **Properties** and **Block**
  tabbed right, the analysis stack in the middle (**CAN Trace**, **Pipeline**,
  **Transmit**, **Graph**, **Statistics**, **Diagnostics**) and **Output**
  across the bottom.
- Each panel shows its name **once**, in a tab. No title bar above the tab.
- The tab strip is *darker* than the panel body; the active tab is the same
  colour as the panel and carries an accent line on top.
- The gaps between panels are visible grooves, and light up when hovered.
- No pale or white line anywhere along a panel edge.
- The Output panel lists the style sheet size, the bound channels, and
  `Press Start (F5) to go bus-on.`

**Then toggle the theme** (Home → Theme) and check the same six points in the
light theme. The two themes share one style sheet, so a defect in one is usually
a defect in both.

---

## 5. The pipeline, end to end

This is the acceptance test for v0.6 and v0.7 together.

1. **File → Open Project** →
   `examples\projects\virtual-vehicle.tbsproj`.
   The title bar becomes `virtual-vehicle - TorqueBus Studio`, and the Output
   panel says `Pipeline: 5 node(s), 3 connection(s).`
2. **Pipeline** tab. Five blocks, wired: `can_1 → ecu_vehicle → tx_1`, and
   `can_2 → speed_only`. Blocks are draggable; dragging one and reopening the
   project puts it back where it was saved.
3. Click **ecu_vehicle**. The **Block** panel comes forward and shows its
   settings, including the Lua script in a monospaced box and the script
   parameters `speed_id`, `temp_id`, `tick_ms` below them.
4. **Start** (F5). The Output panel shows
   `ecu_vehicle: vehicle ECU ready, speed on 0x101`, and the **CAN Trace** fills
   with `0x101` about ten times a second and `0x102` about once a second.
5. **Stop**. The frame count freezes; the trace keeps what it captured.
6. Change `speed_id` in the Block panel to `0x201`, press **Start** again.
   The trace now shows `0x201`. This is the whole edit-run loop: no rebuild, no
   restart.
7. **File → New Project**. It asks about unsaved changes, because step 6 changed
   something. Answer **Cancel** — nothing should be lost.

**Worked:** all seven. Step 4 is the one that proves the milestone: a script
edited in the window is running on a bus and reaching a panel.

---

## 6. Real hardware (optional)

Only with a Kvaser adapter and CANlib installed.

```bat
ctest --preset windows-msvc-debug -L hardware
```

Then, in the application: **Hardware → Refresh**, pick a Kvaser channel in the
Project Explorer, **Start**, and confirm frames arrive in the trace. The
acceptance shape from PLAN.md section 30 is `Kvaser Virtual 0 → TorqueBus →
Kvaser Virtual 1`, which the standard driver install provides without any
physical adapter.

---

## General validation notes

- **A green suite on one toolchain does not mean undefined behaviour is absent.** MSVC's debug runtime fills freed memory with `0xDD`, which immediately catches dangling pointers that might pass by coincidence on another allocator.
- **Rebuild cleanly after header layout changes.** If a test suite that was not touched crashes after a struct member is added, suspect stale object files first.
- **Run the entire test suite, not only the subset touched.** Integration and example-script tests verify cross-layer contracts that unit tests alone do not.
