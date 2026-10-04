# Getting started

How to get from a clean Windows 11 machine to a running TorqueBus Studio and a
green test suite.

---

## 1. Install the toolchain

| Component | Notes |
|---|---|
| **Visual Studio 2022** | "Desktop development with C++" workload. MSVC x64 is the only supported Windows compiler. The standalone **Build Tools for Visual Studio 2022** — the same workload, without the IDE — are enough, and a much smaller download. |
| **Qt 6.11.2** | Install via the Qt Online Installer. Select `MSVC 2022 64-bit` **and** the **Qt Serial Bus** module — the PEAK backend needs it. |
| **CMake ≥ 3.25** | Bundled with Visual Studio — the *C++ CMake tools for Windows* component of the C++ workload; 17.14 ships 3.31 — or install separately. |
| **Ninja** | Bundled with Visual Studio. |
| **Git** | Needed at configure time: KDDockWidgets and GoogleTest are fetched from GitHub. |

That is everything needed to build, run and test. KDDockWidgets and GoogleTest
are built from source into your build tree on the first configure.

To **open a pull request** you need two more, which building does not:

| Component | Notes |
|---|---|
| **clang-format 21.1.0** | The version CI pins: its formatting check fails on any file this version would change. A different version formats differently, so clean under one is not clean under the other — and the copy that Visual Studio bundles is an older one (19.1.5 in 17.14). Get 21.1.0 from the LLVM releases, or `pip install clang-format==21.1.0`, which is what CI does. |
| **PowerShell 7** (`pwsh`) | Runs `tools\check-before-push.ps1`, the local copy of CI's gates. |

Without clang-format the pre-push script cannot check formatting. It says so in
its verdict rather than passing quietly — but the check that matters is CI's, so
install it.

CMake finds Qt on its own if it is at `C:\Qt\6.11.2\msvc2022_64` (the installer
default) or if `QTDIR` is set. Anywhere else, point at it:

```powershell
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.11.2\msvc2022_64"
```

…or pass `-DCMAKE_PREFIX_PATH=...` on the configure line. Qt does **not** need to
be on `PATH`: not to build, not to run the tests, and not to run the executable.
The build copies what the executable needs beside it, and `ctest` runs with the
Qt the tests were built against at the front of its `PATH` — which also means a
different Qt that happens to come first, as STM32CubeProgrammer's does, cannot
win. (Running a test executable by hand, outside `ctest`, is the one case that
still needs Qt on `PATH`, or the DLLs beside it.)

> **If you use a Qt prompt, it is not enough on its own.** `qtenv2.bat` sets
> `QTDIR` and `PATH` and nothing else; it does not set up MSVC, which it tells
> you on startup. Run `tools\torquebus-prompt.bat` instead — it runs
> `vcvars64.bat` and then `qtenv2.bat` — or just use the `windows-msvc-vs`
> preset, which needs neither. See Troubleshooting if you have already hit the
> `ld.exe: cannot find /nologo` wall.

### Check what you have

Before the first configure, a minute here saves the first failure. Each line says
what it should print.

```powershell
# Visual Studio (or Build Tools) with the C++ toolset. No output means it is missing.
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName

# Qt 6.11.2 for MSVC 2022 64-bit, and its Serial Bus module. Both must say True.
$qt = if ($env:QTDIR) { $env:QTDIR } else { "C:\Qt\6.11.2\msvc2022_64" }
Test-Path "$qt\lib\cmake\Qt6\Qt6Config.cmake"
Test-Path "$qt\lib\cmake\Qt6SerialBus\Qt6SerialBusConfig.cmake"

# Tools. CMake must say 3.25 or newer.
cmake --version
ninja --version
git --version
```

And, if you will contribute:

```powershell
clang-format --version     # clang-format version 21.1.0
pwsh --version             # PowerShell 7.x
```

`cmake --version` answers for the **first** CMake on `PATH`, which is not always
the one you installed — STM32CubeCLT, for one, brings its own. `where.exe cmake`
lists them all, in the order they win. The same goes for `clang-format` and
`ninja`.

---

## 2. Build

From **any** command prompt:

```powershell
git clone https://github.com/ar2labs/torquebus.git
cd torquebus

cmake --preset windows-msvc-vs
cmake --build --preset windows-msvc-vs
```

The first configure takes a few minutes while KDDockWidgets and GoogleTest are
cloned and built. Subsequent configures are fast.

Result: `build/windows-msvc-vs/bin/Debug/TorqueBusStudio.exe`

### Presets

| Preset | Generator | Needs a developer prompt? | What it is for |
|---|---|---|---|
| `windows-msvc-vs` | Visual Studio 2022 | **No** | Start here. The VS generator finds MSVC itself, and no other compiler on `PATH` can hijack it. Multi-config — pick `Debug` or `RelWithDebInfo` at build time. |
| `windows-msvc-debug` | Ninja | **Yes** | Faster incremental builds, once you are set up. |
| `windows-msvc-release` | Ninja | **Yes** | RelWithDebInfo, for performance work. |
| `windows-msvc-strict` | Ninja | **Yes** | Warnings as errors — what CI runs. Use before opening a PR. |

The Ninja presets pin `cl.exe`, which exists only inside the Visual Studio
developer environment. Run them from an **x64 Native Tools Command Prompt for
VS 2022**, or use `windows-msvc-vs`, which has no such requirement.

Every preset has a **build** preset and a **test** preset of the same name
(`windows-msvc-vs` also has a `windows-msvc-vs-release` pair, for
`RelWithDebInfo`). Configure, build and test with the same name: each one has its
own build directory, and the test preset is what knows which.

**Why MSVC is pinned at all.** Qt for Windows is built with MSVC. If CMake
picks up some other compiler that happens to be first on your `PATH` — an
installed LLVM is the usual one — you get an ABI mismatch against Qt and a
local build that CI never reproduces. TorqueBus fails the configure with an
explanatory message rather than letting that happen quietly.

---

## 3. Run the tests

```powershell
ctest --preset windows-msvc-vs
```

The test preset with the **same name** as the one you built, because that is
what points `ctest` at the right build directory. If that directory was never
built — say, you built `windows-msvc-vs` and ran `windows-msvc-debug` — the run
fails with `No tests were found!!!` and a non-zero exit code, rather than
passing with nothing run.

Or directly. The Visual Studio generator builds several configurations side by
side, so `ctest` has to be told which one:

```powershell
ctest --test-dir build/windows-msvc-vs -C Debug --output-on-failure
```

A Ninja preset has one configuration, so there is no `-C`:
`ctest --test-dir build/windows-msvc-debug --output-on-failure`.

Tests that need a physical CAN adapter carry the `hardware` label and are
excluded by default. To run only the integration level:

```powershell
ctest --test-dir build/windows-msvc-vs -C Debug --label-regex integration
```

**No CAN hardware is required.** The built-in virtual bus covers the whole
pipeline, which is exactly why it was built first.

---

## 4. First run

Launch the executable. You should see:

- The **Project Explorer** on the left, listing *TorqueBus Virtual CAN 0* and
  *1* under Hardware.
- The **Properties** panel on the right — click a channel to inspect its
  capabilities.
- The analysis panels tabbed in the centre, each naming the milestone that
  fills it in.
- The **Output** panel at the bottom, reporting which backends are available.

Things worth trying immediately, because they are what v0.1 and v0.2 actually
deliver:

- **F5 starts a measurement** on the virtual bus, **F6 stops it**. The status
  bar comes alive: frame count, frames per second, bus load, and per-channel
  state. No hardware needed.
- **Ctrl+Shift+T** toggles Dark / Light.
- Drag a panel out of the window — it becomes a floating window you can move to
  another monitor.
- Drop one panel onto another to tab them together.
- Close and reopen the application: the arrangement comes back.
- **View → Reset Window Layout** if you lose a panel somewhere unreachable.
  From the command line, `TorqueBusStudio.exe --reset-layout` does the same.

---

## 5. Where state lives

| | Path |
|---|---|
| Settings, theme, window layout | `%APPDATA%\TorqueBus\TorqueBus Studio\settings.json` |

It is plain, indented JSON — readable, diffable, and safe to delete if you want
to start from a clean slate.

---

## 6. Optional: real hardware

| Interface | Install | Then |
|---|---|---|
| Kvaser | Kvaser Drivers for Windows + CANlib SDK | Reconfigure CMake. If CANlib is in a non-standard place, pass `-DKVASER_CANLIB_DIR=<path>`. |
| PEAK-System | PCAN device driver + PCAN-Basic (`PCANBasic.dll`) | No reconfigure needed — the Qt SerialBus plugin loads the DLL at runtime. |

The Kvaser Windows installer creates two virtual channels, which are the
intended target of the v0.3 acceptance test:
*Kvaser Virtual 0 → TorqueBus → Kvaser Virtual 1*.

A backend whose SDK is missing reports itself as unavailable in the Output
panel and appears greyed out. It never breaks the build or the launch.

---

## 7. Before opening a pull request

```powershell
cmake --preset windows-msvc-strict
cmake --build --preset windows-msvc-strict     # warnings are errors here
ctest --preset windows-msvc-strict
clang-format -i <the files you touched>          # 21.1.0 - see section 1
```

Then read [`../ARCHITECTURE.md`](../ARCHITECTURE.md) and
[`../../CONTRIBUTING.md`](../../CONTRIBUTING.md). Most review feedback on this
project is about layering, not style.

---

## Checking it works

Building is not the same as working. The acceptance pass - what each panel
should look like, what the example pipeline should do, and what to send when it
does not - is in [`validation.md`](validation.md).

## Troubleshooting

**`Could not find executable torquebus_unit_tests_NOT_BUILT-<hash>`** — the
tests have not been built yet. Nothing is wrong with the code.

`gtest_discover_tests` asks the test executable for its list of cases, which it
cannot do until that executable exists; `ctest` does it when it starts. If the
executable is not there, it registers one placeholder test per target, named
`<target>_NOT_BUILT`, whose only job is to fail so that a `ctest` run on an
unbuilt tree does not report success.

```bat
cmake --build --preset windows-msvc-debug
ctest  --preset windows-msvc-debug
```

**`Could not read presets from … Unrecognized "version" field`**, or **`CMake 3.25
or higher is required`** — the CMake that answered is older than 3.25. The presets
file is schema version 6, which arrived in 3.25, and an older CMake does not
recognise the number.

It is usually not that you have no recent CMake, but that an older one comes
first on `PATH`:

```powershell
where.exe cmake      # the first line is the one that runs
cmake --version
```

STM32CubeCLT brings its own CMake and Ninja, and puts them on `PATH`. Put the
newer one first, or run it by its full path — Visual Studio's is under
`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` in the installation
directory that `vswhere` reports. The developer prompt does not fix it: it adds
that directory at the *end* of `PATH`, after whatever was already winning.

**`Could NOT find Qt6`** — `CMAKE_PREFIX_PATH` does not point at your Qt
installation, or you installed the MinGW build instead of MSVC 2022 64-bit.

**`Could NOT find Qt6SerialBus`** — the Qt Serial Bus module was not selected
in the Qt installer. Re-run the Qt Maintenance Tool and add it.

**`The CMAKE_CXX_COMPILER: cl is not a full path and was not found in the
PATH`** — you ran a Ninja preset from a plain command prompt. `cl.exe` lives
only inside the Visual Studio developer environment. Either use
`cmake --preset windows-msvc-vs`, which needs no developer prompt, or reopen
the shell as an *x64 Native Tools Command Prompt for VS 2022*.

**`No CMAKE_CXX_COMPILER could be found` / Visual Studio generator not
found** — Visual Studio 2022 (or the standalone **Build Tools for Visual
Studio 2022**, which is a much smaller download) is not installed. Qt for
Windows is built with MSVC, so an MSVC-ABI toolchain is required. Check what
you have with:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property displayName
```

No output means no Visual Studio and no Build Tools.

**`TorqueBus Studio targets MSVC x64 on Windows, but CMake selected Clang…`** —
another compiler comes first on your `PATH` (an LLVM install is the usual
culprit). Use `cmake --preset windows-msvc-vs`. Delete the stale `build/`
directory first — CMake caches the compiler it picked and will not change its
mind otherwise.

**`fatal error C1083: cannot open include file: 'string'`** (or `'array'`, or
any other standard header) — the same missing-vcvars problem as the `ld.exe`
entry below, one step further along.

The tell is the compile command line: the only `/I` flags are the project's own
directories. There is no path to the CRT or the Windows SDK, because those never
appear as flags - they come from the `INCLUDE` environment variable, which
`vcvars64.bat` sets and a plain prompt does not.

**Configure succeeds anyway**, which is what makes this confusing. CMake's
compiler check is cached from the last successful configure, so a stale build
tree reports `Compiler .............. MSVC 19.x` and a full, healthy summary
block, and then every translation unit fails on the first `#include`. The
summary is describing what was true when the cache was written.

The other tell is *which* files fail: all of them, including ones nobody has
edited. A change you just made cannot break `#include <string>` in a file you
did not touch.

```bat
tools\torquebus-prompt.bat
cmake --build --preset windows-msvc-debug
```

No need to delete `build/` - nothing wrong was written to the cache, there
simply are no object files yet.

**`ld.exe: cannot find /nologo`, `ld.exe: cannot find kernel32.lib`** — nine
times out of ten this is a prompt started from `qtenv2.bat` alone.

`qtenv2.bat` sets `QTDIR` and puts Qt on `PATH`. That is all it does — it prints
*"Remember to call vcvarsall.bat to complete environment setup!"* on startup,
and that line is very easy to scroll past. Without vcvars there is no `INCLUDE`,
no `LIB`, and no MSVC linker ahead of whatever else is on `PATH`, so CMake finds
`cl.exe` (usually still there from a system-wide entry), compiles its test file,
and then links it with the first `ld.exe` it can find.

Use the prompt that has both halves:

```bat
tools\torquebus-prompt.bat
```

It locates Visual Studio with `vswhere`, runs `vcvars64.bat` and then
`qtenv2.bat`, in that order, and drops you in the repository root. Or do it by
hand:

```bat
cmd /A /Q /K "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" ^
  && "C:\Qt\6.11.2\msvc2022_64\bin\qtenv2.bat"
```

The `windows-msvc-vs` preset needs none of this: the Visual Studio generator
brings its own toolchain, and CMake now finds Qt from `QTDIR` or from
`C:/Qt/6.11.2/msvc2022_64` without help from `PATH`.

If that was not it, the cause is one of the two below. The tell either way is
the linker path in the failing command line — `ld.exe` rather than `link.exe`:

```
-- Check for working CXX compiler: .../cl.exe - broken
    ... -- C:\Tools\gcc-v14.2.0\bin\ld.exe /nologo ...
                                   ^^^^^^
```

1. **A stale build directory.** `CMAKE_LINKER` is cached separately from the
   compiler, so a directory configured once with a GNU toolchain on `PATH` keeps
   the GNU linker even when the compiler is re-detected as MSVC — which is why
   the log can say "compiler identification is MSVC" and still link with `ld`.
   Delete it and configure again:

   ```bat
   rmdir /s /q build\windows-msvc-debug
   cmake --preset windows-msvc-debug
   ```

2. **A GNU toolchain ahead of Visual Studio on `PATH`.** An embedded toolchain
   will do it — STM32CubeCLT ships its own CMake, Ninja and GCC. Check with
   `where link` and `where ld`. Either fix the `PATH`, or sidestep it entirely:

   ```bat
   cmake --preset windows-msvc-vs
   ```

   The Visual Studio generator brings its own toolchain and ignores `PATH`,
   which is why it is the recommended preset.

The Ninja presets now pin `CMAKE_LINKER` to `link` for the same reason they pin
`cl` — but a pin only applies to a directory being configured for the first
time, so it does not rescue a cache that already holds the wrong value.

**`ar.exe: invalid option -- /`, while linking a static library** — the same
problem one tool further along, and it hid for longer. `CMAKE_AR` is what builds
a static library, it is cached separately from both the compiler and the linker,
and it was not pinned until this was hit. An embedded GNU toolchain on `PATH`
supplies an `ar.exe`, CMake finds it, and then hands it MSVC's `/nologo` and
`/out:`.

It fails later than the linker problem, which is what makes it confusing: the
archiver only runs when a static library actually needs rebuilding. A build
directory can be green for days — every incremental build reusing the `.lib`
that was already there — and then break on the first change that touches a
vendored source. Almost everything here is a static library, so this is the
expensive one to get wrong.

The presets pin `CMAKE_AR` to `lib` now. Check what a directory actually has
before blaming anything else:

```powershell
findstr /C:"CMAKE_AR:" build\windows-msvc-strict\CMakeCache.txt
```

If it does not say `lib`, delete the directory. Reconfiguring keeps the cached
value.

**`Target "kddockwidgets" links to Qt6::WidgetsPrivate but the target was not
found`** — you are on a build tree configured before this was fixed. KDDockWidgets
links Qt's private targets without requesting them, so TorqueBus requests them
on its behalf in `cmake/TorqueBusDependencies.cmake`. Delete `build/` and
reconfigure.

**FetchContent fails to clone** — configure needs network access on the first
run. Behind a proxy, set `HTTPS_PROXY` before configuring.

**The window opens with no panels** — a saved layout from an older build.
Run with `--reset-layout`.

**`windeployqt` is not recognised** (when packaging) — add
`C:\Qt\6.11.2\msvc2022_64\bin` to your `PATH`.
