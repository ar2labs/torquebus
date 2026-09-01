// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Guards the core headers against Qt's keyword macros.
//
// Rule #3 keeps Qt out of the core's *dependencies*. It cannot keep the core's
// headers from being *included by* Qt code - which is exactly what the ui layer
// does on every file. So a core header that uses `emit`, `signals`, `slots` or
// `foreach` as an identifier compiles perfectly on its own and fails the moment
// the UI includes it.
//
// That happened. `NodeContext::emit()` built and passed every core test, then
// produced a hundred errors on the first Windows build - starting in
// PipelineNode.h, cascading through <thread> and the CRT startup headers, with
// nothing in the output naming Qt. `emit` expands to nothing, so
// `void emit(std::size_t port, ...)` became `void (std::size_t port, ...)`.
//
// This file defines the four macros exactly as qglobal.h does and then includes
// every core and driver header. It needs no Qt, runs in the plain unit suite,
// and turns a confusing cross-layer failure into a compile error in the file
// that caused it.
//
// It deliberately includes no Qt header itself - the macros below would break
// Qt's own code.

#define slots
#define signals public
#define emit
#define foreach Q_FOREACH

#include "core/Result.h"
#include "core/can/CanChannel.h"
#include "core/can/CanEngine.h"
#include "core/can/CanFilter.h"
#include "core/can/CanFrame.h"
#include "core/can/CanStatistics.h"
#include "core/can/CanTypes.h"
#include "core/can/FrameQueue.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/NodeParameters.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/PipelineNode.h"
#include "core/pipeline/PortType.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/scripting/LuaRuntime.h"
#include "core/trace/TraceSinkNode.h"
#include "core/trace/TraceStore.h"
#include "drivers/api/CanBackendRegistry.h"
#include "drivers/api/ICanBackend.h"
#include "drivers/kvaser/KvaserCanBackend.h"
#include "drivers/virtual/VirtualCanBackend.h"

#undef slots
#undef signals
#undef emit
#undef foreach

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Core headers survive Qt's keyword macros", "[core][headers]")
{
    // Reaching this line means the includes above compiled with `emit`,
    // `signals`, `slots` and `foreach` defined - which is the whole assertion.
    // The check below exists so the test has a body; the real test ran in the
    // preprocessor.
    SUCCEED("Core headers use no Qt macro name as an identifier");
}
