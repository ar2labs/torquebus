## What this changes

<!-- One or two sentences. Link the issue if there is one. -->

Closes #

## Milestone

<!-- Which roadmap milestone does this belong to? See docs/PLAN.md / README. -->

## Architecture checklist

Please confirm each of these, or explain why the change needs an exception.
See `docs/ARCHITECTURE.md`.

- [ ] No UI code talks to hardware directly
- [ ] No driver code includes a widget header
- [ ] `src/core/` still includes only the C++ standard library
- [ ] No `QObject` is created per received frame
- [ ] Nothing blocks the UI thread waiting on hardware
- [ ] Hardware differences are expressed as capabilities, not brand checks
- [ ] No proprietary vendor SDK was committed

## Testing

- [ ] Unit tests added or updated
- [ ] Integration tests added or updated (virtual backend)
- [ ] Tests requiring a physical adapter are labelled `hardware`
- [ ] `ctest` passes locally
- [ ] `clang-format` is clean

Hardware this was tested against (if any):

<!-- e.g. Kvaser Virtual CAN 0/1, PCAN-USB FD, none -->
