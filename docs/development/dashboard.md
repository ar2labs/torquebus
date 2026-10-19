# The Dashboard

The Dashboard is the half of the application people look at: a page of widgets bound to the bus - gauges
that move, lamps that light, a slider whose value a simulated ECU is reading. It has two modes and the
mode is the whole design:

* **Run** is what a bench looks like. Widgets show what is on the bus; a slider under the hand writes a
  variable. Nothing can be moved by accident.
* **Edit** (*Simulation → Edit Dashboard*) is what a designer looks like. Widgets are dragged, resized by
  their corner, added, copied and deleted; controls do not respond, so that dragging a slider into place
  does not send the values it sweeps through.

Everything is painted by hand except the **instrument cluster**, which is QML and has a page of its own
(`cluster.md`). This page is about the rest: what the widgets are, and how a Dashboard is built from the
databases that are loaded rather than typed from memory.

---

## What it is made of

A widget is a rectangle, a kind and a **binding**. The binding is the point: a gauge bound to nothing is a
picture of a gauge. There are three things a widget can read, and one it can write:

| Binding | Reads | Writes |
|---|---|---|
| **CAN signal** (`Message.Signal`) | the newest decoded value in the plot store - what the Plot Viewer shows | never: a widget that wrote a signal would have to own a message, a cycle time and a channel, which is a transmit entry |
| **Variable** | a system variable | controls (slider, knob, switch, button) write it; a script or a transmit entry decides what that means on the wire |
| **Message** | every signal of one message | - (the Table only) |

The kinds. The last five columns say which settings a kind has - the form shows only those, from one table
(`DashboardKindTraits`) that `validate()` reads too, so "a lamp has no range" is a fact in one place. A Lamp
and a Switch or Button also have a *threshold*, a Trend a *window*, and every kind that reads a signal has a
*stale time-out*.

| Kind | Shows | Range | Unit | Limits | Names | Picture |
|---|---|:-:|:-:|:-:|:-:|:-:|
| **Gauge** | a dial with a needle and the number under it | ✓ | ✓ | ✓ | | |
| **Numeric** | the number, as large as the box lets it be | ✓ | ✓ | ✓ | ✓ | |
| **Bar** | how full: a bar filling along its longer side | ✓ | ✓ | ✓ | | |
| **Lamp** | on or off, above a threshold; the bulb can be a picture | | | | ✓ | ✓ |
| **State** | the *name* the value has - "Reverse", "ThermalStress" - on a chip tinted by what it means | | | | ✓ | |
| **Image** | a picture; bound to a value, the picture of the state that value is in | | | | ✓ | ✓ |
| **Trend** | the last seconds of one signal as a line | ✓ | ✓ | ✓ | | |
| **Table** | every signal of one message, with its value | | | | | |
| **Slider**, **Knob** | a continuous value a person sets | ✓ | ✓ | | | |
| **Switch**, **Button** | one of two values, held or momentary | ✓ | | | | |
| **Label** | fixed text | | | | | |
| **Group** | a titled frame behind other widgets | | | | | |
| **Faults** | the J1939 trouble codes of the whole bus: the four DM1 lamps and a row for each active code (see *The fault list*) | | | | | |
| **Cluster** | the QML instrument cluster, fed by a profile | | | | | |

A **Faults** widget is bound to nothing: it reads the J1939 block, and has a title and nothing else to set.

Any widget that reads one value can be **shown as** any other that does - *Show as* in the form - and the
signal, unit, range and names stay: a level as a dial, a dial as a trend. If the signal is in a database
and the new kind uses something the old one did not (a bar wants the range, a state wants the names), it
brings it.

---

## Building one from the databases

A widget can be configured by hand - two names typed exactly as the database spells them, then a unit, a
range, a number of decimals and, for a gear selector or a risk level, the words for the values, every one
of which the database already has. Given the loaded `.dbc` files the Dashboard stops being a place to type
names and becomes a place to choose them. None of it is required: with no database loaded everything works
as it always did, and what is typed is kept as typed.

```
 DBC Explorer ─── drag ───────────────┐
                                      ▼
 Add from Database... ──►  SignalDictionary  ──►  suggestWidget  ──►  DashboardDescription
 (search, multi-select)   every signal of every      kind, binding,        (what is saved)
                          loaded database            unit, range,
 widget form ── completion, Browse...,               names, time-out
 Fill from database        audit
```

### The dictionary

`SignalDictionary` (`src/core/dashboard/`, no Qt) turns the loaded databases into one flat list of
**entries**: every signal with its unit, its physical range, how often its message is sent, the transmitter,
the comments and its value table. It is *copied* out of the databases, so it does not depend on them staying
alive, and rebuilt whenever a database is loaded or the Explorer is cleared (New and Open do).

Search treats the words typed as a conjunction - every one has to match - and ranks by where it matches: the
signal's name, then what it starts with, then the message, a word inside the name, the **initials** (`ect`
finds `EngineCoolantTemperature`), anywhere in the name, the unit, and last the comment. Ties keep the
databases' order.

### Which widget a signal wants

Rules, not guesses: each is written down in `suggestKind` and pinned by a test, so "why did it make a
numeric of that" has an answer in one place.

1. A value table that names exactly `0` and `1` is a **Lamp**; any other value table is a **State**.
2. A signal of one or two bits is a **Lamp**.
3. A percentage is a **Bar**.
4. A unit that counts time, distance or turns is a **Numeric** - a dial of an odometer is a dial that never
   moves.
5. A signal whose range the database declares, with a span under a hundred thousand and a unit, is a
   **Gauge**.
6. Everything else is a **Numeric**, which is correct for anything.

### What a widget made from a signal gets

`applyEntry` fills in everything the database knows that the kind uses:

* the **title**, from the name in words (`EngineCoolantTemperature` → "Engine Coolant Temperature"). A title
  somebody typed is theirs and is never replaced; one the previous binding produced is.
* the **unit**, and the **range** - the one the database declares, or what the bits can hold when it
  declares none (a `.dbc` that says `[0|0]` is not saying the signal is always zero).
* the **decimals**: enough to tell one step of the signal from the next, at most two. A step of 0.125 shows
  one decimal, 0.05 two, and a step of 1 none.
* the **names of the values**, from its value table. The table holds *raw* values and the widget reads
  *physical* ones, so each is converted (`raw × factor + offset`) with the decoder's own arithmetic. Each
  name is tinted by what it says: *Error*, *Fault*, *Critical* are danger; *Warning*, *High* attention;
  *Running*, *Engaged* good; *Off*, *Idle*, *NotAvailable* quiet. A starting point to edit, not a judgement.
* a **stale time-out**, when the database says how often the message is sent: three of its periods, never
  under half a second and never over a minute (see *A signal that goes quiet*).
* an **id** from the signal's name (`EngineSpeed` → `engine_speed`, made unique).

Size, picture and limits are left alone.

### Four ways in

**Drag.** Signals and messages in the **DBC Explorer** are drag sources. By default the Explorer and the
Dashboard are tabs of one group, so to drag between them put them side by side first (drag the Explorer's tab
to an edge of the window, or float it). Dropped on the Dashboard in Edit mode, each signal becomes the widget
it suits, already bound, in a grid from where it was dropped, in the order the Explorer shows them; a message
becomes a **Table** of its signals. Dropped on a widget that reads one signal, a single signal **re-binds**
that widget and fills in what the database knows. The drag carries references - a message name and a signal
name, never a pointer into a database that could be reloaded between the press and the release - in
`application/x-torquebus-signals-v1`, and the same text (`Message.Signal`, one a line) for anything that takes
a drop and does not know the format.

**Add from Database...** (right-click the Dashboard in Edit mode, first entry). A dialog that searches every
signal of every database and groups them by message. Select several, or a message for a table; *Show as*
overrides the kind for every signal selected ("Automatic" is what `suggestKind` says); the pane on the
right says what the selected signal is - unit, range, bit layout, comment, how often it is sent, who sends
it, its value table - and which widget it would make by default, because that is what a person will be
surprised by.

**Add Image...** or a **file dropped** on the Dashboard (see *Pictures*).

**The form.** Select a widget in Edit mode and the *Widget* dock shows its settings:

* *Message* and *Signal* **complete as they are typed**, from the databases. A signal typed before its
  message completes as `Message.Signal` and sets both. A message is completed from every database at once.
* **Browse...** opens the same dialog for one signal (or one message, for a Table).
* **Choosing** from a list - completion, or the dialog - fills in what the database knows, the same as a
  drop. **Fill from database** does it again later, after a hand has changed something and the hand turned
  out to be wrong. *Typing* never replaces anything: only a choice does.
* One line under the fields says what the binding **is** - the signal, its unit and range, how often it is
  sent - or what is **wrong** with it: *"EEC1 has no signal "EngineSped". Did you mean EngineSpeed?"*. The
  nearest name is offered when it is close enough to be a typing mistake or a case difference, and a signal
  that exists in another message is offered qualified.
* A second line says whether the signal is **arriving**: its last value, or - when it is not in the plot
  store at all, which is the commonest reason a gauge shows dashes and the one nobody can see from the gauge
  - that a *Signal Plot* block has to receive it.

### When a name is wrong

Nothing is refused: a Dashboard opens as often with the database unloaded as with it loaded, and refusing to
open a project because a `.dbc` is on another machine would be the wrong trade (`validate()` does not ask
whether a signal exists). `SignalDictionary::audit` asks, **for a person to read**:

* in Edit mode a widget whose message or signal is in no loaded database wears a **mark**, and hovering
  names the nearest thing that is;
* opening a project writes one line per such widget - and per missing picture - on the Output panel;
* hovering any widget (in either mode) says what it is, what it is bound to, what it reads now, what the
  database knows of it and what is wrong with it.

With no database loaded the dictionary is empty and an empty dictionary cannot say a widget is wrong: the
form says "No database is loaded, so these names are not checked".

---

## Pictures

A picture is a file. **PNG, JPEG, GIF, BMP and SVG** are drawn; SVG is rendered at whatever size the widget
has, and raster pictures are kept at no more than 1600 pixels on a side. A picture can be:

* an **Image** widget on its own - a logo, a diagram of a vehicle;
* an Image **bound to a value**, with a picture for each of the value's names: a door open or shut, the gear
  that is engaged, the regime the model says the engine is in. The widget's own picture is drawn for a value
  no state names. A state that has none shows the widget's own. Until its value is known it shows the first
  state's picture, turned down - what it would be, not what it is;
* the **bulb of a Lamp**: lit it is as it is, dark it is the same picture turned down, so that it still
  looks like the lamp that was on.

To add one: **drop a picture file** on the Dashboard (one file on a widget that has a picture of its own
replaces *that* widget's picture; anywhere else - or several files - makes Images), **Add Image...** in the
context menu, or *Browse...* in the form. *Named values* has a picture column for the kinds that can show one.

### Where the file is

A project is a file that names other files, and a dashboard's pictures follow the rule the database list and
the scripts already do:

* **stored** relative to the project's folder when the picture is in it or up to four folders away, with
  forward slashes whatever the platform - a project is read on more than one - and absolute when it is on
  another drive or farther off;
* **resolved** by joining a relative path to the project's folder (an unsaved project has none, and its
  relative paths mean the current directory, as a script path does);
* **rebased** by *Save As*: the folder moves and the pictures do not, so every relative path is worked out
  again - on a copy, so that a save that fails leaves the dashboard as it was.

A picture that is not where the project says it is is **not an error that refuses the project**: the widget
shows a placeholder that says so, hovering names the file, and the Output panel lists it when the project is
opened. A file saved over in an editor is noticed within a couple of seconds.

---

## Alarm limits

A reading drawn in the text colour says nothing about whether it is a good one. A Gauge, Numeric, Bar or
Trend can carry up to four **limits** - warning below and above, critical below and above - and what it
draws turns amber past a warning one and red past a critical one: the needle and the arc of a gauge, the
fill of a bar, the number, the line of a trend (which also draws the limits as dashed lines at the heights
they are). A gauge marks them on its scale.

Each limit is optional and a widget with none draws what it always drew. They are **strictly** past - a
coolant at exactly its warning limit has not exceeded it - and between them a reading is neutral, not green:
a gauge that turned green whenever it was fine would be a gauge that is mostly green. `validate()` refuses
limits that contradict each other (a warning above that is under the warning below, a critical limit
inside the warning one). The tint is a *tone*, not a colour - the theme decides what danger looks like, so
a dashboard built on the dark theme reads the same on the light one.

The names of a State and a Lamp are tinted the same way, by the tone of the state the value is in.

---

## A signal that goes quiet

A needle that stays where the last frame left it looks exactly like a healthy reading. A widget with a
**stale time-out** (*Stale after*, a tick and a number of milliseconds) that has not seen a new sample of its
signal for that long **stops showing the last value**, shows dashes and a *stale* badge, and says so when
hovered. A Trend keeps its history and wears the badge.

* A widget made from a database gets three of its message's periods (`GenMsgCycleTime`), clamped between half
  a second and a minute. A message that missed one has probably only been late; a 20 ms message is not stale
  at 60 ms, it is a bus having a busy moment.
* One made by hand, or from a database that does not say how often a message is sent, has none, which is what
  every widget did before the setting existed.
* *Stale* is a signal that **was alive** and stopped. One that never spoke, or whose last word was "not
  available", is unknown, not stale.
* It is measured against the panel's own clock, from the moment the newest sample was first seen to change,
  so it works the same for a live bus and a replay, and a stopped measurement turns every widget that has a
  time-out stale once it has passed.

---

## Trends and tables

A **Trend** draws the last *Window* seconds (30 by default) of one signal as a line, in the box it is given.
It is not a second Plot Viewer - it is a signal at a glance, with its limits, next to the widgets that say
what it is. A sample that is not a number ("not available") **breaks the line** instead of plunging it to
zero. A Trend bound to a **variable** has no history in the plot store, so the panel keeps its own trail from
the moment the Dashboard was shown.

A **Table** lists every signal of one message with what each reads now - the number with its unit, or the
*name* of the value when the signal has a value table and the value has one - tinted by what the name means.
It is what a person wants when they have found the message and want to see what is in it, without placing
eight widgets by hand. A box too small for the rows says *+ N more*.

---

## The fault list

The cluster says that something is wrong; a technician wants to know what. A **Faults** widget (*Add Faults* in
the menu) is the answer next to it: the four DM1 lamps of the whole bus - **STOP**, **WARN**, **MIL**,
**PROTECT** - and under them one row for every trouble code the bus is reporting *now*, whichever ECU reports
it:

```
 Active faults
 ● STOP     ● WARN     ○ MIL      ○ PROTECT
┃ SPN 100 / FMI 1  ×1                                          SA 0 · Engine
┃ Data valid but below normal operational range - most severe level
┃ SPN 1043 / FMI 12  ×127+                                          SA 11
┃ Bad intelligent device or component
  + 2 more
```

* A **row** is `SPN n / FMI m`, the failure mode in words (J1939-73, Table 2), how many times the ECU says the
  fault has occurred (127 is the most the field holds, so it is written `×127+`), and which ECU: its source
  address and, after a dot, the name of its function when the ECU was seen claiming its address *and* the name
  tables know it (`SA 0 · Engine`). An ECU that never claimed one is an address and nothing more - the
  example's six Lua ECUs claim theirs when they start, and its TinyML ECU does not, so its rows say
  `SA 128` - because a number known to be right beats a name that is a guess. An SPN the J1939 block did not assemble (a legacy packing nobody declared, see
  `J1939Diagnostics.h`) is `SPN ?` and not a number that would look like one; hovering gives the four bytes.
* **The tint is the lamp's.** A lamp belongs to a DM1 and not to one code in it, so a row is as bad as the
  lamps of the message it arrived in: the red stop lamp makes it red, any other lit lamp (amber warning,
  malfunction indicator, protect) amber, and a message whose lamps are all dark leaves its codes plain -
  a logged fault that is not yet worth telling the driver is not drawn as an alarm. The list is **worst
  first**, and within a rank in the order of the bus (ECU by ECU, address ascending), so it does not shuffle.
  A box too small for the rows says *+ N more* and keeps the worst on show.
* **No fault is an empty list**, and so is everything that looks like it: a healthy bus, a measurement that
  is not running, a graph with no J1939 block. The widget says *No active faults* in the quiet colour. It
  is never an error, and a measurement that has stopped shows no code.
* **Hovering** says which lamps are lit and gives the first few codes in full, with the ECU and what is wrong.
* The **lamps** are the ones the instrument cluster reads, the system variables `j1939.lamp_stop`, `_warning`,
  `_mil` and `_protect` that the J1939 block writes (`databases.md`, *Fields that are not one signal*) - the
  lamp on the cluster and the lamp on the list are one variable and cannot disagree.

### Where the codes come from, and the thread they cross

```
 engine thread                                               GUI thread, every 50 ms
 J1939Node.process ── every pass ──► the codes being reported now
                         │                      │ published only when the list changed
                         ▼                      ▼
              j1939.lamp_* variables     J1939Network  ◄── revision()  (an atomic load, every tick)
                         │                      │          snapshot()  (a copy, when it moved)
                         └──────────────►  DashboardPanel ──► the rows, and the four lamps
```

The panel is given a `const J1939Network*` and nothing else of the block's, and asks it two things: its
revision, which is an atomic load, every tick, and - only when that moved - a copy of what it holds, which is
taken under the lock the block takes to publish. It never reads a table the executor thread is writing, and a
steady bus, which publishes nothing after its first second, costs the panel one load per tick. The ECUs a row
names and the codes it lists are published together and so are from the same pass.

**The block ages the list, not the panel.** What `J1939Network` keeps for the J1939 panel is what each ECU
*last said*, published when its content changes - so the time stamped on a steady DM1 is when it *changed*, and
says nothing about whether the ECU is still repeating it. Only the block sees every frame. On every pass it
works out the codes of every DM1 that is still being repeated - one that has not come for three seconds of
**bus** time is an ECU that is switched off or unplugged, the rule the lamps already use
(`j1939DiagnosticIsCurrent`) - and publishes the list when it differs from the last one. So an ECU that falls
silent takes its rows off the list at the moment it takes its lamp out, an ECU that comes back puts them back
with a DM1 that is byte for byte the one before, and at **Stop** the block publishes the list empty - while
the J1939 panel still shows what every ECU last said, which is what it is for afterwards. Time is the bus's, so
a log being replayed ages the same way.

**A bus that falls silent altogether** - every ECU at once, an adapter unplugged - has no newest frame to
age against, because the clock of the bus is its frames and it stopped with them. On a pass in which nothing
arrived (or only error and remote frames, which say nothing a DM1 is repeated by) the block asks the machine's
clock how long that has gone on, and ages from where the bus's own stood: three seconds of silence is the
same as an ECU that stopped repeating itself, and the list and the lamps let go together, not at Stop. Only
the *length* of the silence is taken from the machine and never what time it is - the two clocks are not
compared, so a replayed log or a device that stamps its own frames is not confused - and the clock is not
asked at all while nothing is reported and no lamp is lit, which is a healthy bus between its messages. A
replay that is paused, or has ended, is a silent bus and ages the same way.

---

## Arranging

* **Group** is a titled frame. It is sent to the back when it is made, and what is put on it afterwards is in
  front: later widgets are drawn over earlier ones and hit first, so the order of the list is the only
  layering there is. *Bring to Front* and *Send to Back* are in the widget's context menu.
* **Duplicate** (`Ctrl+D`, or the context menu) copies the selected widget a little down and to the right,
  with the next free number on its id (`engine_speed_2`); the copy is the selected one.
* **Delete** (`Delete`, `Backspace`, or the context menu).
* A long name is cut with an ellipsis to the width of its box instead of running past it, and a number is
  made smaller before it is cut: a reading somebody needs to read is never replaced by dots where its unit
  was.

---

## The project file

Everything above is stored in the `dashboard` of the `.tbsproj`, and **only when a widget uses it** - a
dashboard that does not is byte for byte what it was before they existed, so an older build is not asked to
understand a file that has nothing in it for them. The format version is unchanged.

| Key of a widget | Holds |
|---|---|
| `kind` | `gauge`, `numeric`, `lamp`, `button`, `switch`, `slider`, `knob`, `label`, `cluster`, `bar`, `state`, `image`, `trend`, `table`, `group`, `faults`. A kind this build does not have is **refused**, not drawn wrongly - which is also what an older build does with a project that has a fault list |
| `binding.source` | `signal` (`message` + `signal`), `variable` (`variable`), `message` (`message`, for a table) |
| `image` | the picture, as *Pictures* says |
| `states` | `[{ "value", "label", "tone", "image" }]` - `value` is the **physical** value; `tone` is one of `ok`, `info`, `warning`, `danger`, `off` (a tone this build does not know reads as none) |
| `limits` | `warningBelow`, `warningAbove`, `criticalBelow`, `criticalAbove` - each only when set |
| `windowSeconds` | a Trend's window, when it is not 30 |
| `staleMs` | the time-out, when there is one |

A fault list adds nothing to this: it is a `kind`, a place, a size and a title, with an empty `binding` - its
codes are not in the project, they are on the bus.

`validate()` refuses what can be refused without a database, a measurement or a screen: duplicate ids, a
widget bound to nothing that needs a binding, a range that cannot be swept, a control bound to a signal it
cannot write, a table bound to anything but a message, two names for one value, limits that contradict each
other, a non-numeric state or limit, a negative size, a cluster whose profile this build does not have.

---

## The examples

Both example projects now show a Dashboard that is more than its cluster, built the way this page describes
(`examples/projects/`). The pictures are in `examples/images/`, small SVG files, and are named relative to
the project.

* `j1939-vehicle.tbsproj` - beside the cluster: fuel and DEF **bars** with limits, battery and oil pressure
  **numerics** that turn amber and red (try the `low_oil_pressure` scenario of the engine ECU and watch the oil
  pressure go red with the cluster's lamp), the AI regime as an **image with a picture per state** next to its
  **state** chip, **trends** of engine speed and coolant temperature with their limits, and a **table** of the
  model's message, all in a **group** where they belong together; and under the trends an **Active faults**
  list - dark and empty for a healthy vehicle, and with the engine ECU on `low_oil_pressure` the stop lamp
  lit and one red row, `SPN 100 / FMI 1`, from `SA 0 · Engine`.
* `virtual-vehicle.tbsproj` - the same for the 11-bit vehicle's TinyML telemetry.

`ExampleDashboardTests` keeps them honest: every widget names a signal its database has, every picture is
where the project says (and `DashboardPaintersTests` that each one loads), and every signal a widget reads
is delivered to the plot store by running the project's own pipeline - a widget bound correctly to a signal
nothing carries is a widget that shows dashes for ever. The fault list is bound to nothing, so nothing there
could notice it showing an empty list for ever: the test runs the J1939 example with the engine on
`low_oil_pressure` and expects SPN 100 on the list under a red stop lamp, a healthy run to leave the list
empty and the lamps dark, and Stop to empty it.

---

## How it is built

```
src/core/dashboard/                      no Qt
    DashboardDescription   kinds, traits, states, limits, validate()
    SignalDictionary       the databases as a flat list; suggest, search, audit
    DashboardAssets        where a picture is, and moving it with the project
    DashboardFaults        the fault list as data: which code is worst, which ECU, what it is called
src/core/j1939/                          no Qt
    J1939Diagnostics       ... and the codes being reported now (j1939ActiveFaults), aged by the bus's clock
    J1939Network           the hand-over the block publishes to and a panel copies from
src/ui/dashboard/
    DashboardPanel         the widget: modes, drag and drop, hover, stale, trails, the fault list's rows
    DashboardWidgetPainters  the newer kinds, as free functions that take a painter
    DashboardImages        the pictures, cached, re-read when the file changes
    DashboardText          the sentences: summaries, findings, the words of a trouble code
    SignalMime             what a dragged signal is
    SignalPickerDialog     Add from Database... / Browse...
    DashboardWidgetEditor  the form
src/ui/database/DatabasePanel   the DBC Explorer: a drag source
src/services/ProjectFile        the file format above
```

The rules are in the core so that one set of rules answers the form, the drop and a test. The painters of the
newer kinds are free functions of a painter, a rectangle and a reading, so a test draws one on a `QImage` and
asks for the colour of a pixel - a stronger statement than "it did not crash" and a weaker one than a golden
image, which would break on every font.

## Tests

| Suite | What it pins |
|---|---|
| `DashboardTests` | the description, `validate()`, the file format round trip, the kinds and what each asks for |
| `DashboardFaultsTests` | the fault list as data: what a lamp says about a code, worst first, an ECU named only when the NAME and the tables say so, and the lamps the cluster reads |
| `J1939NodeTests`, `J1939DiagnosticsTests` | what the block hands over: the codes being reported now with their lamps, an ECU that falls silent leaving the list and coming back to it, Stop emptying it while what the ECUs said stays, a steady bus publishing once, and a reader never seeing half a publication |
| `SignalDictionaryTests` | search and ranking, `suggestKind`, `applyEntry`, raw to physical, tones, time-outs, the audit and its did-you-mean |
| `DashboardAssetsTests` | relative and absolute, `..`, Windows paths, moving a project |
| `ExampleDashboardTests` | the two example dashboards, against their databases, their files and their pipelines - and the J1939 example's fault list on a run of the engine's `low_oil_pressure` |
| `DashboardPaintersTests` | the painters by pixel: how full a bar is, what a state is tinted, which picture appears, a line that breaks at a gap, the fault list's lamps, tints, empty list and *+ N more* |
| `DashboardPanelTests` | drops sent as a drop is sent, re-binding, pictures, duplicate and delete, the mark on a wrong binding, hover, stale, tables, trails; the fault list against a network of its own, repainting by itself, the menu entry, and the J1939 example run all the way to the screen |
| `DashboardWidgetEditorTests` | which fields each kind shows, completion, what a choice fills in and typing does not, limits, named values |
| `SignalPickerDialogTests` | what the dialog lists, chooses and says; the DBC Explorer's drag at the model, where Qt asks for it |
| `MainWindowDashboardTests` | the window's wiring, with the whole window built: the databases a project loads reach the Dashboard and its form, the project's folder is where its pictures are, the Output panel says what a dashboard names that is not there, and the engine's J1939 network and the window's name tables reach the fault list |

The panel is driven the way a person drives it - a drop event sent to the widget, a clock the panel reads for
itself - and not through the methods the drop calls, because a drop that is accepted and does nothing is the
failure this has to catch.

## Not done

* The **fault list** shows the whole bus: it cannot be limited to one ECU, it does not show the codes that
  were active (DM2) and it cannot clear them (DM11, which is a request on the bus and so a transmit entry).
  The example's TinyML ECU does not claim an address, so a code of its own says only `SA 128`.
* **Copy and paste** between projects, a **snap to grid**, and moving several widgets at once: a widget moves
  one at a time and a drop of many lays them out for you.
* A CAN signal cannot be *written* from a widget, on purpose (see *What it is made of*).
