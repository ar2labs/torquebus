# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three levels, per PLAN.md section 32:
#
#   unit         - no Qt event loop, no hardware. Runs everywhere, always.
#   integration  - the virtual backend end to end. Runs everywhere, always.
#   hardware     - needs a real adapter. Excluded from the default test run
#                  and labelled so CI can skip it.

# The libraries the plugin loader tests load. Before unit, because unit needs
# the directory it puts them in.
add_subdirectory(plugins)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(hardware)
