# TorqueBus Studio — Architecture

This document is the contract. `PLAN.md` describes what TorqueBus Studio will
become; this file describes the shape it has to keep while getting there.

Everything below is enforced somewhere: by a link dependency in
`src/CMakeLists.txt`, by a test, or by review. Where a rule is enforced
mechanically, the enforcement point is named.

---

## 1. The ten rules

These are frozen. A change to any of them is an architectural decision, not a
refactor.

| # | Rule | Why it exists | Enforced by |
|---|------|---------------|-------------|
| 1 | No UI code talks to hardware directly. | A panel that opens a channel cannot be tested, reused or driven by a script. | Review; `torquebus_ui` reaches drivers only through the registry. |
| 2 | No driver knows the UI exists. | A backend is a translation layer, not a feature. | `torquebus_drivers` does not link `Qt6::Widgets`. |
| 3 | The core does not depend on widgets. | The core must be linkable into a headless logger, a CLI and a test binary. | `torquebus_core` links neither `Qt6::Widgets` nor `Qt6::Gui`. |
| 4 | `CanFrame` is vendor-independent. | Otherwise every new backend leaks into every consumer of a frame. | `core/can/CanFrame.h` includes nothing but the C++ standard library. |
| 5 | A received frame never becomes a `QObject`. | At 100k frames/s, one allocation and one signal emission per frame is the whole CPU budget. | `static_assert(std::is_trivially_copyable_v<CanFrame>)`; `CanFrameTests.cpp`. |
| 6 | The UI never blocks waiting for hardware. | A bus-off adapter must not freeze the window. | Backends deliver through callbacks on their own threads. |
| 7 | Logging never depends on the UI. | Closing the Trace panel must not stop a recording. | Logging engine owns its own thread and writer. |
| 8 | Protocols never depend on specific hardware. | ISO-TP and UDS must run over Kvaser, PEAK, a replayed log or a simulated ECU without changing. | Protocol layers consume `CanFrame`, not backends. |
| 9 | Hardware is discovered by capability, not by brand. | `if (driver == PCAN)` scattered through the code is how a tool becomes unmaintainable. | `CanCapabilities`, queried at enumeration. |
| 10 | Anything that can grow gets an API boundary. | Drivers, databases, protocols and scripting are all third-party extension points. | `ICanBackend`, `IDatabaseParser`, the plugin directory. |
| 11 | Every data path is an edge in the pipeline graph. | Two ways for data to reach a panel means two things to keep in sync, and they diverge. The graph is not a view of the pipeline; it *is* the pipeline. | `PipelineGraph` owns every source, transform and sink. |
| 12 | Nodes exchange batches, never single frames. | A graph walked once per frame, with a virtual call per edge, cannot hold 100k frames/s. Batching is what makes a visual pipeline affordable at bus speed. | The throughput test runs through the graph, not around it. |

Rules 11 and 12 were added when the pipeline graph replaced the fixed
sink list. The first ten are unchanged.

---

## 2. Layers

```
                        TorqueBusStudio (app)
                                 |
                          torquebus_ui
                     Qt Widgets + KDDockWidgets
                                 |
                       torquebus_services
                 settings, projects, workspaces
                                 |
                       torquebus_drivers
              ICanBackend + one impl per vendor
                                 |
                        torquebus_core
        CanFrame, CanTypes, Result — std C++ only, no Qt
```

Dependencies point **downwards only**. The link declarations in
`src/CMakeLists.txt` are the machine-readable version of this diagram: a
`#include <QWidget>` inside `core/` is a build error, not a review comment.

| Target | Links | Deliberately does not link |
|---|---|---|
| `torquebus_core` | `Threads::Threads` | Qt entirely; `torquebus_drivers` |
| `torquebus_drivers` | `core`, optionally `Qt6::SerialBus` | `Qt6::Widgets` |
| `torquebus_services` | `core`, `Qt6::Core` | `Qt6::Widgets` |
| `torquebus_ui` | `core`, `drivers`, `services`, `Qt6::Widgets`, `KDAB::kddockwidgets` | — |

**`ICanBackend` belongs to the core, not to the drivers.** The core defines the
abstraction; each vendor implements it. The header lives at
`src/drivers/api/ICanBackend.h` because that is where PLAN.md section 10 puts
it and where an implementer looks for it — but it compiles into
`torquebus_core`, because a core that linked `torquebus_drivers` would drag
every vendor backend into a headless test binary. This is dependency
inversion, and the link graph enforces it.

**The engine's thread is `std::thread`, not `QThread`.** Rule #3 is not a
preference about headers; it means the measurement runs with no event loop, no
`QObject`, and no Qt at all in the path a frame takes.

---

## 3. The frame pipeline

The single most performance-critical path in the product. It is designed once,
here, and every later module hangs off it (`PLAN.md` sections 16–17).

```
   hardware / vendor driver
             |
             v
    backend receive thread          <- vendor structs become CanFrame here,
             |                          and never appear above this line
             v
       frame queue (lock-free)      <- fixed capacity; overflow is counted,
             |                          never allowed to grow without bound
             v
      pipeline executor             <- one thread, drains every source,
             |                          then runs the compiled graph
             v
      +------+------------------------------+
      |      the graph (see section 3b)      |
      +------+------------------------------+
             |
             v
    sink nodes: trace store, logger, plot, script
```

Non-negotiable properties:

- **Batching, not signalling.** The UI receives batches (100–1000 frames) or
  timed updates. It never receives one signal per frame.
- **Bounded queues.** An overflow increments `CanBusStatus::softwareOverruns`
  and is reported. Silent frame loss is a defect.
- **The logger is upstream of the UI.** Closing the Trace panel changes nothing
  about what is written to disk.

### Who counts a dropped frame

`FrameQueue` counts only what **it** threw away:

- `push()` returning `false` is a refusal, not a loss — the caller still holds
  the frame and decides whether to retry or discard. Nothing is counted.
- `pushBatch()` discarding a tail that does not fit **is** a loss: the
  backend's batch is gone the moment the handler returns. That is counted, and
  surfaces as `softwareOverruns` in the statistics.

The distinction matters because "the adapter could not keep up"
(`hardwareOverruns`), "TorqueBus could not keep up" (`softwareOverruns`) and
"your filter removed it" (`filteredFrames`) are three different diagnoses that
must never be collapsed into one number.

### One dispatch thread, not one per channel

`CanEngine` drains every channel from a single thread. Deliberately:

- Frames from different channels must reach the trace in a consistent order
  relative to each other. Independent threads would interleave
  non-deterministically and make a recording unreproducible.
- Sinks then need no locking of their own — a file logger and a trace store are
  always called from the same thread.
- One thread is enough. The per-frame work is a filter test and two increments;
  the queues absorb the burstiness.

### Where Qt enters

Exactly one place: `ui/engine/CanEngineController`. Statistics (~10 Hz, small)
are copied and posted to the GUI thread. Frames are **not** signalled at all —
the GUI thread pulls counters on its own timer, so a stalled UI can never
back-pressure the engine, and no per-frame path into the UI exists to be
closed again later.

---

## 3b. The pipeline graph

TorqueBus does not have a fixed set of panels wired to a fixed set of sources.
It has a **typed dataflow graph**, and the panels are nodes in it:

```
[Kvaser CAN 1] ──► [DBC Decoder] ──┬──► [CAN Trace]
                                   │
                                   ▼
                          [J1939 Decoder] ──► [PGN Filter] ──┬──► [Signal Plot]
                                                             │
                                                             └──► [Lua Script]
```

This is the one idea worth taking from CANdevStudio, and it is what separates
this project from a re-implementation of TSMaster. A fixed workbench answers
"show me the bus". A pipeline answers "decode this as J1939, keep only these
PGNs, feed them to my script and plot the result" — without a rebuild, and
without us having anticipated that particular combination.

### Typed ports

Edges carry a type, and the editor refuses connections that do not typecheck.
A DBC decoder does not emit frames; it emits signals. Pretending otherwise -
one universal "CAN data" type - moves the error from connection time to
runtime, which for a visual tool is the whole difference between a diagram you
can trust and a diagram you have to debug.

| Port type | Payload | Produced by |
|---|---|---|
| `Frames` | `std::span<const CanFrame>` | channels, replay, script nodes |
| `Signals` | decoded signal values with timestamps | DBC / ARXML decoders |
| `Pgns` | J1939 / ISOBUS PGNs, reassembled | J1939 transport |
| `Events` | diagnostic responses, DTCs, state changes | UDS, ISO-TP |

New types are added as the protocol layers land. What must not happen is a
node that accepts `Frames` and quietly means something else.

### Batches, not frames (rule #12)

The graph is **compiled before a measurement starts**, not interpreted per
frame. Nodes are topologically sorted once; the executor then walks a flat list
and hands each node a whole batch.

This is not an optimisation, it is the feasibility condition. At 150k frames/s
- the rate the v0.2 pipeline is tested at on every pull request - a per-frame
walk with one virtual call per edge spends the entire CPU budget on dispatch.
Per batch, that same dispatch is amortised across hundreds of frames and
disappears into the noise.

Consequences that follow, and are not negotiable:

- A node's `process()` takes a span and returns a span. It never sees one frame.
- A node may not allocate on the hot path. Output buffers are owned by the node
  and reused across batches.
- The graph may not be edited while a measurement runs. Editing stops the
  measurement, recompiles and restarts - which is honest, cheap, and avoids an
  entire class of concurrency bugs that would otherwise live forever.

### Composed topology: the graph builder

The graph is authoritative at runtime - every frame goes through it - and
`CanEngine::buildGraph()` rebuilds it from scratch on every start. Rebuilding
is right: the graph is small, and a stale edge pointing at a channel that no
longer exists is the kind of bug that survives for months.

But it means a node added to `graph()` by hand is destroyed by the next start,
and a user who pressed Stop and Start would find their ECUs gone with no error
to explain it. So the project contributes nodes through a **callback**, not by
handing them over once:

```cpp
engine.setGraphBuilder([&](PipelineGraph& graph,
                           std::span<const NodeId> sources) -> Result {
    const NodeId ecu = graph.addNode(std::make_unique<LuaEcuNode>(source, name));
    return graph.connect(PortRef{sources[0], 0}, PortRef{ecu, 0});
});
```

It runs after the default graph is built, so a builder can wire to any channel
source and to anything the default put there, and a failed `Result` fails the
start with the message the user sees - a script that will not compile stops the
measurement from starting rather than surfacing on the first frame.

The consequence is worth stating, because it is what a user feels: every start
recreates the nodes, so **a Lua ECU reloads its script and resets its state on
Start**. That is what pressing Start should mean, and it is why script edits
take effect without restarting TorqueBus.

### Two graphs, on purpose

`PipelineGraph` is the *running* graph: compiled, with node objects and an
execution plan, alive only between start and stop. `GraphDescription` is the
*user's* graph: ids, types, settings, wires and canvas positions, which outlive
any measurement and survive a round trip through a file.

```
GraphDescription  ──build()──►  PipelineGraph  ──compile()──►  execution plan
   (the project)                 (this run)                     (this pass)
```

The description is the source of truth and the running graph is derived from
it, every time. That is what lets the engine rebuild from scratch on every
start - the correct thing to do - without the user losing anything.

`NodeCatalog` is what turns a type name plus settings into a node. It exists
because until it did, a node could only be created by C++ that named its class,
which works exactly as long as the person choosing the nodes is the person
compiling. A canvas, a project file and a script that assembles a measurement
all need the same thing instead: data.

Each registered type declares its ports and parameters *without instantiating
anything*, so a node palette can show "CAN Channel: one output, Frames" before
the user has dropped one, and a properties panel can build its fields from the
parameter list rather than from a switch on the type name. Adding a node type
should not require touching the UI.

Nodes are addressed by string id rather than index: a project file that
renumbers when a node is deleted has unreadable diffs and edges that quietly
point somewhere else.

`validate()` is deliberately separate from `build()`. The canvas needs to refuse
a wire while the user is drawing it - no engine, no instantiated nodes - and
every message names the node, and both ends where there are two, because the
user is looking at blocks with labels on them rather than at C++ types.

Canvas positions live in the description and are ignored by the runtime.
Reopening a project to find the blocks rearranged into a default layout would be
its own small betrayal, and carrying two doubles costs the executor nothing
because building never reads them.

What is still missing is durability and the canvas itself: a description is
assembled in C++ today. Drawing it is v0.6; writing it to a `.tbsproj` is v0.13,
and is now close to free.

### One reader, one drain

`ChannelSourceNode` publishes a view of what the engine already drained this
pass; it does not drain the channel itself. The difference matters because
draining consumes, and a graph must let two blocks reading CAN 1 both see CAN 1
- where the engine's own default trace path is already one of those readers.

Before the graph runs, the engine drains each channel once into that channel's
pass buffer. Filtering and statistics therefore happen exactly once, which is
what keeps the status bar showing what entered the graph rather than what the
driver handed over.

This was a real defect, found by a test: a described graph carrying its own
`can.source` for CAN 1 answered nothing, because the engine's source had taken
the batch first. Whichever node ran first won - silently, and depending on the
order the user happened to add blocks in.

### The default graph is implicit

The trap in CANdevStudio is that you must build a graph before you can see
anything. That is the right price for an advanced pipeline and the wrong price
for "I plugged in an adapter, show me the bus" - which is most of what anyone
does most days.

So TorqueBus builds the minimal graph itself. Detecting two channels produces:

```
[CAN 1] ──► [CAN Trace]
[CAN 2] ──┘
```

already running, with no canvas visit required. The canvas *exposes* that graph
rather than being a precondition for it. A user who never opens the Simulation
panel gets a TSMaster-shaped tool; a user who does gets the pipeline. Neither
pays for the other.

### Where the node graph library sits

QtNodes 3.x (`DataFlowGraphModel`, `NodeDelegateModel`, `GraphicsView`) draws
the canvas, and like KDDockWidgets it is confined behind one seam - the canvas
is an *editor* for `PipelineGraph`, never its owner. The runtime graph has no
dependency on QtNodes at all, which is what keeps rule #3 true: a measurement
must be runnable headless, from a script or a CI job, with no GUI present.

Note that CANdevStudio is on QtNodes **2.x** (`FlowScene`, `NodeDataModel`),
Qt5 and C++17, last touched in May 2024. The API generations are not
compatible, so nothing is ported; the debt is intellectual, and acknowledged.

### Scripting: one VM per ECU

A Lua ECU is a node whose behaviour is a script (`docs/development/scripting.md`
is the contract). Three decisions are worth recording here.

**Lua 5.5.0 is vendored**, built as part of the project. No external Lua to
install, no version to keep in step with, and no chance of a user's system Lua
changing behaviour underneath a saved project.

**One interpreter per node.** Two copies of the same script must be able to run
at once - a bus with four identical sensors on it is an ordinary thing to
simulate - and sharing one VM would make their globals collide. It costs 13 KB
per ECU, which is nothing next to being unable to run the same script twice.

**`emit` does not reach the bus.** It puts a frame on the node's *output port*,
and where that goes is the graph's business. An ECU wired to nothing is a
valid, testable thing; the same ECU wired to a filter, a trace and a channel is
three different experiments with no change to the script. `ChannelSinkNode` is
what closes the loop to real hardware.

Only `LuaRuntime.cpp` includes `lua.h`, so the interpreter is a private
implementation detail of the core rather than a dependency of everything that
touches a node.

### Performance targets (from v0.4 onwards)

| Metric | Target |
|---|---|
| Frames retained without UI stalls | ≥ 1,000,000 |
| Internal throughput without loss | ≥ 100,000 frames/s |
| Filter application | Perceptibly instant |
| Logging while the Trace panel is closed | Unaffected |

These are tested, not aspired to.

---

## 4. The driver boundary

`ICanBackend` (`src/drivers/api/ICanBackend.h`) is the seam. Adding a vendor
means writing one implementation and adding one line to
`CanBackendRegistry::registerBuiltins()`. Nothing else changes.

```
                    TorqueBus Core
                          |
                    ICanBackend
                          |
   +--------+-------------+-------------+--------+
   |        |             |             |        |
Virtual   Kvaser        PEAK        (future)  (plugin)
 built-in  CANlib   Qt SerialBus     Vector    third-party
                     / PCAN-Basic    IXXAT     .dll
                                     SocketCAN
                                     TOSUN
                                     J2534
```

### Lifecycle

```
enumerate()  ->  open(config)  ->  start()  ->  ...  ->  stop()  ->  close()
```

- `enumerate()` is valid on a closed backend at any time.
- `isAvailable()` reports whether the vendor SDK is installed. A missing DLL
  produces a greyed-out row in the Hardware Manager, never a crash at startup.
- A successful transmission comes back through the frame handler as a `Tx`
  echo, so the Trace shows what reached the bus rather than what was requested.

### Threading contract

The frame handler is invoked from the backend's receive thread. It must not
block, must not allocate on the hot path, and must not touch a widget. It hands
the batch to a queue and returns.

### Proprietary SDKs

Vendor SDKs are never committed (`PLAN.md` section 31). The build detects them;
their absence downgrades a backend to unavailable and never breaks the build.
This keeps the GPLv3 distribution clean.

---

## 5. Testing without hardware

Three levels, as laid out in `PLAN.md` section 32.

| Level | What it covers | Needs hardware | Runs in CI |
|---|---|---|---|
| Unit | `CanFrame`, DLC maths, `Result`, queue, filters, statistics, settings | No | Always |
| Integration | The virtual backend and the engine, end to end, including throughput | No | Always |
| Hardware | Kvaser (and later PEAK) through the real vendor driver | Driver, not necessarily an adapter | Labelled `hardware`, excluded by default |

`tests/hardware/KvaserVirtualTests.cpp` makes the same assertions as
`tests/integration/VirtualBusTests.cpp`, against the Kvaser virtual channels
instead of the built-in bus. That symmetry is the point of `ICanBackend`: if
the seam holds, the same test reads the same way on either side of it, and a
difference in outcome is a difference in the driver rather than in what
TorqueBus expects of one. Each test skips itself, rather than failing, when the
Kvaser driver is not installed.

`VirtualCanBackend` exists specifically so that levels 1 and 2 cover the real
pipeline. `tests/integration/VirtualBusTests.cpp` is already written in the
shape of the v0.3 acceptance test — *Kvaser Virtual 0 → TorqueBus → Kvaser
Virtual 1* — so the assertions are green before any vendor SDK is involved.

---

## 6. UI structure

- **Docking:** KDDockWidgets, reached only through `src/ui/mainwindow/Docking.h`.
  That header is the *only* file in the repository that includes KDDockWidgets;
  it is the entire blast radius of a docking-library change.
- **Panel identity:** each dock has a frozen unique name
  (`torquebus.dock.trace`, …). These names are part of the persisted layout
  format. **Changing one invalidates every saved workspace.**
- **Theming:** no colour is hard-coded in a widget. Widgets ask `Theme` for a
  role. One QSS template serves both variants, with colour tokens substituted
  at apply time, so Dark and Light cannot drift apart structurally.
- **Icons:** one monochrome SVG set, tinted at load time from the active theme.

### Placeholder panels

Panels whose module has not shipped yet exist in the layout from v0.1 as
`PlaceholderPanel`s. This is deliberate: docking, workspaces and layout
persistence are exercised against the final set of panels from the first
commit, so a saved workspace stays valid as each real module lands in place.

---

## 7. Persistence

Two distinct scopes, deliberately not merged:

| | Application settings | Project |
|---|---|---|
| File | `<AppData>/TorqueBus/settings.json` | `<name>.tbsproj` |
| Scope | Follows the *user* | Follows the *work* |
| Contents | Theme, window geometry, dock layout, recent projects | Hardware mapping, bitrates, databases, filters, transmit lists, workspaces, scripts |
| Format | JSON | JSON |

JSON throughout, rather than the registry: a lab can diff it, copy it between
machines and check it into version control.

Writes are atomic (`QSaveFile`): an interrupted save cannot leave a truncated
settings file behind. A corrupt settings file falls back to defaults and never
prevents startup.

---

## 8. Where things go

```
src/
  core/        vendor-neutral, Qt-free domain model
    can/       CanFrame, CanTypes
    logging/   logging engine            (v0.9)
    database/  DBC / ARXML parsing       (v0.7)
    diagnostics/ ISO-TP, UDS             (v0.11-v0.12)
    scripting/ embedded Lua 5.5          (v0.7)
    project/   .tbsproj model            (v0.10)
  drivers/
    api/       ICanBackend, registry
    virtual/   in-memory bus (always available)
    kvaser/    CANlib backend            (v0.3)
    peak/      Qt SerialBus / PCAN-Basic (v0.5)
  services/    settings, projects, workspaces
  ui/          widgets, theming, docking
  plugins/     out-of-tree extension points
```

---

## 9. Decision log

| Decision | Rationale |
|---|---|
| GPLv3 | Qt Graphs is GPLv3 for open source users, and KDDockWidgets is GPL-or-commercial. Fighting that would cost more than it buys, and the project is open source by intent anyway. |
| Own `CanFrame` instead of `QCanBusFrame` | Keeps the core independent of Qt, of every vendor, and of the platform (rule #4). |
| Qt Widgets for the workbench, QML for dashboards | Widgets win on information density and table performance; QML wins on the Dashboard Designer (v0.14). |
| KDDockWidgets over `QDockWidget` | Multi-monitor tear-off, tabbing and layout serialisation are requirements, not nice-to-haves. |
| Backends behind a registry rather than compiled in | Third parties can ship `driver-acme-usbcan.dll` without touching this repository (rule #10). |
| Virtual backend first | Makes the whole application developable, demonstrable and CI-testable with no adapter on the desk. |
| Qt private modules requested, and their warning silenced | KDDockWidgets links `Qt6::WidgetsPrivate` and `Qt6::GuiPrivate` without asking for them; because we build it via FetchContent, it inherits our `find_package`, so we must. TorqueBus itself uses no Qt private API. See below for what this costs. |
| A typed pipeline graph instead of a fixed sink list | Taken from CANdevStudio, which solved the visual CAN pipeline properly. It is the difference between a tool that answers the questions we anticipated and one that answers questions we did not. |
| The graph is authoritative, with an implicit default | One data path, so nothing can diverge (rule #11) - but built automatically for the simple case, so the pipeline never becomes an entry fee for reading a bus. |
| Lua 5.5 everywhere; Python dropped | Reverses PLAN.md section 28 entirely. Lua is ~200 KB, embeds cleanly, starts a VM per ECU cheaply, is deterministic and has no GIL - all of which matter when every simulated ECU is a script. It is already proven in `cansim`, with a working node lifecycle and 20 scripts. One embedded language means one binding to maintain, one API to document, and one language for the user to learn. |
| Lua built from source, pinned to 5.5.0 | Not a prebuilt `liblua.a` from a local path. CI has to reproduce the build anywhere, and a binary dependency living on one developer's disk is how a project stops building for everyone else. |
| No Lua compatibility flags | `LUA_COMPAT_5_3` would resurrect `math.frexp`, `math.ldexp` and the global `unpack` for three `cansim` scripts that still call them - but it would also freeze the scripting surface in a 5.3 shape that every future script author inherits. The scripts migrate instead, in the direction their own newer siblings already took: `string.pack("<f", x)` replaces twenty lines of hand-rolled IEEE 754 with one. |
| KDDockWidgets **and** QtNodes, not one or the other | The two references each solved half of this. TSMaster has the workbench and no pipeline; CANdevStudio has the pipeline and no workbench density. Combining them is the actual product idea. |

### The Qt version is pinned, not preferred

Requesting Qt's private modules ties the build to one Qt module build. Qt says
so, loudly, at configure time; `QT_NO_PRIVATE_MODULE_WARNING` silences the
message but not the constraint. Concretely:

- A TorqueBus binary must run against the **exact** Qt it was built against —
  6.11.2 today. `windeployqt` copies those DLLs beside the executable, so a
  packaged build is self-consistent; pointing the application at a different Qt
  on `PATH` is what breaks, potentially at an arbitrary point at runtime rather
  than at startup.
- The Qt version in `.github/workflows/ci.yml` and in the documented
  requirements is therefore a hard pin. Bumping it is a deliberate change that
  requires rebuilding and repackaging, not a configuration tweak.

This is inherited from the docking library rather than chosen. If it ever
becomes painful, the escape route is the seam that already exists:
`src/ui/mainwindow/Docking.h` is the only file that includes KDDockWidgets.

### KDDockWidgets paints one frame we cannot turn off

`Config::setDisabledPaintEvents()` is the documented way to stop the library
painting its own chrome so a style sheet can do it instead. Every widget honours
it — except `Group`.

In 2.2.5, `Separator::paintEvent` and `FloatingWindow::paintEvent` both open by
checking `Config::self().disabledPaintEvents()` and deferring to
`QWidget::paintEvent`. `Group::paintEvent` never consults it, and unconditionally
draws a 1px rounded rectangle in a hardcoded `QColor(184, 184, 184, 184)` — from
no palette and no style sheet.

That was the pale border around every panel, and it survived three rounds of
fixes aimed at the style sheet, at the palette's bevel roles, and at the
paint-event flags. All three were reasonable and none could have worked: the
rectangle is painted after everything they control.

The fix is a `ViewFactory::createGroup` override returning a `Group` subclass
whose `paintEvent` calls `QWidget::paintEvent` directly (`src/ui/mainwindow/
Docking.cpp`). The supported extension point, and it leaves the frame to
`torquebus.qss` where the rest of the chrome already lives.

The general lesson is the one this project keeps relearning: when a UI defect
survives two fixes aimed at plausible causes, stop proposing a third and read
the code that draws the pixels. The dependency's source is in the build tree.

### Where the canvas sits

```
GraphDescription        the project's pipeline, owned by MainWindow
       ^
       | reads and writes in place - no copy, no apply step
       |
PipelineGraphModel      src/ui/canvas, implements QtNodes::AbstractGraphModel
       ^
       |
BasicGraphicsScene / GraphicsView        QtNodes' own widgets
```

`PipelineGraphModel` derives from `AbstractGraphModel` rather than using
QtNodes' `DataFlowGraphModel`. `DataFlowGraphModel` keeps the graph in its own
structures, which would mean holding the user's pipeline twice and syncing the
copies — and two copies of a thing editable from both ends diverge. Dragging a
block writes its position into the description; there is no apply step because
there is nothing to apply.

The engine takes its copy at **Start**, not at construction. That is what makes
"what runs is what was on the canvas when you pressed Start" literally true, and
it is what lets the canvas stay editable during a recording. Getting this wrong
once — copying an empty description at startup, so nothing the user drew ever
reached the engine — is the reason it is written down here.

The canvas is a view and nothing more: close it and the pipeline still runs, the
same way closing the Trace panel does not stop a recording (rule #7 generalised).
A headless run needs no canvas at all, which is what v0.16's script runner
depends on.

Three places where QtNodes' documented API and its actual behaviour differ, all
found by reading the library rather than by trying it:

| Documented | Actual |
|---|---|
| `PortRole::DataType` is "a QString describing the port data type" | Every painter unwraps it with `.value<NodeDataType>()`; a QString gives a default-constructed type — unnamed ports, every wire the same colour |
| `AbstractGraphModel` is a `QObject` | It declares no constructor, so there is nothing to pass a parent to; use `setParent()` |
| `NodeRole::Widget` returns an optional `QWidget*` | Read as `nodeData<QWidget*>()`, so an empty `QVariant` already means "none" |

### The project file is the description, written down

`.tbsproj` is JSON, and deliberately readable JSON: a project file is diffed in
reviews, copied between machines and hand-edited in labs. That is why it is
indented, why the keys are in a stable order, and why `NodeParameters` is an
ordered map rather than a hash — opening a project and saving it again with
nothing touched must not produce a diff.

The format follows cansim's `nodes.json` closely, and not by accident: that file
arrived at the same shape from the same problem, with twenty working scripts
configured through it. Where the two differ this one is the superset — ports on
the wires, and canvas positions.

Two decisions worth naming:

**A parameter's type is carried by the JSON value, not by a type field beside
it.** A number stays a number and a string stays a string, which is what keeps
the file readable. The cost is one ambiguity on the way back: JSON has a single
number type, so an integer and a real are told apart by whether the value is
integral. Without that, a CAN identifier would come back as `402653440.0` and be
written out next time with a `.0` that was never there.

**A file that cannot be read fully is not read at all.** Loading builds into a
local `GraphDescription` and assigns it only on success, so a broken file leaves
the canvas showing what was already there rather than half a pipeline nobody
saved. A file from a newer format version is refused outright for the same
reason: opening it would silently drop whatever the newer version added, and a
project that loads with its wires missing is worse than one that will not load.

Saving is atomic (`QSaveFile`: temporary plus rename), because losing yesterday's
work to a crash during today's save is not a trade anyone agreed to.
