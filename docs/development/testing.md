# Test sequences

A simulation that runs answers one question: does anything crash. The question
worth asking is whether the **network behaved** — whether the answer came within
200 ms, whether the counter incremented, whether the lamp stayed out. A person
watching a trace can see all of that, and cannot see it a hundred times in a
row, which is exactly when it matters.

A **Test Sequence** block is a Lua script that checks and reports:

```lua
test("The engine message appears and is plausible", function()
    local frame = expect_frame(0x100, { within = 200 })
    assert_equal(#frame.data, 8, "DLC")
end)

test("The lamp goes out once the fault is cleared", function()
    send(0x7E0, "\x04\x14\xFF\xFF\xFF")
    wait(100)
    expect_silence(0x300, 500)
end)
```

Drop the block on the canvas, wire the channel into it and its output to a
transmit block, press **Start**. The **Test** panel comes forward and fills in
as each case finishes — a run is watched at least as often as it is read
afterwards, and somebody watching an eight-minute sequence should be able to see
it failing at case three and stop.

The panel shows the summary first, because that is the whole answer, and then
one row per case; opening a row shows the checks it made. Failing cases open
themselves. **Export...** writes the run as Markdown, for a ticket or a
repository — a verdict that cannot leave the window is a verdict nobody else can
act on. Every result also goes to the Output panel, beside the frames that
caused it.

The verdict stays on screen after Stop. That is the moment it is actually read.

**A sequence observes and reports. It does not stop the measurement**, fail a
recording, or change what any other node sees. A test that could halt the run it
is measuring would make every failure ambiguous — did the network do that, or
did the test?

## How it can be written in the order things happen

Each case runs in its own Lua coroutine. `expect` and `wait` yield; the node
resumes the case on its next pass. So a sequence reads top to bottom while the
executor still returns immediately, and neither the trace nor any other block
notices that a test is running.

One case runs at a time, in the order they were declared. A network is a shared
thing, and two cases driving it at once would make each one's result depend on
the other's timing — which is the property a test exists not to have.

## Declaring cases

```lua
test(name, function() ... end)
```

Called at the top level of the file. The cases are collected when the sequence
loads, which is why a broken sequence fails **at Start** rather than three
seconds into a run: finding out that a test file has a typo after setting up the
bench is the wrong moment.

A file that declares no cases says so, in the error colour. `0 of 0 passed` is
the most dangerous sentence a test report can print.

## Waiting for the bus

| Call | What it does |
|---|---|
| `expect(id, options)` | Waits for a frame and **returns** it, or `nil` if none came. Silence is a fact about the network, not an error — a case is entitled to be testing for it |
| `expect_frame(id, options)` | `expect` plus the assertion nearly every use of it is followed by: fails the case if nothing arrived |
| `expect_silence(id, ms, description)` | Fails if anything arrives on `id` within `ms`. The assertion most tools cannot express, and the one fault injection exists to provoke |
| `wait(ms)` | Lets the measurement run |

`options`:

- `within` — timeout in milliseconds, default 1000.
- `channel` — only frames on that channel count.
- `where` — a function taking the frame and returning true to accept it.

A frame is a table: `id`, `data` (a byte string), `channel`, `extended`,
`timestamp_us`.

```lua
local frame = expect(0x100, { within = 300, where = function(f)
    return f.data:byte(1) == 0x42
end })
```

The deadline belongs to the **wait**, not to each candidate frame. A filter that
reset the clock on every frame it rejected would turn a busy bus into a wait
that never ends — the test would hang rather than fail, which is the worse of
the two.

### One pass of latency

A frame that arrives in the same pass as the `expect` asking for it is matched
on the *next* pass — one dispatch interval, 5 ms by default. So a sequence
measuring a response time reads a number that can be up to one pass late. This
is written down rather than pretended away, and it is the second reason every
timing assertion in a sequence should be a range.

## Checks

| Call | Fails when |
|---|---|
| `assert_true(condition, description)` | the condition is false or nil |
| `assert_equal(actual, expected, description)` | they differ |
| `assert_near(actual, expected, tolerance, description)` | they differ by more than the tolerance |
| `assert_between(actual, low, high, description)` | outside the range, inclusive |
| `fail(message)` | always — for the branch that should not have been reached |

Prefer a range to a number for anything measured. A cycle time held to the
microsecond is a simulation; a test that demands one fails on real hardware for
reasons that have nothing to do with the ECU.

**Every check is recorded, passed or failed.** A report listing only failures
cannot be read as evidence that anything was checked, and "17 checks, all
passed" is the sentence somebody signs off on.

A failing check **ends its case and no other**. One requirement not being met
says nothing about the others, and a run that stopped at the first failure would
have to be repeated once per bug.

## Sending

```lua
send(id, data, options)          -- options: { extended = , fd = , brs = }
```

The same frame the Lua ECU's `emit` builds, on this node's output port. Where it
goes is the graph's business.

Because the pipeline refuses cycles, a sequence that **sends and then waits for
the answer** needs its input to come from the channel rather than from the block
it is talking to. That is the ordinary arrangement anyway:

```text
CAN Channel ──┬──> Test Sequence ──> CAN Transmit
              └──> Lua ECU        ──> CAN Transmit
```

## Also available

`log(text)` writes a line to the Output panel. `get_time_us()` and the whole
`tb` prelude from [scripting.md](scripting.md) — `tb.now()`, `tb.crc8`, `tb.e2e`
and the generators — are here too, because a test that has to build a *valid*
request needs the same arithmetic an ECU needs to answer one.

`parameters` holds whatever the block was given, so one sequence with
`engine_id` and `tester_id` as parameters is a sequence for four ECUs rather
than four copies of a file.

## Three outcomes, not two

| Outcome | Means |
|---|---|
| **Passed** | every check in the case passed |
| **Failed** | a check failed — something about the network under test |
| **Errored** | the script itself threw — a nil index, a bad argument: something about the test |

They are separate because they need different people. A tool that files a nil
index as a failure sends somebody to the bench to debug the test harness.

A run stopped part-way marks the case that was in flight as **Errored** rather
than dropping it. It did not pass — it did not finish — and the dropped case
would be exactly the one somebody stopped the measurement to look at.

## What a sequence cannot do

- It cannot stop, pause or fail the measurement.
- It cannot read the trace store (`bus_last`, `bus_stats`): a test asks about
  what is happening now, and a test that reached into the history could pass on
  evidence from before it started.
- There is no `require` and no file access, the same sandbox every script here
  gets.

## Example

`examples/scripts/sequence_engine.lua` — six cases against an engine ECU: the
message exists, keeps its cycle time, has the right length, its counter
increments, the ECU answers a VIN request, and the lamp stays out.
