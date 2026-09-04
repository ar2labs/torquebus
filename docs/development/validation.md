# Validating a build on Windows

The acceptance pass for a TorqueBus build, ordered so that the cheapest failures
surface first. Each step says what "worked" looks like, so a partial result is
not mistaken for a pass.

Run it after any change that touches the build, the docking, the canvas or the
project file — and in full before tagging a release.

> The first full run of this pass found seven failures, none of them in the
> newly written Qt code — which built clean — and none of them Windows-specific.
> Four were stale tests, one was an off-by-one in a test's own bound, and one
> was a real defect in `removeFrameSink`. All are fixed. The lesson is recorded
> under [What is likely to break first](#what-is-likely-to-break-first).

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

**If every file fails on `C1083: cannot open include file: 'string'`** — the
prompt has no MSVC environment. Step 0 was skipped, or this is a different
window. Configure still passes in that state, because its compiler check is
cached; see [`getting-started.md`](getting-started.md#troubleshooting).

Warnings from `_deps\` are KDDockWidgets' and QtNodes', not ours. Warnings from
`src\` are ours and are worth reading — the project builds clean today, so a new
one is a new mistake.

---

## 3. Tests

Step 2 has to have run. `ctest` on an unbuilt tree reports three failures named
`<target>_NOT_BUILT-<hash>` - that is Catch2's placeholder for "this executable
does not exist yet", not a broken test.

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
- The gaps between panels are visible bars, about 5 px, and turn accent when
  hovered. So does the divider inside the **Pipeline** tab, between the block
  list and the canvas — that one borders the darkest surface in the window and
  is the one that was invisible.
- No pale or white line anywhere along a panel edge.
- **Pipeline** tab: the canvas is the deepest surface in the theme, the 150 px
  grid reads as a faint guide, and the 15 px grid is felt rather than seen. The
  canvas and its grid are unmistakably from the *same* theme.
- The Output panel lists the style sheet size, the bound channels, and
  `Press Start (F5) to go bus-on.`

**Then toggle the theme** (Home → Theme) and check the same points in the light
theme. The two themes share one style sheet, so a defect in one is usually a
defect in both.

Three things specifically survive the toggle, because each was once a colour
copied at construction and never revisited:

- the canvas ground changes with the theme — it does not stay dark grey;
- the Output panel's **existing** scrollback recolours, rather than the lines
  written before the switch staying in the old theme's colours;
- the block list beside the canvas is painted like a panel, not like the canvas.

**View → Inspect Panel Chrome** dumps what Qt actually holds for every widget
under the docking area. Two columns matter beyond the palette:

- `brush=` appears on a `QGraphicsView` and is the colour its scene ground is
  painted with. It is a *separate value* from the palette, and it is what the
  pipeline canvas got wrong for a whole release while showing a perfectly
  healthy palette beside it. Only the graphics view has one — an ordinary
  scroll area's viewport takes `QPalette::Base`, which is already printed.
- `hidden` marks a widget that has never been shown. Its palette is unpolished,
  usually `#000000` at a default 100x30 - not a defect, just meaningless.

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

## 5b. The database, on top of a running measurement

The acceptance test for v0.8. Do this with the example project from step 5
**still running** — the point is that a database explains traffic that is
already there.

1. **File → Import Database…** →
   `examples\databases\vehicle.dbc`.
   The Output panel says `Database: vehicle.dbc - 2 message(s), 2 signal(s).`
   and the **DBC Explorer** comes forward.
2. The tree shows `vehicle.dbc` → `VehicleSpeed` and `EngineTemp`, each opening
   onto its signal. `SpeedKmh` reads `0|16 Intel unsigned`, range
   `0 .. 6553.5  (x0.1)`, unit `km/h`.
3. Type `speed` in the filter box. `EngineTemp` disappears, `VehicleSpeed`
   stays — because its *child* matches. Clear the box; the tree comes back
   without collapsing what you had opened.
4. **CAN Trace** tab. The **Name** column now reads `VehicleSpeed` and
   `EngineTemp` instead of being blank, and the **Signals** column reads
   `SpeedKmh = 42.3 km/h` and `EngTemp = 70 degC`.
5. Rows captured *before* the import are decoded too. Scroll up and check.
6. Import `examples\databases\ecu.dbc` as well. The explorer holds both; the
   trace is unaffected, because that database describes different identifiers.
7. Import something that is not a database — any `.txt` will do. The Output
   panel shows one warning naming a line number, no dialog appears, and the
   explorer still holds what it had.

**Worked:** all seven. Step 4 is the one that proves the milestone, and step 7
is the one that proves the failure path, which is the half that normally goes
untested until a user finds it.

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
