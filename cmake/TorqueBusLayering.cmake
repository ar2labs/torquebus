# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turns three of the ten architectural rules from review into build failures.
#
# ARCHITECTURE.md rules 2, 3 and 4 are statements about what a library links:
# the core must not depend on widgets, a driver must not know the UI exists.
# Their "Enforced by" column used to say, in effect, that somebody would notice
# in review - and a layering violation is exactly the kind of thing that does
# not look wrong in a diff. One `target_link_libraries` line is all it takes,
# it compiles, every test passes, and the core is no longer linkable into a
# headless logger.
#
# So the rule is checked where it can be checked cheaply: at configure time,
# walking what a target actually links, transitively. Transitively matters -
# adding Qt6::Widgets to the core is the obvious mistake, but linking something
# that *itself* pulls in Qt6::Widgets has the same effect and hides one level
# further down. The error names the chain, because "the core links Qt6::Widgets"
# without saying how is a fact you then have to go and find.

# Collects the transitive link closure of `target` into `out_variable`.
function(_torquebus_link_closure target out_variable)
    set(pending "${target}")
    set(seen "")

    while(pending)
        list(POP_FRONT pending current)

        if(current IN_LIST seen)
            continue()
        endif()

        list(APPEND seen "${current}")

        if(NOT TARGET "${current}")
            continue()
        endif()

        # An ALIAS resolves to its real target; without this the walk stops at
        # every torquebus::x name and sees nothing beyond it.
        get_target_property(aliased "${current}" ALIASED_TARGET)
        if(aliased)
            list(APPEND pending "${aliased}")
            continue()
        endif()

        get_target_property(type "${current}" TYPE)

        # INTERFACE libraries have no LINK_LIBRARIES, only the interface one,
        # and asking for the wrong property on one is an error rather than an
        # empty answer.
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(direct "${current}" LINK_LIBRARIES)
            if(direct)
                list(APPEND pending ${direct})
            endif()
        endif()

        get_target_property(interface "${current}" INTERFACE_LINK_LIBRARIES)
        if(interface)
            list(APPEND pending ${interface})
        endif()
    endwhile()

    set("${out_variable}" "${seen}" PARENT_SCOPE)
endfunction()

# Fails the configure if `target` can reach any of the forbidden targets.
#
#   torquebus_forbid_link(torquebus_core
#       RULE  "rule 3: the core does not depend on widgets"
#       DENY  Qt6::Widgets Qt6::Gui
#   )
function(torquebus_forbid_link target)
    cmake_parse_arguments(FORBID "" "RULE" "DENY" ${ARGN})

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "torquebus_forbid_link: no such target '${target}'")
    endif()

    _torquebus_link_closure("${target}" closure)

    set(violations "")
    foreach(denied IN LISTS FORBID_DENY)
        if(denied IN_LIST closure)
            list(APPEND violations "${denied}")
        endif()
    endforeach()

    if(NOT violations)
        return()
    endif()

    string(REPLACE ";" ", " violation_text "${violations}")

    # Only real targets in the printed chain. The raw walk also carries
    # generator expressions and bare system library names, and a wall of
    # $<LINK_ONLY:...> buries the one line somebody needs to read.
    set(readable "")
    foreach(entry IN LISTS closure)
        if(TARGET "${entry}")
            list(APPEND readable "${entry}")
        endif()
    endforeach()
    string(REPLACE ";" "\n    " readable_text "${readable}")

    message(FATAL_ERROR
        "Architectural rule broken.\n"
        "  ${FORBID_RULE}\n"
        "  ${target} reaches: ${violation_text}\n"
        "\n"
        "  Through these targets:\n"
        "    ${readable_text}\n"
        "\n"
        "  This is ARCHITECTURE.md section 1, and the ten rules are frozen: a\n"
        "  change to one is an architectural decision, not a refactor. If that\n"
        "  decision has genuinely been taken, change the rule and this check in\n"
        "  the same commit, so the contract and the build never disagree."
    )
endfunction()
