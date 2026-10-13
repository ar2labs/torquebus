# SPDX-License-Identifier: GPL-3.0-or-later
#
# One correction to QtNodes 3.0.16, made to the source after it is fetched.
#
# What is wrong
# -------------
# ConnectionGraphicsObject::mouseMoveEvent and ::mouseReleaseEvent, the two functions that run while
# a wire is being dragged and when it is dropped, both begin
#
#     auto view = static_cast<QGraphicsView *>(event->widget());
#     auto ngo = locateNodeAt(event->scenePos(), *nodeScene(), view->transform());
#
# and the widget of a scene event is the *viewport* - a plain QWidget - not the QGraphicsView that
# owns it. The cast turns a QWidget into a QGraphicsView without asking, and view->transform() then
# reads a member of QGraphicsViewPrivate out of an object whose private part is a QWidgetPrivate:
# several hundred bytes past the end of it. Nearly always that is other live heap and the answer is
# a matrix of garbage nobody notices; now and then it is the end of a page, and the process dies of
# an access violation inside Qt6Widgets, from a mouse move, on a canvas that was working a moment
# earlier.
#
# It was found as a test that failed once in a thousand runs, with a stack that ended in
# ConnectionGraphicsObject::mouseMoveEvent (tests/ui/CrashTrace.h is what printed it). It is not a
# test problem: anybody dragging a wire is making those calls, once per mouse move.
#
# What this does
# --------------
# The view is the viewport's parent. Where there is none, the identity transform is used - which is
# what a view that is not scaled has, and all the transform is for is deciding which items ignore
# it. Done as a text replacement and not a patch file because it has to work on a source tree that
# FetchContent did not populate (FETCHCONTENT_SOURCE_DIR_QTNODES, a cached _deps), and has to be safe
# to run on every configure.
#
# It is idempotent, and it is loud in the one case that matters: if the lines are not the ones this
# was written against - the pinned tag moved, or upstream fixed it - configure stops and says so,
# instead of silently carrying on with a patch that does nothing. Remove this file when the tag
# moves to a release that has the fix.

function(torquebus_patch_qtnodes sourceDir)
    set(path "${sourceDir}/src/ConnectionGraphicsObject.cpp")

    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "PatchQtNodes.cmake: ${path} does not exist. QtNodes has been reorganised; the "
            "correction to its wire dragging (see cmake/PatchQtNodes.cmake) has to be looked at again.")
    endif()

    file(READ "${path}" source)

    set(marker "TorqueBus: the widget of a scene event is the viewport")

    string(FIND "${source}" "${marker}" alreadyPatched)
    if(NOT alreadyPatched EQUAL -1)
        return()
    endif()

    set(brokenCast "auto view = static_cast<QGraphicsView *>(event->widget());")
    set(brokenUse "view->transform()")

    string(FIND "${source}" "${brokenCast}" castAt)
    string(FIND "${source}" "${brokenUse}" useAt)

    if(castAt EQUAL -1 OR useAt EQUAL -1)
        message(FATAL_ERROR
            "PatchQtNodes.cmake: the lines it corrects are not in ${path}. Either the QtNodes tag "
            "was changed - check whether ConnectionGraphicsObject still casts the viewport to a "
            "QGraphicsView in mouseMoveEvent and mouseReleaseEvent, and delete this patch if it no "
            "longer does - or the file was edited by hand.")
    endif()

    string(CONCAT fixedCast
        "// ${marker}, not the view; the view is its parent.\n"
        "    auto view = event->widget() != nullptr\n"
        "                    ? qobject_cast<QGraphicsView *>(event->widget()->parentWidget())\n"
        "                    : nullptr;")

    string(REPLACE "${brokenCast}" "${fixedCast}" source "${source}")
    string(REPLACE "${brokenUse}" "(view != nullptr ? view->transform() : QTransform{})" source "${source}")

    file(WRITE "${path}" "${source}")

    message(STATUS "QtNodes: corrected which object ConnectionGraphicsObject reads the view transform from")
endfunction()
