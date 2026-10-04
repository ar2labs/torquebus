# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites:
#
#   unit         - no Qt event loop, no hardware. Runs everywhere, always.
#   integration  - the virtual backend end to end. Runs everywhere, always.
#   ui           - widget interaction and docking, using Qt's offscreen plugin.
#   hardware     - needs a real adapter. Excluded from the default test run
#                  and labelled so CI can skip it.

# Every suite below finds the Qt it was built against, whatever is on PATH - and
# that includes a machine with none. This has to come first: it sets the default
# for gtest_discover_tests, which is read when a suite calls it.
#
# A test binary has to start twice over - once to be asked what tests it holds,
# once per test - and both happen inside ctest, which is where the Qt is put on
# PATH. Nothing runs a test binary at build time. The story is in
# cmake/TorqueBusTestEnvironment.cmake.
torquebus_use_built_qt_for_tests()

# The libraries the plugin loader tests load. Before unit, because unit needs
# the directory it puts them in.
add_subdirectory(plugins)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(ui)
add_subdirectory(hardware)
