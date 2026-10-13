# Robustness: what happens when something is wrong

TorqueBus runs code that its authors did not write (a Lua ECU, a plugin's block), reads files that other tools
wrote (`.dbc`, `.tblog`, `.tbusml`, `.tbsproj`) and talks to hardware through vendor SDKs. Each of those can
be wrong, and the question this document answers is the same for each: **how far does the failure travel?**
The rule the project holds itself to is *not past the thing that failed* - and where it does, there is a
sentence on the Output panel with the block, the file or the line in it, never a frozen window and never a
process that is simply gone.

This is the map of where that rule is kept, how it is tested, and - because a claim without its limits is
worth little - where it is not kept.

---

## 1. The layers

| What goes wrong | What happens | Where |
| --- | --- | --- |
| A script loops forever | The call fails after its budget (250 ms; the top of a script, 2 s) with the line and the limit in the message. An ECU that ran out of time is taken out of the run at once. | `LuaRuntime` (`LuaLimits`, a count hook) |
| A script allocates without end | The allocation is refused at 64 MiB and the script gets "not enough memory" with its own line number. The interpreter is still usable. | `LuaRuntime::Allocator` |
| A script declares timers without end | `every()`/`cyclic()` fail past 1024 per ECU. | `LuaEcuNode` |
| A script passes a value the bindings cannot take | A Lua error. The bindings are fuzzed with every kind of value in every position. | `InputFuzzTests` |
| A block throws from `process()` | The block is taken out of the run, reported once with its name and the reason, and publishes nothing from then on. Everything it was not feeding carries on. It is still told the run is over. | `PipelineGraph::execute()` |
| A block throws from `prepare()` or `finish()` | `prepare()`: the compile fails naming the block, and the blocks before it are finished. `finish()`: reported, and the others still finish. | `PipelineGraph::compile()`, `finish()` |
| A creator or a validator throws (a plugin's, say) | A `Result` that names the block. Start fails; the canvas goes on. | `NodeCatalog::create()`, `validateParameters()` |
| Building the pipeline throws | Start fails with the reason. | `CanEngine::start()` |
| Something outside a block throws on the dispatch thread | The measurement ends, reported, and the thread is joined later. This is the last net, and it is the old one. | `CanEngine`, `ThreadGuard.h` |
| A setting is of the wrong kind or out of range | The block is refused while it is on screen, in the words of the form. Nothing is cast down to a value that fits. | `ParameterDescriptor`, `NodeCatalog::validateParameters()` |
| A `.dbc` is damaged, huge, in another encoding | A parse error with the line, the message and the number; or it is read (Windows-1252, a byte order mark, the CANdb++ container message, CAN FD). | `DbcParser` |
| A `.tbsproj` is damaged or hand-edited | Refused with a message, leaving the open project as it was; or kept as it can be (a number too big for an integer stays a real). | `ProjectFile`, `GraphDescription::validate()` |
| A `.tblog` or `.tbusml` is damaged | The log is read to its damage and says how many bytes it could not; a model whose sizes cannot be believed is refused before anything is read from it. | `TraceLogReader`, `TinyMlModel` |
| A frame arrives that claims more bytes than it holds | Held to the array at the channel, before any node reads it; and again by the blocks that read payloads and by the trace store, which are also fed by other blocks. | `CanChannel::drain()`, `CanFrame::payloadSize()`, `TraceStore::append()` |
| A frame is sent that cannot exist | Refused at the channel with the reason; the driver never sees it. | `CanChannel::transmit()` |
| A graph would repeat every frame forever | A bus wired to itself, or to another bus on the same channel, is refused by `validate()`. | `GraphDescription::validate()` |
| A hardware handle is text that is not a number | The backend says it is not one of its own (`DeviceNotFound`); nothing throws out of `open()`. | `VirtualCanBackend`, `KvaserCanBackend` |
| A library the canvas is built on reads the wrong object | QtNodes 3.0.16 read a view's transform out of the viewport on every mouse move of a dragged wire. Corrected in its source at configure time. | `cmake/PatchQtNodes.cmake` |
| A path has accents in it | Converted from UTF-8 where the core opens a file, not read in the ANSI code page. | `core/FilePath.h` |

Two things hold all of this together and are worth saying once:

* **`validate()` and `build()` agree.** Whatever one rejects the other rejects, and a disabled block is not
  held against the graph by either. The canvas shows what `validate()` says while a block is on screen.
* **A failure is reported where the person is looking.** The Output panel, a message on the block, the status
  of the Start button. Counting a failure without saying so (the old way a rejected frame was lost) is a bug
  of the same family.

---

## 2. What is *not* covered

* **A script that catches the time limit and loops again.** The limit is an error raised inside the script, so
  `while true do pcall(function() while true do end end) end` catches it every time. The per-call deadline is
  cooperative, not a kill; defending against a script written to defeat it would take a separate process.
* **Work that happens inside one C function.** The count hook runs between Lua instructions. A pattern with
  catastrophic backtracking in `string.find`, or a very large `table.sort`, runs inside the C library where
  no hook fires. The memory limit still applies; the time limit cannot.
* **Blocking I/O.** A log sink writing to a network drive that stops answering, a vendor SDK call that does not
  return, a frame sink callback that waits for the window. The dispatch thread is one thread, and Stop joins it.
* **Hardware.** The vendor backends live outside this repository's tests; the channel validates what it hands
  them, and that is the extent of it.
* **Semantic mistakes in a database.** A signal that overlaps another, or sits past the end of its message, is
  read as written. The shipped databases are checked for it (`ShippedDatabaseTests`); a person's are not.

---

## 3. How it is tested

| Suite | What it holds |
| --- | --- |
| `PipelineFaultTests` | A node that throws is quarantined, once, with its name; the others carry on; `prepare()` and `finish()` failures. |
| `LuaLimitsTests` | Endless loops (in a handler, at the top, in a coroutine), a growing table, the timer cap, and the identifiers that used to end the application. |
| `ParameterValidationTests` | Every kind of wrong setting, and a creator, validator or tap that throws. |
| `DbcRobustnessTests` | The files that actually arrive: the container message, CAN FD, Windows-1252, a byte order mark, numbers that are not numbers, a very large database. |
| `FilePathTests` | Folders and file names with accents and CJK characters through every place the core opens a file. |
| `CanChannelRobustnessTests` | A driver that lies about a frame's length; the frames the channel will not send. |
| `ProjectFileTests` (the end of the file) | What a hand edit can put in a project. |
| `CanvasRobustnessTests` | A canvas handed a graph nobody would draw, and one that is large. |
| `ShippedDatabaseTests` | The databases in `examples/databases`. |
| `PipelineBlocksTests` | **Every block in the catalog**, built, wired, started, fed traffic and stopped - and a test that fails for a block that has no recipe. |
| `InputFuzzTests` | Seeded, damaged inputs to every reader, every Lua binding called with every kind of value, and the blocks that read frames fed frames that lie about their own length. |

### A crash that happens once in a thousand runs

gtest reports an access violation as a code and an empty stack. The UI tests install `tests/ui/CrashTrace.h`,
which prints the address and the functions that were running, with their lines, to stderr - which ctest keeps
for a failed test. To make a rare crash happen often enough to be caught, run the suite that exercises it many
times at once:

```bat
ctest --test-dir build\windows-msvc-debug -R CanvasBusTest -j 24 --repeat until-fail:150 --output-on-failure
```

That is how the QtNodes crash in the table above was found, after a hundred and fifty rounds of twenty-eight
tests, and how it was shown gone.

### The fuzz test, longer

The default count keeps it to a couple of seconds. `TORQUEBUS_FUZZ_SCALE` multiplies every test's count:

```bat
set TORQUEBUS_FUZZ_SCALE=50
build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_filter=InputFuzzTests.*
```

A read past the end of a buffer is a silent success in an ordinary build. Under **AddressSanitizer** it is a
report, and that is the run worth making before a release. MSVC's is installed with the C++ workload; it needs
a build without the debug runtime checks (they conflict with it), so a separate directory:

```bat
cmake -S . -B build\asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
      -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_LINKER=link -DCMAKE_AR=lib ^
      "-DCMAKE_C_FLAGS=/fsanitize=address" "-DCMAKE_CXX_FLAGS=/fsanitize=address /EHsc" ^
      "-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO" "-DCMAKE_SHARED_LINKER_FLAGS=/INCREMENTAL:NO"
cmake --build build\asan
set ASAN_OPTIONS=detect_leaks=0
ctest --test-dir build\asan -LE hardware
```

(from `tools\torquebus-prompt.bat`, like every configure here). The whole suite runs clean under it, and so
does the fuzz test at a scale of 100.

---

## 4. Writing a block that keeps the rule

* **Declare what a setting is.** A `ParameterDescriptor` has a kind and, for a number, a `minimum` and a
  `maximum`. A narrowing cast of a parameter is a bug: use the declared range, or clamp, and never let a
  value wrap into a different one.
* **Return a `Result` from `prepare()`.** Opening a file, parsing a database and compiling a script belong
  there, and failing there is a message with the block's name in it. If something there can still throw, it is
  contained; do not rely on that for anything you expected.
* **`process()` does not allocate, block, or throw.** If it throws anyway, the block goes quiet and the run
  continues - the contract is still that it does not.
* **Trust a frame's `length` only through `payloadSize()`.** It is a byte that anything can set.
* **A message formatted for Lua** (`luaL_error`, `lua_pushfstring`) uses `%s %d %I %f %p %c %U` and nothing
  else. Format anything more elaborate with `snprintf` into a local array and pass it as `%s`.
* **Add a recipe** to `tests/integration/PipelineBlocksTests.cpp`. The test that walks the catalog fails
  until you do, which is the point.
