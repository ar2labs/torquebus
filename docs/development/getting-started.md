# Getting started

How to get from a clean Windows 11 machine to a running TorqueBus Studio and a
green test suite.

---

## 1. Install the toolchain

| Component | Notes |
|---|---|
| **Visual Studio 2022** | "Desktop development with C++" workload. MSVC x64 is the only supported Windows compiler. |
| **Qt 6.11.2** | Install via the Qt Online Installer. Select `MSVC 2022 64-bit` **and** the **Qt Serial Bus** module — the PEAK backend needs it. |
| **CMake ≥ 3.24** | Bundled with Visual Studio, or install separately. |
| **Ninja** | Bundled with Visual Studio. |
| **Git** | Needed at configure time: KDDockWidgets and Catch2 are fetched from GitHub. |

Nothing else needs installing. KDDockWidgets and Catch2 are built from source
into your build tree on the first configure.

CMake finds Qt on its own if it is at `C:\Qt\6.11.2\msvc2022_64` (the installer
default) or if `QTDIR` is set. Anywhere else, point at it:

```powershell
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.11.2\msvc2022_64"
```

…or pass `-DCMAKE_PREFIX_PATH=...` on the configure line. Qt does **not** need to
be on `PATH` to build — only to run the executable, which the presets handle.

> **If you use a Qt prompt, it is not enough on its own.** `qtenv2.bat` sets
> `QTDIR` and `PATH` and nothing else; it does not set up MSVC, which it tells
> you on startup. Run `tools\torquebus-prompt.bat` instead — it runs
> `vcvars64.bat` and then `qtenv2.bat` — or just use the `windows-msvc-vs`
> preset, which needs neither. See Troubleshooting if you have already hit the
> `ld.exe: cannot find /nologo` wall.

---

## 2. Build

From **any** command prompt:

```powershell
git clone https://github.com/ar2labs/torquebus.git
cd torquebus

cmake --preset windows-msvc-vs
cmake --build --preset windows-msvc-vs
```

The first configure takes a few minutes while KDDockWidgets and Catch2 are
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

**Why MSVC is pinned at all.** Qt for Windows is built with MSVC. If CMake
picks up some other compiler that happens to be first on your `PATH` — an
installed LLVM is the usual one — you get an ABI mismatch against Qt and a
local build that CI never reproduces. TorqueBus fails the configure with an
explanatory message rather than letting that happen quietly.

---

## 3. Run the tests

```powershell
ctest --preset windows-msvc-debug
```

Or directly:

```powershell
ctest --test-dir build/windows-msvc-debug --output-on-failure
```

Tests that need a physical CAN adapter carry the `hardware` label and are
excluded by default. To run only the integration level:

```powershell
ctest --test-dir build/windows-msvc-debug --label-regex integration
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
ctest --preset windows-msvc-debug
clang-format -i <the files you touched>
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
