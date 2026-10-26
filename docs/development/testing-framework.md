# Testing and quality assurance

TorqueBus Studio uses Google Test for C++ tests and CTest to discover and run
them. Tests are grouped by what they exercise so contributors can run a focused
subset locally, while CI runs the virtual-bus suites on Windows with both Debug
and Release builds.

Test cases are discovered from the test binaries when CTest runs. Their total
can change as coverage grows, so this guide describes the suites and labels
rather than maintaining a fixed case count.

## Test suites

| Suite | CTest label | What it covers |
| --- | --- | --- |
| Unit | — | Core data structures, protocols, parsers, scripting, and other isolated behavior. |
| UI | `ui` | Widgets, QML, dashboard, pipeline canvas, and docking behavior. |
| Integration | `integration` | End-to-end virtual CAN, pipeline, and ECU scenarios. |
| Hardware | `hardware` | Kvaser and PEAK adapter tests that require vendor drivers or physical interfaces. |

The supplied CTest presets exclude the `hardware` label, as does CI. A direct
CTest invocation with `--test-dir` can include hardware tests; those tests skip
with a reason when a required driver or adapter is unavailable. The virtual bus
keeps unit, UI, and integration coverage available without a physical adapter.

## Run tests locally

Configure and build the matching preset before running CTest. For example:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

Use a preset that matches the build directory you compiled. To run only one
label or tests matching a name, add `-L` or `-R`:

```powershell
ctest --preset windows-msvc-debug -L integration --output-on-failure
ctest --preset windows-msvc-debug -L ui --output-on-failure
ctest --preset windows-msvc-debug -R J1939 --output-on-failure
```

Run hardware tests directly when a suitable driver and adapter are available.
Use `--test-dir` here because the supplied test presets exclude this label:

```powershell
ctest --test-dir build/windows-msvc-debug -L hardware --output-on-failure
```

Google Test binaries are also available under
`build/<preset>/bin/`. For example:

```powershell
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_filter="*J1939*"
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_repeat=10
```

Running a Qt-linked test binary directly may require the matching Qt DLLs and
platform plugins to be available. CTest sets up the test environment for the
configured build; see [Getting Started](getting-started.md) for details.

## Write a Google Test

Follow the Arrange–Act–Assert pattern and keep each test focused on an
observable behavior:

```cpp
#include <cstdint>
#include <gtest/gtest.h>
#include "torquebus/j1939/J1939Codec.h"

TEST(J1939CodecTests, DecodesEngineSpeedFromEEC1) {
    const std::uint8_t payload[8] = {
        0xFF, 0xFF, 0xFF, 0x20, 0x4E, 0xFF, 0xFF, 0xFF
    };

    const auto metrics = torquebus::j1939::decodeEEC1(payload, sizeof(payload));

    ASSERT_TRUE(metrics.has_value());
    EXPECT_NEAR(metrics->engineSpeedRpm, 2500.0, 0.125);
}
```

Tests use `GTEST_SKIP()` when a required optional resource is unavailable, such
as a vendor driver or physical adapter. Keep the skip condition specific and
include a reason so the result is distinguishable from a pass.

## Export reports

CTest can write a JUnit report for CI systems and other report viewers:

```powershell
ctest --preset windows-msvc-debug --output-junit build/ctest-report.xml
```

Google Test can also write its native XML format when running a binary directly:

```powershell
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe `
    --gtest_output=xml:build/gtest-report.xml
```

## Continuous integration

The workflow in [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
runs for pull requests and pushes to `main` or `master`, version tags, and
manual dispatch. On Windows it configures and builds the Debug and Release
presets with warnings treated as errors. It runs unit and UI tests, then the
integration suite, and writes JUnit reports. On the Release build it also
packages the application and checks the package contents. A separate Ubuntu job
checks C++ formatting with the CI-pinned `clang-format` version.

CI does not run the `hardware` suite because hosted runners do not have the
required adapters. Use the local [validation checklist](validation.md) for the
manual application and hardware acceptance pass.
