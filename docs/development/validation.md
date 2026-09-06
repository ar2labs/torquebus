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
>
> The v0.8 run — sixteen commits, including every hand-painted widget and the
> whole database layer, none of it ever compiled — was green on the first
> attempt: **222 tests, 0 failed**. Worth stating plainly, because the two
> practices that got it there are cheap and easy to skip: checking each Qt call
> against the type stubs for the pinned Qt version instead of recalling it, and
> running the core under `-fsanitize=address,undefined`.
>
> **A green ctest is not a green window.** None of the painting or animation
> has test coverage; it compiles and the suite passes, which says nothing about
> what the window looks like. Step 4 below is the part that does.
>
> A third practice joined the two above while the accent colour was being
> written: **running the real Qt value types before writing C++ against them.**
> The PySide6 wheel for the pinned version carries the actual Qt libraries, so
> `QColor::fromHslF`, `toHsl` and `hslSaturationF` can be exercised in a
> throwaway script and the derivation checked across all sixteen hue-and-theme
> combinations *before* any of it exists as code. That is a measurement of Qt's
> behaviour rather than a memory of it, and it is what the
> [`backgroundBrush` mistake](#what-is-likely-to-break-first) cost an afternoon
> for want of.
>
> And a fourth, from the plot store: **ThreadSanitizer, when the claim is about
> a lock rather than about memory.** AddressSanitizer finds a use-after-free and
> says nothing at all about a data race; the store's mutex exists to make a read
> of a wrapping ring safe, and only TSan can say whether it does. Where TSan
> refuses to start with "unexpected memory mapping", run the binary under
> `setarch $(uname -m) -R` to turn ASLR off.

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
| `[accent]` | A chosen accent that is invisible on its panel, unreadable under its own label, or that changed the house colour for everybody |
| `[contrast]` | The WCAG maths itself — black against white must be 21:1, and green must outrank red must outrank blue |
| `[settings][bitrate]` | A hand-edited rate no backend has segment timing for being obeyed instead of replaced |
| `[graph][validate]` | A block that cannot run being reported at Start instead of when it was dropped — and, the other way, a half-configured canvas being refused |
| `[log]` | A recording that was cut short reading as corrupt instead of as forty-nine whole frames and a note |
| `[plot]` | A plot quietly lying about the bus: a wrapped ring read out of order, a truncated signal drawn as zero, an axis that shrinks as history ages out |
| `[statistics][bitrate]` | The default bitrate drifting apart between the three places that spell it |
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
- The gaps between panels are visible bars, about 5 px, in **both**
  orientations — the vertical one beside the Project Explorer and the
  horizontal one above the Output panel. Hovering one fades it to the accent
  over about an eighth of a second rather than snapping.
- The divider inside the **Pipeline** tab, between the block list and the
  canvas, is visible too. That is a `QSplitter`, not a dock separator, and it
  borders the darkest surface in the window — the case that hid the fault
  twice.
- Clicking between tabs slides the accent marker from one to the other instead
  of moving it in one step, and leaves a brief ripple from the point of the
  click. Dragging a tab to reorder it snaps the marker rather than animating:
  nothing moved from anywhere, the strip was relaid out.
- Toolbar buttons fade in their hover fill and ripple from the point of a
  click, with the icon and label at full strength on top - never tinted.
  **Press Start**: the moment it disables itself the wash goes with it, rather
  than staying lit under a cursor that is no longer over anything clickable.
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
8. Reopen the example project, drop a **DBC Decoder** block on the canvas and
   set its **Database** to `..\databases\vehicle.dbc` — a path relative to the
   project file, not to wherever the executable was launched from. Press
   **Start**. The Output panel reports the pipeline building, with no file
   error.

   Then launch the application from a different directory entirely and repeat.
   It has to behave the same: that is the whole point, and until this release
   the project only opened from the repository root.

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

## 5c. Transmitting by hand

The acceptance test for the Transmit panel. Run it with the virtual bus
started, and watch the **CAN Trace** while you do.

**Nothing has to be wired first.** A list with rows in it reaches the bus on
its own, the same way every channel reaches the trace without a visit to the
canvas. If this needed a block dragged onto the Pipeline canvas before Send did
anything, the panel would look broken to everyone who tried it.

1. **Transmit** tab → **Add**. A row appears: `New frame`, **Ch** `1`, ID `100`,
   eight zero bytes, **On** ticked and **Cyc** *not* ticked.
2. **Nothing is on the bus.** A new row is manual, and manual means it goes out
   when you say so. This is the point of the panel most worth checking: a list
   is filled in with the cable already connected to something real.
3. Type `52 03 00` into **Data**. The **DLC** column follows to 3 on its own -
   it is derived, not typed.
4. Press **Send**. Exactly one `0x100` appears in the trace. Press it three
   more times: three more frames, one per press.
5. Tick **Cyc** and set **ms** to `200`. Frames now arrive five times a second,
   and the **Count** column climbs. Untick it and they stop.
6. Type `zz` into **ID**. The cell snaps back to what it was and the Output
   panel says the row could not be read. Nothing about a row that failed to
   parse is left looking committed - believing you are transmitting on an
   identifier you are not is the failure this prevents.
7. Untick **On** and press **Send**. Nothing goes out, and the Output panel
   says why rather than leaving a button that silently does nothing.
8. Set **Ch** to `2` and press **Send**. The frame appears on CAN 2 and not on
   CAN 1 - one list serves every bus, and a row goes out once, where it was
   addressed. Setting it to `9` on a four-channel setup is refused rather than
   making a row that can never send and never says why.
9. With `vehicle.dbc` imported (step 5b), **Add from message...** → pick
   `VehicleSpeed`. The row arrives named, with the right identifier and length,
   and with the database's own cycle time already filled in - still manual,
   because a period is a suggestion and transmitting is a decision.

10. With that `VehicleSpeed` row selected, press **Signals...**. A dialog lists
   `SpeedKmh` with its unit and range, and the payload in hex at the foot.
11. Type `85` into the value. The payload becomes `52 03 …` while you watch -
    the encoding happening in front of you rather than being taken on trust.
12. Type `85.03`. The cell settles on `85`, because a signal scaled by a tenth
    cannot carry hundredths. That is the bus being honest: a field that showed
    `85.03` while transmitting `85` would be lying about the one thing you came
    here to set.
13. Type `99999`. It saturates at the widest the field holds rather than
    wrapping, and the payload shows it.
14. On a multiplexed message, change the switch signal. The rows belonging to
    other pages grey out and read *(not on this page)* — on that frame they are
    not zero, they are absent, which is the same distinction the decoder makes.

15. **File → Save Project As...**, close the application, reopen it and open
    that project. Every row comes back - channel, identifier, data, period and
    the switched-off ones still switched off. The **Count** column reads zero:
    a project records what to send, not what a previous run sent.
16. Open `examples\projects\virtual-vehicle.tbsproj`, which was written before
    the transmit list existed. It opens, with an empty list. A build that
    refused older files would make the format's version check pointless - it is
    there to stop a *newer* file being opened by an older reader, not the
    reverse.

**Worked:** all sixteen. Steps 2 and 7 are the ones worth being fussy about;
they are the difference between a tool that does what it is told and one that
does things nobody asked for.

---

## 5d. Statistics: the bus and the pipeline

The panel that makes a whole class of silent mistake visible. Open the
**Statistics** tab (it shares a group with Trace).

1. Before pressing Start, with interfaces detected: the top table already lists
   the bound channels, state **Ready**, every counter zero. It is filled in when
   the channels are bound, not when the measurement starts - a table that stayed
   blank until Start would look broken rather than idle.
2. Press **Start**. Rx climbs, **Frames/s** settles, and the **Load** cell grows
   a bar behind its figure. The bar is green; drive the bitrate down (or the
   traffic up) far enough and it turns amber at 80% - the same threshold, for
   the same reason, that the status bar already colours the load at.
3. Switch to the **Trace** tab and back. The numbers are current, not stale and
   not blank: the panel keeps the last figures while hidden and writes them in
   on the way back, rather than paying to update a table nobody is looking at.
4. The lower tree lists every block of the running pipeline, each with the
   counters it chose to report. A **DBC Decoder** shows *Frames decoded* and
   *Frames not in the database*; a **Lua ECU** shows what it emitted and how many
   frames it dropped when saturated.
5. **The step this panel exists for.** Import a database that does not describe
   the traffic on the bus - any `.dbc` for a different vehicle. *Frames not in
   the database* climbs to match the whole of the traffic while *Frames decoded*
   stays at zero. Before this panel that mistake looked exactly like a quiet bus,
   and the Signals column simply stayed empty.
6. Expand a block, then let several seconds pass. The branch stays open: the
   values are written into the existing rows and the tree is rebuilt only when
   the shape of the pipeline actually changes.
7. Press **Stop**. The final counts stay on screen - the ticks stop, the numbers
   do not disappear.

**Worked:** all seven. Step 5 is the one to be fussy about; the rest is
plumbing, and that one is the reason for the plumbing.

---

## 5e. Preferences

**Tools → Preferences**, or `Ctrl+,`. Five pages; three of them do something,
and the other two say so.

**Theme and accent.** These apply while you watch, not on OK.

1. On the **Appearance** page, click the **Light** card. The whole window
   changes as the card is clicked - and **Follow the system theme** clears
   itself, because picking a theme is an explicit choice and the checkbox must
   not go on claiming otherwise.
2. Click a **purple** swatch. The window's focus rings, active tab marker and
   selection turn purple, and *both* theme cards repaint - the point of the two
   cards side by side is that one hue is two colours.
3. Look at the purple on the Light card and on the Dark card. They are not the
   same colour, and they should not be: a swatch stores a hue, and the tone is
   worked out for whichever theme it lands on. A single stored RGB would put
   this exact purple on a white panel.
4. Try **yellow** on the Light theme. It comes out as a dark ochre rather than
   the yellow of the swatch, because the derivation walked it down until it
   cleared the contrast floor against a white panel. That is the loop in
   `AccentColor.cpp` earning its place - and `AccentColorTests` asserts it for
   all sixteen combinations.
5. Press **Cancel**. Theme, accent and density all go back to what they were
   when the dialog opened. This is what makes applying live safe rather than a
   trap, so it is the step worth being fussy about.
6. Reopen, change the accent, press **OK**, close TorqueBus and start it again.
   The accent is still there.

**Following the desktop.**

7. Tick **Follow the system theme**. The window takes the desktop's setting
   immediately - not at the next time Windows changes its mind. Neither card is
   marked as chosen, because nothing has been chosen.
8. With the dialog closed and the box still ticked, change Windows between Light
   and Dark (Settings → Personalisation → Colours). TorqueBus follows within a
   second, with no restart.
9. Tick it, then pick a card. The box clears and stays clear through a restart:
   an explicit choice keeps winning.

**Density.**

10. Set **Compact** and look at the Trace. The rows tighten and more frames fit
    on screen. **Spacious** goes the other way. **Comfortable** is exactly what
    the application looked like before this setting existed, which is why it is
    the default - the default is not a new opinion about how TorqueBus looks.

**Trace and Transmit.**

11. Set the refresh to 500 ms with a measurement running. New rows arrive in
    visible steps. The frame counter in the status bar keeps climbing smoothly:
    the store is filled by the pipeline whatever this says, so a slow refresh
    loses liveness and nothing else.
12. Tick **Show identifiers in decimal**. The column changes under you without
    the view scrolling or the selection moving - the rows did not change, only
    how one column is spelled.

**Reset, and the pages that admit they are empty.**

13. **Reset preferences** puts theme, accent, density and the Trace settings
    back. The panel layout is *not* touched; that is **View → Reset Window
    Layout**, and the button's tooltip says so.
14. **Shortcuts** and **Plugins** are pages with a description and "Planned for
    v0.10". No controls. A checkbox that did nothing would be worse.

**What is remembered.**

15. Drag the divider between the block palette and the canvas in **Pipeline**,
    and the one between the two tables in **Statistics**. Resize the window and
    move a dock. Close and reopen: all four are as you left them. The window
    geometry and the docks were already saved; the two dividers *inside* panels
    are invisible to the dock layout saver and are stored by name.
16. Turn **Restore the panel layout from the last session** off, rearrange the
    docks, restart. The docks come back to the default - and the two dividers
    still come back where you put them, because a divider inside a panel is the
    panel's business, not the layout's.

**Worked:** all sixteen. Steps 5 and 16 are the ones to be fussy about. Step 5
is the promise that makes applying live acceptable at all, and step 16 is the
distinction that most invites being got wrong.

---

## 5f. The bitrate of each interface

The default is 250 kbit/s - the J1939 rate, and so the rate of the buses this
tool was started to read. A passenger car is usually 500k, so most sessions
change this once and never again.

1. **Tools -> Preferences -> Hardware.** One row per interface the Hardware menu
   last found, numbered the way the rest of the window numbers them: row 1 is
   CAN 1, the same channel the status bar, the Trace and the transmit list mean.
2. Set CAN 1 to 500 kbit/s with the measurement stopped. The Output panel says
   the channel was rebound and at what rate - the change is applied, not merely
   recorded.
3. Press **Start**. Frames arrive. Set the rate to 125 kbit/s *while running*:
   the Output panel says it was saved but the channels are on the bus, and
   nothing changes underneath the measurement. **Stop**, then **Hardware ->
   Refresh Interfaces**, and it takes effect.
4. Put a channel on a rate the bus is not using and press Start. The Trace stays
   empty, the error counter in the Statistics panel climbs and the state goes to
   warning and then bus-off. That is the mistake being loud rather than subtle,
   and it is the reason a default is a convenience here rather than a hazard.
5. Close and reopen TorqueBus. Each interface still has its own rate.
6. Unplug an adapter, plug it back in so it enumerates second, and reopen the
   page. It kept its rate: the setting is keyed by the device handle, not by the
   channel slot it happened to occupy last time.
7. Hand-edit `settings.json` and put `33333` under one of the `can/bitrate/`
   keys. TorqueBus opens that interface at 250k instead. A CAN controller is
   configured with segment timing rather than a frequency, so a rate nothing has
   timing for would open a channel that produces error frames instead of failing
   outright - `[settings][bitrate]` asserts this, and step 4 is what it looks
   like when it is not caught.
8. **Reset preferences** puts every interface back to 250k and removes the keys,
   which is the state a fresh installation is in - not one that has explicitly
   chosen the default everywhere.

**Worked:** all eight. Steps 3 and 7 are the ones to be fussy about; both are
places where the honest answer is to refuse and say so.

---

## 5g. The Graph panel

The other half of v0.8, and the first thing that makes a decoded signal worth
decoding.

1. Open a `.dbc` (**File -> Import Database**), then on the **Pipeline** canvas
   wire **CAN Channel -> DBC Decoder -> Signal Plot**. Give the decoder its
   database with the **...** button in the Block panel. Press **Start**.
2. The **Graph** tab's list fills with `Message.Signal` names as the bus
   introduces them - qualified by message, because two databases can use one
   name for two different things.
3. Tick two signals with very different ranges: an engine speed in the thousands
   and a temperature in the tens. **Both are readable.** Each gets its own
   vertical scale, labelled at the left edge in that trace's colour. On one
   shared axis this is a line and a flat line, which is why every tool in this
   family does it this way.
4. Hover the plot. A dashed cursor follows, and the status line reads the value
   each ticked signal held *at that instant* - the last sample at or before the
   cursor, not an interpolation. A signal that changes in steps must not be
   reported at a value it never carried.
5. Change the window to **1 s** and back to **10 s**. The time labels gain and
   lose decimals: at one second, tenths would print the same number six times.
6. **Freeze.** The line stops advancing and the Output panel keeps counting
   frames - the pipeline is still recording. Unfreeze and it catches up rather
   than resuming from where it stopped.
7. Leave it running past the capacity of a series (8192 samples - about 80
   seconds of a 100 Hz signal). The status line starts reporting samples that
   have aged out. A plot that silently began part-way through a measurement is
   one somebody draws a wrong conclusion from.
8. Switch theme while it is running. The lines change colour: they are the
   accent palette, derived per theme and contrast-checked, so a plot line is
   legible on both themes for the same reason a focus ring is.
9. Drag the divider between the list and the plot, restart, and it is where you
   left it.

**Worked:** all nine. Steps 3 and 4 are the ones to be fussy about - both are
places where the convenient answer produces a plot that is wrong rather than
merely ugly.

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
