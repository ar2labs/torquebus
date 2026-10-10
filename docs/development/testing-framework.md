# Testing Framework & Quality Assurance (Google Test & CTest)

TorqueBus relies on automated testing as a design constraint, not an afterthought: **every commit runs 834 automated tests** across unit, UI, integration, and hardware layers. The project uses [Google Test](https://github.com/google/googletest) (v1.15.2) as its primary C++ testing framework, integrated with CMake and CTest.

---

## 1. Test Architecture & Layers

The test suite is structured into four distinct layers in `tests/`:

```
tests/
├── unit/          # Fast, isolated tests for core data structures and protocols (707 tests)
├── ui/            # Headless and GUI tests for QML components, widgets, and docking (86 tests)
├── integration/   # Multi-node bus scenarios, Lua scripts, dataflow throughput (41 tests)
└── hardware/      # Adapter loopbacks against physical Kvaser / PEAK hardware (4 tests)
```

| Suite | Target | Focus | Execution Speed |
|---|---|---|---|
| **Unit** (`tests/unit`) | `torquebus_unit_tests` | CAN engine core, DBC parser, J1939 codecs, ISO-TP, UDS, TinyML inference, Lua runtime | ~35 seconds |
| **UI** (`tests/ui`) | `torquebus_ui_tests` | Dashboard Instrument Cluster, QML bindings, docking layouts, settings dialogs | ~42 seconds |
| **Integration** (`tests/integration`) | `torquebus_integration_tests` | Virtual CAN multi-channel bus, Lua ECU lifecycles, pipeline dataflow, 150k frames/s throughput benchmarks | ~14 seconds |
| **Hardware** (`tests/hardware`) | `torquebus_hardware_tests` | Physical PEAK PCAN and Kvaser adapter loopbacks | ~1 second (auto-skipped if detached) |

---

## 2. Google Test Integration

Google Test is fetched and built via CMake's `FetchContent` in `cmake/TorqueBusDependencies.cmake`:

```cmake
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
```

Tests are discovered at build time via `gtest_discover_tests(...)`, ensuring that every `TEST()` or `TEST_F()` declared in C++ is registered as an independent test case in CTest.

### Anatomy of a TorqueBus Unit Test

Tests follow the Arrange-Act-Assert pattern with strict assertions:

```cpp
#include <gtest/gtest.h>
#include "torquebus/j1939/J1939Codec.h"

// Basic test case
TEST(J1939CodecTests, DecodesEngineSpeedFromEEC1) {
    // Arrange: Create a standard 8-byte J1939 EEC1 frame (PGN 0xF004)
    uint8_t payload[8] = { 0xFF, 0xFF, 0xFF, 0x20, 0x4E, 0xFF, 0xFF, 0xFF }; // 2500 RPM
    
    // Act: Decode through the codec
    auto metrics = torquebus::j1939::decodeEEC1(payload, sizeof(payload));

    // Assert: Verify expected values
    ASSERT_TRUE(metrics.has_value());
    EXPECT_NEAR(metrics->engineSpeedRpm, 2500.0, 0.125);
}
```

### Hardware-Dependent Tests (`GTEST_SKIP`)

Tests requiring physical CAN hardware must never fail when running on developer machines or cloud CI runners. They detect driver availability and skip cleanly using `GTEST_SKIP()`:

```cpp
TEST_F(PeakLoopbackTests, ChannelOpensAndCloses) {
    if (!PeakDriver::isAvailable()) {
        GTEST_SKIP() << "no peakcan plugin or hardware detected";
    }
    // Loopback test logic...
}
```

This keeps the test suite **100% green** in all environments while retaining full hardware test coverage when plugged into a physical test bench.

---

## 3. Running Tests Locally

### Using CTest (Recommended)

From the project root using the standard developer prompt:

```powershell
# Run all unit and integration tests (preset matching your build)
ctest --preset windows-msvc-debug --output-on-failure

# Run only a specific category by label
ctest --preset windows-msvc-debug -L integration --output-on-failure

# Run only tests matching a name pattern
ctest --preset windows-msvc-debug -R J1939 --output-on-failure
```

### Running Native Google Test Executables

You can run the test binaries directly to access Google Test CLI flags:

```powershell
# Run all unit tests with colored console output
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe

# Filter specific test suites or cases
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_filter=J1939*

# Repeat tests to check for race conditions / flakiness
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_repeat=10 --gtest_break_on_failure
```

---

## 4. Exporting Formal Reports (XML / JUnit)

Google Test and CTest can export machine-readable test reports compatible with Jenkins, SonarQube, GitHub Actions, and academic evaluation packages.

### Exporting via Google Test
```powershell
.\build\windows-msvc-debug\bin\torquebus_unit_tests.exe --gtest_output=xml:build/gtest_report.xml
```

### Exporting via CTest
```powershell
ctest --preset windows-msvc-debug --output-junit build/ctest_report.xml
```

The resulting XML files contain detailed execution times, individual test status, and error logs for every test case.

---

## 5. Continuous Integration (GitHub Actions)

Every pull request and push to `master` triggers `.github/workflows/ci.yml`.

### Automated Test Pipeline:
1. **Configures & Compiles** in both `Debug` and `Release` modes using MSVC 2022.
2. **Executes 834 Google Tests** across both configurations.
3. **Generates JUnit XML Artifacts** for both Debug and Release.
4. **Publishes Visual Summary** directly to GitHub Actions Job Summary with test metrics and duration tables.
5. **Uploads Test Results** as downloadable artifacts for auditing and grading.

View live test runs at: [https://github.com/ar2labs/torquebus/actions](https://github.com/ar2labs/torquebus/actions)
