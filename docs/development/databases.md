# Databases

How TorqueBus reads a `.dbc`, and the three places a decoder can be wrong
without anyone noticing.

---

## Why this part gets more care than the rest

A decoder that is off by one bit does not crash and does not log. It shows a
plausible number, and the engineer reading it has no way to tell. Every other
component in this project fails loudly; this one fails by lying.

So three rules hold here that do not hold elsewhere:

1. **The implementation is written against the specification, not inferred from
   a working example.** An implementation reverse-engineered from one database
   is correct for that database.
2. **The tests are anchored to vectors somebody else published.** `motohawk.dbc`
   is the example database cantools ships, and its documentation states that
   `Temperature = 250.55, AverageRadius = 3.2, Enable = 1` encodes to
   `C0 06 E0 00 00 00 00 00`. Decoding those bytes has to give those values
   back, and encoding those values has to give those bytes back.
3. **Nothing is silently corrected.** A value out of range, a frame too short, a
   message the database does not describe — each is reported rather than
   smoothed over, because smoothing over a fault hides the fault.

---

## Bit numbering, which is where the mistakes live

A DBC bit number is `byte * 8 + bitInByte`, where bit 0 is the **least**
significant bit of its byte. So bit 8 is the least significant bit of byte 1.

The confusing part is elsewhere: within a byte, the bit that goes on the wire
**first** is bit 7, the most significant. That is what makes Motorola signals
run *downwards* through the numbering.

| | Intel (`@1`) | Motorola (`@0`) |
|---|---|---|
| The start bit is the signal's… | least significant bit | **most** significant bit |
| Bits run… | upwards through the numbering | downwards |

`CanSignal.cpp` handles Motorola by converting to **wire order** — position 0 is
bit 7 of byte 0, position 1 is bit 6, position 8 is bit 7 of byte 1. In that
space the signal is a contiguous run starting at its most significant bit, so
the arithmetic is ordinary addition. Reasoning directly in DBC numbering is
where the off-by-one lives.

### The consequence that surprises everybody

`SG_ Temperature : 0|12@0-` spans **three** bytes.

Start bit 0 is the last bit of byte 0 to go on the wire. The signal runs from
there forwards: one more bit of byte 0, all eight of byte 1, and the first three
of byte 2. Twelve bits, three bytes.

This is pinned by a test, because the first version of that test asserted two
bytes and the implementation was right.

---

## What the parser reads

| Section | Read | Note |
|---|---|---|
| `VERSION` | yes | Informational |
| `BU_` | yes | Node list |
| `BO_` / `SG_` | yes | Including multiplexing and extended identifiers |
| `VAL_` | yes | The value tables that turn a 3 into "Reverse" |
| `CM_` | yes | On a message or a signal |
| `BA_` | `GenMsgCycleTime` only | The rest are skipped |
| Everything else | skipped | `NS_`, `BS_`, `EV_`, `SIG_GROUP_`, … |

**The rule: unknown sections are skipped, malformed known ones are an error.**

A `.dbc` containing a section nothing downstream reads should still load — the
format grows and files move between tools. But an `SG_` line that does not parse
is not a section being ignored: it is a signal that would silently go missing,
and a missing signal is a panel that shows nothing with no explanation. Those
fail the load and name the line.

### Three things about real files

- **`NS_` lists keywords one to a line.** A file declaring it may use `CM_`
  contains a line that is exactly `CM_`. Read as a section, that is a comment
  with no text — and all three databases this project is tested against failed
  at line 7 the first time the parser ran. An `NS_` entry is a bare word alone
  on its line, and no real section is; that shape, not a list of names, is what
  the parser keys on.

- **`/* … */` blocks are not DBC syntax and appear anyway.** Both hand-written
  databases here are full of them. A parser every other tool disagrees with is
  the one that is wrong, so they are stripped — keeping the newlines, so an
  error after a twenty-line comment still names the real line.

- **Numbers go through the classic locale.** A `.dbc` always writes `0.01` with
  a dot. On a machine whose locale uses a comma, the ordinary conversions read
  that as `0`, and every scaled signal on the bus would decode to its offset —
  silently, and only on that machine.

---

## Bit 31 of the identifier is not part of the identifier

A `.dbc` folds the extended flag into the number on the `BO_` line:

```text
BO_ 2566840064 EEC1: 8 ECU     # 0x98FEDF00 = extended | 0x18FEDF00
```

The parser splits it out, so `CanMessage::identifier` compares directly against
a `CanFrame`. A parser that does not produces a message no frame can match — and
it matches nothing, silently.

The format is part of the lookup key too: standard `0x123` and extended `0x123`
are different messages, and a database may define both.

---

## Decoding

```text
CanFrame -> DbcDecoderNode -> DecodedSignal on a Signals edge
```

`DbcDecoderNode` is the first node in the project whose output port is not
`Frames`. That is the point of it: a decoder that emitted frames "with signals
attached" would be the easy shape and the wrong one. What comes out of a DBC
decoder is not CAN traffic, and a plot should not be connectable to a channel
transmit. The graph refuses that wire, and there is a test that it does.

**Measured:** 23 ns per signal, 184 ns for an eight-signal frame, 5.4M frames/s
through a graph. The engine sustains 193k frames/s, so a decoder on the hot path
costs about 3% of the budget — which is the number that says it can sit there
rather than behind a "decode on demand" switch.

### What a DecodedSignal carries, and what it does not

It carries the timestamp, the value, the raw bits, the identifier and the
channel. It does **not** carry its name: a batch is a contiguous non-owning span,
so the payload has to be trivially copyable, and a `std::string` in it would
mean an allocation per signal per frame. It points at its definition instead.

**That makes the database's lifetime part of the contract.** `DbcDecoderNode`
holds its database by `shared_ptr` for exactly this reason: reloading a `.dbc`
mid-measurement swaps in a new one and leaves the old alive until the last batch
referencing it is gone.

### Three cases where the honest answer is not the obvious one

| Case | What happens | Why not the obvious thing |
|---|---|---|
| Frame shorter than the database says | The signal is emitted with `truncated` set and a value of 0 | "The sensor reads zero" and "we could not read the sensor" are a fault in the vehicle and a fault in the setup. A decoder returning only a number destroys the difference |
| Identifier not in the database | Counted, not logged | Most traffic on a real bus is outside any one database. A count that reaches 100% is the tell that the wrong database is loaded |
| Remote or error frame | Skipped | A remote frame has a length and no payload. Decoding it yields a full set of plausible zeroes, which looks exactly like a real reading |

---

## Encoding

`CanSignal::encode` is the inverse, and the test that matters is that composing
the two is the identity on the published vector.

`encodeRaw` ORs its bits in after clearing only its own, rather than writing
whole bytes. On a message where a 4-bit gear code sits beside a 4-bit mode code,
writing bytes means setting one always zeroes the other.

Out of range **saturates and returns false**. Wrapping would turn a torque
request of 300% into -56%, which is the kind of failure that moves an actuator.

The declared `minimum` and `maximum` do **not** clamp, in either direction. Only
the physical width of the field is a hard limit; the database's declared range
is documentation, and silently correcting a wrong value hides the fault that
produced it.

---

## Using it

### In the window

**File → Import Database…** loads a `.dbc` into the **DBC Explorer**, which is
tabbed with the trace. From that moment the trace's **Name** column carries the
message name and the **Signals** column carries the decoded values — including
for frames already captured, because a database imported halfway through a
measurement should explain what has already been seen.

Failures are reported in the Output panel rather than in a message box: the
message names a line number, and a line number is something you want to keep
looking at while you open the file in an editor.

### In a pipeline

Drop a **DBC Decoder** block and set its `database` parameter to a `.dbc` path.

A relative path is resolved against the **project file's own directory**, so a
`.tbsproj` and the databases beside it travel together and open from anywhere.
An absolute path is left exactly as written — somebody who typed one meant it.

Resolution happens when the graph is built, not when the project is read. Doing
it at load would make the stored paths absolute, and the next save would write
them back that way — turning a project that travels with its folder into one
pinned to the machine that last saved it.

The panel and the graph are deliberately separate: a decoder block names its own
file, so a project describes its own decoding rather than depending on what
happens to be open in a panel.

---

## Example databases

`examples/databases/` holds `vehicle.dbc` and `ecu.dbc`, which match what
`examples/scripts/ecu_vehicle.lua` puts on the bus — so importing `vehicle.dbc`
while the example project runs turns the trace from hex into km/h and °C.

---

## A note on `IDatabaseParser`

PLAN.md section 19 asks for the parser to sit behind an interface, so that
ARXML, LDF, FIBEX and A2L can follow. That interface does not exist yet, and
deliberately: an abstraction extracted from one implementation is a guess about
what the second one will need. `DbcParser::parse` is a free-standing static
that fills a `CanDatabase`, which is the shape an interface would wrap — so the
extraction stays cheap when there is a second format to extract it *from*.
