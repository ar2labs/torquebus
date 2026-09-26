# SPDX-License-Identifier: GPL-3.0-or-later
#
# Resolves every external dependency of TorqueBus Studio.
#
#   Qt              - required, must be provided by the environment
#   KDDockWidgets   - fetched and built from source (FetchContent)
#   QtNodes         - fetched and built from source (FetchContent), the canvas
#   GoogleTest      - fetched and built from source (FetchContent), tests only
#   Kvaser CANlib   - optional, detected on the system
#   PCAN-Basic      - optional, reached through the Qt SerialBus peakcan plugin

include(FetchContent)
include(FindPackageHandleStandardArgs)

set(FETCHCONTENT_QUIET OFF)

# --------------------------------------------------------------------------
# Qt
# --------------------------------------------------------------------------

# Qt warns, once per private component, that using private headers ties the
# build to this exact Qt module build. The warning is accurate, and the coupling
# is real - but it is KDDockWidgets' coupling, not ours (see the comment on the
# private components below). Silencing it here is a statement that we have read
# it and accepted it, not that we are ignoring it.
#
# What it actually costs us:
#
#   * The binary must run against the Qt 6.11.2 DLLs it was built against.
#     windeployqt copies exactly those, so the packaged build is fine; what is
#     NOT fine is pointing the application at a different Qt on PATH.
#   * The Qt version is therefore pinned, not merely recommended, in
#     .github/workflows/ci.yml and in the documented requirements.
#
# Recorded in docs/ARCHITECTURE.md under the decision log.
set(QT_NO_PRIVATE_MODULE_WARNING ON)

# --- Finding Qt without depending on the prompt -------------------------
#
# Qt does not have to be on PATH for CMake to find it, and relying on it being
# there is how a build becomes "works in my shortcut". qtenv2.bat, the obvious
# thing to run, sets QTDIR and PATH and *nothing else* - in particular it does
# not set up MSVC, which it says on startup in a line that is easy to scroll
# past. A prompt built only from it produces a confusing link failure rather
# than an honest "Qt not found".
#
# So: look where Qt actually is. QTDIR first (that much qtenv2.bat does give
# us), then the default installer layout. Anything the user passes explicitly
# in CMAKE_PREFIX_PATH or Qt6_DIR still wins - these are only appended.

if(NOT DEFINED Qt6_DIR)
    set(_torquebus_qt_hints "")

    if(DEFINED ENV{QTDIR})
        list(APPEND _torquebus_qt_hints "$ENV{QTDIR}")
    endif()

    if(WIN32)
        # The version is pinned (PLAN.md section 1), so this is a short list of
        # one rather than a glob that might find 6.9 and fail confusingly later.
        list(APPEND _torquebus_qt_hints
            "C:/Qt/6.11.2/msvc2022_64"
            "D:/Qt/6.11.2/msvc2022_64"
        )
    endif()

    foreach(_torquebus_qt_hint IN LISTS _torquebus_qt_hints)
        if(EXISTS "${_torquebus_qt_hint}/lib/cmake/Qt6/Qt6Config.cmake")
            list(APPEND CMAKE_PREFIX_PATH "${_torquebus_qt_hint}")
            message(STATUS "Qt found at ${_torquebus_qt_hint}")
            break()
        endif()
    endforeach()

    unset(_torquebus_qt_hint)
    unset(_torquebus_qt_hints)
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Svg
    SvgWidgets
    SerialBus

    # Requested on QtNodes' behalf: it links Qt6::OpenGL publicly (its
    # CMakeLists names Core, Widgets, Gui and OpenGL), and a component that is
    # not in this find_package is a target that does not exist when
    # FetchContent brings the library in.
    OpenGL

    # --- Requested on KDDockWidgets' behalf, not ours --------------------
    #
    # KDDockWidgets links Qt6::WidgetsPrivate and Qt6::GuiPrivate (its
    # src/CMakeLists.txt, QtWidgets frontend and the WIN32 dwmapi block) but
    # its own find_package asks only for COMPONENTS Widgets. Standalone that
    # used to work because older Qt6 created the *Private targets as a side
    # effect; current Qt requires them to be requested explicitly.
    #
    # Because we pull KDDockWidgets in with FetchContent, it inherits this
    # find_package instead of doing its own - so the targets have to exist
    # before FetchContent_MakeAvailable() below, or generation fails with
    # "Target kddockwidgets links to Qt6::WidgetsPrivate but the target was
    # not found".
    #
    # TorqueBus itself uses no Qt private API and must not start.
    CorePrivate
    GuiPrivate
    WidgetsPrivate
)

qt_standard_project_setup()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --------------------------------------------------------------------------
# KDDockWidgets
# --------------------------------------------------------------------------

set(KDDockWidgets_QT6 ON CACHE BOOL "" FORCE)
set(KDDockWidgets_FRONTENDS "qtwidgets" CACHE STRING "" FORCE)
set(KDDockWidgets_STATIC ON CACHE BOOL "" FORCE)
set(KDDockWidgets_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_TESTS OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_DOCS OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(KDDockWidgets
    GIT_REPOSITORY https://github.com/KDAB/KDDockWidgets.git
    GIT_TAG        v2.2.5
    GIT_SHALLOW    TRUE
    SYSTEM
)

FetchContent_MakeAvailable(KDDockWidgets)

# --------------------------------------------------------------------------
# QtNodes - the canvas
# --------------------------------------------------------------------------
#
# Pinned to a tag rather than a branch, like every other dependency here. 3.0.16
# is the newest tag; master has already moved past it (node groups, editable
# labels, more NodeRoles) and the adapter in src/ui/canvas is written against
# what 3.0.16 declares, not against whatever master declares today.
#
# Four options have to be forced, and each of them fails differently if it is
# not:
#
#   BUILD_SHARED_LIBS  Their default is ON. We link everything statically, and a
#                      DLL here would have to be deployed beside the executable
#                      for no benefit.
#   BUILD_TESTING      Their external/CMakeLists.txt does add_subdirectory(Catch2)
#                      when this is on - and Catch2 is a git submodule that
#                      FetchContent does not initialise, so it fails at
#                      configure time with a bare "does not contain a
#                      CMakeLists.txt".
#   BUILD_EXAMPLES     Builds a dozen sample applications we would then have to
#                      look at in the Visual Studio solution.
#   BUILD_DOCS         Wants Doxygen.
#
# They default to QT_NODES_DEVELOPER_DEFAULTS, which is already OFF under
# FetchContent because the project detects a parent directory - but relying on
# that is relying on an implementation detail of theirs. Stating it costs four
# lines.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(QT_NODES_DEVELOPER_DEFAULTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(QtNodes
    GIT_REPOSITORY https://github.com/paceholder/nodeeditor.git
    GIT_TAG        3.0.16
    GIT_SHALLOW    TRUE
    SYSTEM
)

FetchContent_MakeAvailable(QtNodes)

# QtNodes compiles with QT_NO_KEYWORDS privately, so `emit` and `signals` still
# work in our own code. Recorded because it is the kind of setting that, if it
# were PUBLIC, would break every Qt file in the project and take an afternoon
# to trace back to a dependency.

# --------------------------------------------------------------------------
# GoogleTest (tests only)
# --------------------------------------------------------------------------

if(TORQUEBUS_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
endif()

# --------------------------------------------------------------------------
# Kvaser CANlib (optional, runtime-installed SDK)
# --------------------------------------------------------------------------

set(TORQUEBUS_HAVE_KVASER OFF)

if(TORQUEBUS_ENABLE_KVASER)
    # The SDK does not install to one canonical place: the official installer
    # uses Program Files, but people routinely unpack it somewhere else. Rather
    # than make everyone set an environment variable, search the roots that
    # actually turn up in practice - and let KVASER_CANLIB_DIR override.
    set(_torquebus_kvaser_roots
        "$ENV{KVASER_CANLIB_DIR}"
        "${KVASER_CANLIB_DIR}"
        "C:/Tools/Kvaser/Canlib"
        "C:/Kvaser/Canlib"
        "$ENV{ProgramFiles}/Kvaser/Canlib"
        "$ENV{ProgramFiles\(x86\)}/Kvaser/Canlib"
        "$ENV{SystemDrive}/Program Files/Kvaser/Canlib"
    )

    set(_torquebus_kvaser_include_hints "")
    set(_torquebus_kvaser_library_hints "")
    foreach(_root IN LISTS _torquebus_kvaser_roots)
        if(_root)
            list(APPEND _torquebus_kvaser_include_hints "${_root}/INC" "${_root}/inc" "${_root}")
            list(APPEND _torquebus_kvaser_library_hints
                 "${_root}/Lib/x64" "${_root}/lib/x64" "${_root}/Lib" "${_root}/lib")
        endif()
    endforeach()

    find_path(KVASER_CANLIB_INCLUDE_DIR
        NAMES canlib.h
        HINTS ${_torquebus_kvaser_include_hints}
        PATHS /usr/include
        DOC "Directory containing canlib.h"
    )

    # canlib32.lib is the 64-bit import library too - the name is historical and
    # does not mean 32-bit. Searching for "canlib" as well covers Linux.
    find_library(KVASER_CANLIB_LIBRARY
        NAMES canlib32 canlib
        HINTS ${_torquebus_kvaser_library_hints}
        PATHS /usr/lib
        DOC "Kvaser CANlib import library"
    )

    if(KVASER_CANLIB_INCLUDE_DIR AND KVASER_CANLIB_LIBRARY)
        set(TORQUEBUS_HAVE_KVASER ON)
        message(STATUS "Kvaser CANlib headers:  ${KVASER_CANLIB_INCLUDE_DIR}")
        message(STATUS "Kvaser CANlib library:  ${KVASER_CANLIB_LIBRARY}")
    elseif(KVASER_CANLIB_INCLUDE_DIR AND NOT KVASER_CANLIB_LIBRARY)
        # Worth distinguishing: this is a real SDK that is missing its 64-bit
        # import library, which usually means the 32-bit SDK was installed.
        message(WARNING
            "Kvaser CANlib headers were found at ${KVASER_CANLIB_INCLUDE_DIR} but no "
            "import library was. TorqueBus is x64 - check that Lib/x64/canlib32.lib "
            "exists, or point KVASER_CANLIB_DIR at the right SDK."
        )
    else()
        message(STATUS
            "Kvaser CANlib not found - the Kvaser backend will be compiled as a stub "
            "that reports itself unavailable. Install the Kvaser drivers + CANlib SDK, "
            "or pass -DKVASER_CANLIB_DIR=<path>."
        )
    endif()

    unset(_torquebus_kvaser_roots)
    unset(_torquebus_kvaser_include_hints)
    unset(_torquebus_kvaser_library_hints)
endif()

# --------------------------------------------------------------------------
# PEAK-System PCAN-Basic
# --------------------------------------------------------------------------
#
# TorqueBus does not link PCAN-Basic directly: it goes through the Qt SerialBus
# "peakcan" plugin, which loads PCANBasic.dll at runtime. There is therefore
# nothing to find at configure time - only the Qt6::SerialBus dependency, which
# is already required above.

set(TORQUEBUS_HAVE_PEAK ${TORQUEBUS_ENABLE_PEAK})
