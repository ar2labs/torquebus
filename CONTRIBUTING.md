# Contributing to TorqueBus Studio

Thank you for considering a contribution. This project aims to be a serious
engineering tool, so the bar is deliberately about *structure* rather than
style — most review feedback here is about layering.

Please read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before your first
pull request. It is short, and it is the contract.

---

## Before you start

- **Open an issue first** for anything larger than a bug fix. A five-minute
  conversation about which layer something belongs in saves a rewritten branch.
- **Check the roadmap.** If your idea belongs to a later milestone, say so in
  the issue — landing it early is fine, landing it in the wrong layer is not.

---

## Building and testing

```powershell
cmake --preset windows-msvc-strict     # warnings as errors, like CI
cmake --build --preset windows-msvc-strict
ctest --preset windows-msvc-debug
```

Every pull request must pass, on Windows x64 with MSVC:

1. Configure
2. Build Debug **and** Release
3. Unit tests
4. Integration tests
5. `clang-format` check

Tests that need a physical adapter carry the `hardware` label and are excluded
from the default run.

---

## The rules that get pull requests rejected

These come straight from `docs/ARCHITECTURE.md`. They are not negotiable
inside a PR — changing one is a separate architectural discussion.

1. **No UI code talks to hardware.** Go through the engine.
2. **No driver includes a widget header.** `torquebus_drivers` does not link
   `Qt6::Widgets`, so this fails to build rather than failing review.
3. **The core stays Qt-free.** `src/core/` includes the C++ standard library
   and nothing else.
4. **`CanFrame` gains no vendor field.** If a backend needs to carry something
   extra, that is a conversation about the model, not a quick struct member.
5. **No `QObject` per received frame.** Batch, or use the existing queue.
6. **Never block the UI thread on hardware.**
7. **Logging never routes through a widget.**
8. **Protocol code never names a specific adapter.**
9. **No `if (backend == "peak")`.** Add a capability to `CanCapabilities`.
10. **A new extension point gets an interface**, not a switch statement.

If your change genuinely cannot be made without breaking one of these, that is
useful information — open an issue and say so.

---

## Code style

`clang-format` decides formatting; CI enforces it. Run it before pushing:

```powershell
clang-format -i <your files>
```

Beyond formatting:

- **C++23.** Prefer `std::span`, `std::string_view`, designated initialisers,
  `[[nodiscard]]` on anything whose result matters.
- **No raw owning pointers.** `std::unique_ptr` in the core; Qt parent/child
  ownership in the UI.
- **Return `Result`, do not throw across an API boundary**, and never print
  from a driver — the caller decides whether an error is a status bar message,
  a log line or a test failure.
- **Name things the way the domain does.** `identifier`, not `id`; `dlc`, not
  `len`; `ErrorPassive`, not `state2`.

### Comments

Comment the *why*, not the *what*. A comment explaining that a queue is bounded
because unbounded growth turns frame loss into an out-of-memory crash is worth
keeping. A comment saying `// increment the counter` is not.

---

## Commit messages

```
component: short imperative summary

Longer explanation of why the change is needed, if it is not obvious
from the summary. Reference the plan section or issue when relevant.

Fixes #123
```

Components: `core`, `drivers`, `services`, `ui`, `tests`, `build`, `docs`, `ci`.

---

## Adding a CAN backend

This is the most common substantial contribution, and the architecture is built
for it:

1. Implement `ICanBackend` in `src/drivers/<vendor>/`.
2. Translate the vendor's structures into `CanFrame` **inside the backend**.
   No vendor type may appear above the driver layer.
3. Fill in `CanCapabilities` honestly — the UI relies on it instead of on
   brand checks.
4. Make `isAvailable()` return `false` cleanly when the SDK is missing. A
   missing DLL is a greyed-out row, never a crash.
5. Register it in `CanBackendRegistry::registerBuiltins()`.
6. Add integration tests. Model them on
   `tests/integration/VirtualBusTests.cpp`; label anything that needs the
   physical adapter `hardware`.
7. **Do not commit the vendor SDK.** Detect it in
   `cmake/TorqueBusDependencies.cmake`.

If step 5 is the only file outside your own directory that you had to touch,
the architecture did its job.

---

## Licensing

TorqueBus Studio is GPLv3-or-later. By contributing, you agree that your
contribution is licensed under the same terms. Every new source file starts
with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

Do not paste code from proprietary tools, vendor SDK samples under restrictive
terms, or any source whose licence is incompatible with GPLv3. Interoperating
with a proprietary API is fine; copying its implementation is not.

### Narrowing through perfect forwarding

`std::make_unique`, `emplace_back` and friends forward their arguments by
reference, which loses the fact that a literal is a constant. A bare `3` passed
to a `std::uint8_t` parameter therefore becomes an `int&&` and narrows *inside*
`<memory>`, where the compiler can no longer see that the value fits:

```cpp
std::make_unique<LuaEcuNode>(source, name, 3);                 // MSVC C4242
std::make_unique<LuaEcuNode>(source, name, std::uint8_t{3});   // fine
```

MSVC reports this at a line in `<memory>`, with your call site appearing only in
the instantiation trace — so it is easy to read as a standard-library problem.
GCC and Clang do not warn at all, because they still see the constant. Name the
type at the call site.
