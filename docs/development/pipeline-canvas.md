# The Pipeline canvas: a bus with modules on it

The Pipeline tab draws a measurement the way a network is drawn. The **CAN Bus** is a line - the two
wires of the bus, CAN_H and CAN_L - and every module is a **chip** hanging off it by one **drop**, the
way a simulation setup in CANoe draws one: the two wires of the bus carried down to the module, CAN_H in
amber and CAN_L in green, each joined to the wire of the bus that is the same colour. A wire that is not a
drop to a bus (a decoder to a plot) is a curve between two chips.

```
        ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
        │  engine  │ │   body   │ │ cluster  │ │  tinyml  │     modules that speak on the bus hang above it
        └──┬─┬─────┘ └──┬─┬─────┘ └──┬─┬─────┘ └──┬─┬─────┘
           │ │          │ │          │ │          │ │           one drop each: the two wires of the bus
           │ │          │ │          │ │          │ │           carried down - CAN_H (amber), CAN_L (green)
┌────────┐─●─┼──────────●─┼──────────●─┼──────────●─┼───────    CAN_H - a dot where a drop joins a wire
│ can_1  │───●──────────┼─●──────────┼─●────────────●───────    CAN_L - a crossing where it only passes
└────────┘              │ │          │ │
                        │ │          │ │
                        │ │          │ │                        modules that only listen hang below it
                     ┌──┴─┴─────┐ ┌──┴─┴─────┐
                     │ decoder  │ │  trace   │
                     └────┬─────┘ └──────────┘
                          │                                     and what they feed hangs under them
                     ┌────┴─────┐
                     │   plot   │
                     └──────────┘
```

It is smaller than what it replaced on purpose. A block used to be a 310 x 196 card with a frame grid
and a decode panel on every one of them, wired to its channel with one wire to a CAN Channel block and
one to a CAN Transmit block per ECU: the J1939 example was a column eight blocks tall that had to be
zoomed to a third of its size to fit a window, and "making a connection" was a drag between two tiny
ports at that zoom. Now it is one screen.

## What is on the canvas

### Chips and cards

A **chip** (176 x 48) is the block by default: its name, an icon and a stripe in the colour of its kind
(violet for simulation, emerald for the TinyML block, green for decoders and plots, amber for filters and
diagnostics), a light that says whether it is running or in trouble, and one line of what it is doing.
Stopped, that line is what tells two blocks of one kind apart - the channel, the source address, the file.
Running, it is the frame the block last carried: `TX 0CF00400 • 10.0 ms`; for the TinyML block, its regime
and anomaly score.

A **card** (310 x 150) is a block opened up. It is the frame inspection the canvas always had - the last
frame byte by byte, what the database says it means - for the block you want to look inside. Open one
with the chevron at the corner of a chip (shown while it is under the pointer or selected), with
**Show Frame Details** in its context menu, or all of them at once with **Details** on the bar. A card
that hangs over a bus opens away from the bus, and what is beside it in its row moves aside.

Zoomed out far enough that text cannot be read (below about 50 %), a chip is drawn as its name, larger,
and nothing else.

### The bus

The bus is a block that is drawn as a line: a plate with its name and which channel it is
(`CAN 1 • 250 kbit/s`, and `CAN 1 • 1.2k f/s • 7%` while running), the two wires, and the terminating
resistor at the far end. It is as long as the modules on it are spread out. Move a module past the end
and the bus follows; try to move the bus to the right of a module it carries and it stops.

Each module on it is one **drop**, and a drop is the two wires of the bus carried down - or up - to the
module: **CAN_H** in amber on the left, joined to the amber wire of the bus, and **CAN_L** in green on the
right, joined to the green one. A module hangs from one side of the bus, so the wire that has the further to
go passes the near wire of the bus on its way; on a schematic a crossing joins nothing and a dot does, so
each wire ends in a dot on its own wire of the bus and the other has none. Between the two wires a small
arrow says which way frames go, one for each half of the drop that exists - the one the bus delivers to the
module and the one the module puts on the bus. A module under the bus is tapped from its top, one over it
from its bottom.

A module is still wired by dragging **one** wire to the bus. The pair is how that wire is *drawn*, so that the
picture reads as the cable it stands for: the graph holds what the wire says (frames from this port to that
one), and the two conductors of a CAN bus are not something the pipeline carries, so there is nothing to wire
twice.

### Putting a module on the bus

Any of these:

- **Drag a port onto the rail.** From a module's output (its bottom edge) or its input (its top edge) to
  anywhere on the rail. The whole rail is the target while a wire is being dragged, and picks the bus up
  when it is not.
- **Add it to the bus.** Select the bus (or any module on it) and add a block from the palette
  (double-click, or **Add Block**): it is wired to the bus, under it, a drop away, on the bus's channel.
  Or right-click the rail: **Add to This Bus**, **Add Simulated ECU**, **Add TinyML Virtual ECU**.
- **Drop it there.** Drag a block from the palette and let go over or near the rail.
- **Attach.** Right-click any module: **Attach to the Bus** (with several buses, one entry for each).

**One drop is one thing.** The graph holds it as two wires, because "what the bus delivers to a module"
and "what a module sends onto the bus" are different edges with different ports. The canvas does not
make you draw both: wiring one half wires the other when the module is one that speaks on a bus as well
as listening to it (a Lua ECU, a TinyML block, a UDS client), and deleting one half deletes both. A
Frame Filter, a decoder or a trace only listens - a filter's output sent back onto the bus it listens
to would be a loop - and a Rest Bus only speaks.

To change that, right-click the drop: **Direction** is **Receive and send**, **Receive only** or
**Send only**.

### Which frames a module hears

The bus carries 11-bit and 29-bit frames, and a module need not hear both. Each drop that delivers frames
to a module has a pill on it, shown when it has been limited and while the drop is under the pointer:

| Pill | The module hears |
|---|---|
| (none) | every frame |
| `11-bit` | frames with an 11-bit identifier only |
| `29-bit` | frames with a 29-bit identifier only |

Click the pill to go round them, or right-click the drop: **Hears**. A J1939 decoder wired to the bus and
limited to `29-bit` hears what J1939 is, and a Frame Filter in front of it to say so is not needed. The
property is a property of the *wire* (`EdgeDescription::frameFormat`), so it holds on any wire that carries
frames - not only a bus's.

### Arranging

**Arrange** puts every block where a tidy diagram has it: each bus a line across the top of what is wired
to it, the modules that speak on it above (in rows - a row wraps when it is wider than the window can show
at a readable size, and the row further out is half a pitch in so that its drops go between the blocks of
the one nearer), the ones that only listen below, and whatever those feed under them. A block that speaks
on the bus *and* feeds something hangs under the bus, with what it feeds: a drop from over the bus to a
block under it would cross the wires. Blocks on no bus are laid out the same way without one.

Within a row the order the blocks are in is kept, so arranging a diagram somebody has half arranged is a
refinement and not a shuffle, and arranging one that is arranged changes nothing. It is `layOutBlocks()`
in `src/ui/canvas/CanvasArrange.h`, a pure function over the description, which is why it is tested without
a window.

Arranging throws away where somebody put things, so it can be taken back with **Ctrl+Z** - and so can
**Details**, **Group the Wiring on a CAN Bus** and **TinyML Demo Setup** (the last two replace the pipeline,
so they leave one step in the history: the pipeline as it was). Opening a project forgets the history, which
names blocks by ids that are handed out again.

### Projects from before the bus

A project that reaches a channel through a **CAN Channel** and a **CAN Transmit** per ECU still opens and
still runs; those blocks are still in the palette. To move it onto a bus, right-click the empty canvas:
**Group the Wiring on a CAN Bus**. It replaces the CAN Channel with a CAN Bus (taking over its id, so the
wires that named it keep naming it), the CAN Transmits with wires to the bus, and arranges the result.
What runs does not change - the bus builds into exactly the nodes the blocks it replaced were (below) - which
is why it is an action somebody takes and not something a project undergoes when it is opened. A CAN
Channel feeding a CAN Transmit on *another* channel is a gateway, which a pair of blocks says and a bus does
not; both are left as they are.

`examples/projects/j1939-vehicle.tbsproj` and `virtual-vehicle.tbsproj` are shipped grouped.

## How it is built

The bus is a node type, `can.bus`, in the same graph as everything else (rule #11): one input `tx` and one
output `rx`, both frames, and a `channel`.

**`tx` takes any number of wires.** Everywhere else an input takes one, because merging two streams needs a
policy. `PortDescriptor::fanIn` marks the one exception, and `GraphDescription::validate()` honours it. The
running graph keeps its rule: at build time **every wire into a fan-in port is given a node of its own to end
in** (`NodeCatalog::createTap` - for the bus, a CAN Transmit on its channel), so no stream is merged by a rule
nobody chose, and each module's transmissions are counted separately in Statistics. `rx` is a CAN Channel,
read. A bus with six ECUs on it builds into exactly the graph that six pairs of CAN Channel and CAN Transmit
blocks used to be drawn as.

**A limited wire passes through a filter.** `GraphDescription::build()` puts the Frame Filter's node on a wire
whose `frameFormat` is not "all", labelled with the wire (`ecu to decoder (29-bit only)`), so what a wire says
it carries is checked, counted and reported exactly as if the block had been drawn in.

**A block says whether it speaks on a bus.** `NodeTypeInfo::talksOnBus` (Lua ECU, Test Sequence, TinyML
ECU, UDS Client, ISO-TP Transport). The default is "listens only", the safe way round: a block that forgets
to declare it is heard from less, never more.

### In a project file

```json
{ "from": "can_1", "fromPort": 0, "to": "j1939_decoder", "toPort": 0, "frames": "29-bit" }
```

`frames` is `"11-bit"` or `"29-bit"` and is written only when a wire is limited, so a project that never
limits one is what it was. A value this version does not know is refused rather than read as "every frame": a
wire a newer version limited would otherwise open carrying more than it was drawn to. No format version
changes - a project with a `can.bus` in it opens in an older build as a block of an unknown type, which is
what a new type always was.

## How it is drawn

QtNodes' own geometry and painters are made for blocks with a title bar and a label on each side of them, joined
by curves, so the canvas replaces the three (`src/ui/canvas/CanvasPainters.h`):

| | |
|---|---|
| `PipelineNodeGeometry` | Sizes come from the model. Ports are on the top and the bottom edge. A bus is one port wide *while a wire is being dragged*. |
| `PipelineNodePainter` | Chips, cards and the rail. Everything static. |
| `PipelineConnectionPainter` | A drop to a bus (two wires), or a curve. The pill. |

and everything they share is written once in `CanvasLayout.h` - the size of a chip, where its pencil is - because
the model, the geometry, the painters and the tests all have to agree on every number there.

**A drop is two wires.** `layout::dropWire()` in `CanvasLayout.h` is a pure function of where the module and the
bus are - which is why it is tested for every way they can be placed (over, under, straight, stepped to either
side) without a window - and `wireShape()` turns what it says into paths:

- CAN_H is the wire on the left and goes to the upper wire of the bus; CAN_L is on the right and goes to the
  lower. They are `kDropWireOffset` either side of the middle of the chip, which is the bus's own spacing
  (`kRailWireOffset`), so the drop reads as the same pair.
- Whichever has the further to go passes the near wire of the bus on its way, under it: connections are drawn
  below blocks, and the bus is a block. A module over the bus and one under it at the same x make two straight
  lines through the bus that are joined where the dots are.
- A module that is not over the part of the bus that has wires on it steps sideways half way, and the two wires
  step together - the one on the inside of the turn turns first - so they are the same distance apart all the
  way and never touch.
- The shape a drop is picked up by (`getPainterStroke`) takes in both wires and the gap between them, and the
  frame-format pill. `WireShape::path` is still the middle of the pair, which is what the arrows and the pill sit
  on; `WireShape::wires` are the two wires.
- The two halves of a drop are two items on the one pair of wires, and the half that carries the pill has to be
  painted last or the other's wires are drawn across it. Which half was created first depends on which end
  somebody dragged from (or on the order of a project file), so `CanvasScene` makes the order: what the bus
  delivers to the module is raised to `kDeliveredToModuleZ`, one step above what the module sends and still
  below every block.

**What is drawn once, and what moves.** QtNodes keeps each block as a picture (`DeviceCoordinateCache`) and draws it
again only when it changes. So a chip is painted when its own line changes - `CanvasPanel::refreshChips()` compares
what each one would say with what it said, on every status update - and not every frame. What moves is drawn by
the scene foreground, which now draws only that: the packets running along the curves, a light down each of the
two wires of every drop (in the colour of the wire, side by side), a light travelling along each wire of the bus,
and a halo on each running block. The canvas used to repaint every card,
every port badge and every wire's glow, with fonts and gradients, 25 times a second: on the J1939 example in an
optimised build a frame cost 7.7 ms idle and 9.4 ms running, and cost 0.7 and 2 ms after that - and, with each
drop drawn as two wires, 0.7 and about 2.3 ms (a second glow and a second light on every drop while running).

The consequence worth knowing when changing it: anything that changes how a block looks *from outside* - the theme,
a measurement starting, a database loaded - has to say so (`CanvasPanel::invalidateItems()`), because the picture
QtNodes holds is not asking.

### What QtNodes does that is not in its documentation

| Found | So |
|---|---|
| A dragged wire lets go of the mouse (`ungrabMouse`) *before* it asks the node where it landed (`checkPortHit`), so the mouse grabber cannot tell a drop from a press. | The geometry looks for the draft connection among the scene's items instead - it exists until the drag is settled, which is after the question. |
| A connection's drawing area is worked out from its two ends and its control points, plus `PointDiameter` of margin, and nothing painted outside it is repainted. | A drop to a bus is a pair of lines between a module and the middle of the rail, and its pill sits off them; `PointDiameter` is 26 so that it is inside. `CanvasBusTests` checks it. |
| `QGraphicsItem::update()` on a cached item is what invalidates the cache; `QGraphicsScene::update()` is not. | `invalidateItems()`. |
| `setOrientation(Vertical)` also replaces the geometry. | Set first, and the canvas's geometry after it. |

## Tests

| Suite | What it pins |
|---|---|
| `tests/unit/CanBusTests.cpp` | The bus runs: ECUs on it answer and transmit, every wire into it is a CAN Transmit, a limited wire is a filter and the one that is not limited hears more, a switched-off bus silences what is on it, grouping puts one bus where the channel was and the grouped project runs frame for frame as it did. |
| `tests/unit/ProjectFileTests.cpp` | `frames` round-trips, is absent when not used, and an unknown one is refused. |
| `tests/ui/CanvasBusTests.cpp` | The model (one drop is two wires and is wired and deleted as one, a filter does not speak, the rail follows its modules) and the canvas through the widgets a person has - the palette, the ports, the chevron, the real mouse events of a drag onto the rail. And what a drop looks like: the geometry of its two wires for every placement, the colours they are drawn in on both themes, the shape that picks them up, and the glow along them while running. |
| `tests/ui/CanvasArrangeTests.cpp` | Speakers over the bus and listeners under it, nothing on top of anything, arranging twice changes nothing. |
