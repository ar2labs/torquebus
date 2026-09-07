// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What node types exist, and how to build one.
//
// Until now a node was created by C++ code that named its class. That works
// exactly as long as the person choosing the nodes is the person compiling -
// which stops being true the moment there is a canvas, a project file, or a
// script that assembles a measurement. All three need the same thing: a type
// name, some settings, and something that turns the pair into a node.
//
// The catalog is also what a canvas draws *from*. It declares each type's ports
// and parameters without instantiating anything, so a node palette can show
// "CAN Channel: one output, Frames" before the user has dropped one, and the
// properties panel can build its fields from the parameter list rather than
// from a switch on the type name.

#pragma once

#include "core/Result.h"
#include "core/pipeline/NodeParameters.h"
#include "core/pipeline/PipelineNode.h"
#include "core/pipeline/PortType.h"
#include "core/diagnostics/DiagnosticSession.h"
#include "core/log/ReplayControl.h"
#include "core/log/TraceLog.h"
#include "core/plot/SignalSeries.h"

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

class CanChannel;
class ScriptLibrary;
class TraceStore;
class TransmitList;

/// The resources a node may need but must not own.
///
/// A source node borrows a channel; a trace sink borrows the engine's store.
/// Passing them through the build rather than looking them up keeps the catalog
/// free of globals, and keeps a node's lifetime strictly inside the graph's.
struct NodeBuildContext final {
    /// Channels in application order; `channel(0)` is CAN 1.
    std::function<CanChannel*(std::uint8_t)> channel;

    /// The measurement's trace store, or nullptr when there is none.
    TraceStore* traceStore{nullptr};

    /// The open log a `can.log` block records into, or nullptr.
    ///
    /// Borrowed, and nullptr when the measurement is not recording - which is
    /// why a graph holding a logger block fails to build under Start and
    /// succeeds under Record. Silently recording nothing would be worse.
    TraceLogWriter* logWriter{nullptr};

    /// The diagnostic conversation the console drives, or nullptr for a graph
    /// nobody is asking questions through.
    DiagnosticSession* diagnosticSession{nullptr};

    /// The transport a replay block obeys, or nullptr for a graph nobody is
    /// driving. Shared by every replay block in one graph, deliberately: Pause
    /// on a playback bar means "pause the playback", not "pause one of the two
    /// files I happen to have on the canvas".
    ReplayControl* replayControl{nullptr};

    /// Where decoded signals accumulate for the Graph panel, or nullptr.
    ///
    /// Borrowed like the trace store and for the same reason: it belongs to the
    /// measurement rather than to the graph, and the panel reads it on its own
    /// timer while the executor writes.
    SignalSeriesStore* plotStore{nullptr};

    /// The transmit list the Transmit panel edits, or nullptr.
    ///
    /// Borrowed, like the trace store. It belongs to the project rather than to
    /// the graph, because the panel goes on editing it while a measurement runs
    /// - which is the whole point of a transmit list - and a copy taken at
    /// compile time would freeze it at whatever it held when Start was pressed.
    TransmitList* transmitList{nullptr};

    /// Where a script's log_message() and errors go.
    std::function<void(const std::string& text, bool isError)> log;

    /// Where the script editor hands edited sources in, or nullptr for a graph
    /// nobody is editing.
    ///
    /// Borrowed, like the trace store and for the same reason: it outlives the
    /// graph. A measurement stopped and started again must not throw away what
    /// the editor is holding, and the panel goes on editing while the executor
    /// runs - which is the entire point.
    ScriptLibrary* scriptLibrary{nullptr};

    /// Directory that a relative path in a node's parameters is relative *to*.
    ///
    /// The project file's own directory, in practice. Without it a `.tbsproj`
    /// naming `databases/vehicle.dbc` resolves against the process's working
    /// directory - so the project opens from the repository root and from
    /// nowhere else, which is not a property anyone would guess a project file
    /// had.
    ///
    /// Resolved at build time rather than at load. Rewriting the paths when the
    /// file is read would make them absolute, and saving would then write those
    /// absolute paths back - turning a project that travels with its folder
    /// into one pinned to the machine that last saved it.
    ///
    /// Empty means "resolve against the working directory", which is the right
    /// answer for a graph built without a project behind it.
    std::string basePath;
};

/// Everything a canvas needs to know about a type before instantiating it.
struct NodeTypeInfo final {
    /// Stable identifier, written into project files: "can.source", "lua.ecu".
    /// Never translated and never renamed - a rename orphans saved graphs.
    std::string typeName;

    /// Shown in the node palette. This one is for people.
    std::string displayName;

    /// Groups the palette: "Sources", "Transforms", "Sinks", "Simulation".
    std::string category;

    std::string description;

    std::vector<PortDescriptor> inputs;
    std::vector<PortDescriptor> outputs;
    std::vector<ParameterDescriptor> parameters;

    /// The type accepts settings beyond the ones it declares.
    ///
    /// True for lua.ecu, and for the same reason its parameters exist at all:
    /// a script's settings are the script's, invented by whoever wrote it, and
    /// the catalog cannot know that `speed_id` matters to one ECU and
    /// `threshold_high` to another. Everything undeclared is handed to the
    /// script's `parameters` table.
    ///
    /// The properties editor reads this to decide whether to offer a way to add
    /// one. Without the flag it would have to guess, and guessing wrong in
    /// either direction is bad: an editor that hides a setting the project
    /// already contains, or one that invites a setting nothing will ever read.
    bool acceptsExtraParameters{false};
};

/// Builds one node. Returns a failed Result rather than throwing, and names the
/// node in the message: a graph with forty nodes needs to say which one.
using NodeCreator = std::function<Result(const NodeParameters& parameters,
                                         const NodeBuildContext& context,
                                         std::string_view nodeId,
                                         std::unique_ptr<IPipelineNode>& out)>;

/// Checks a node's settings without building it.
///
/// For the constraints a ParameterDescriptor cannot express - "exactly one of
/// these two", "this range only applies when that flag is set". A descriptor
/// says whether one parameter is required; a rule about two of them together
/// has nowhere else to live.
///
/// Optional. A type whose parameters are independent registers none.
using NodeValidator = std::function<Result(const NodeParameters& parameters,
                                           std::string_view nodeId)>;

class NodeCatalog final {
public:
    /// A catalog with the built-in types already registered.
    ///
    /// A free function rather than a singleton: tests build catalogs with one
    /// type in them, and a plugin host (v0.17) will want to hand out a catalog
    /// that a plugin has added to without every other measurement seeing it.
    [[nodiscard]] static NodeCatalog withBuiltinTypes();

    void registerType(NodeTypeInfo info, NodeCreator creator, NodeValidator validator = {});

    /// Everything wrong with a node's settings that can be known without a
    /// build context - no channels, no trace store, no transmit list.
    ///
    /// This is what lets a problem be reported while the user is looking at the
    /// block they just dropped, rather than at the next Start. Missing required
    /// parameters, then the type's own rules.
    ///
    /// An unknown type is *not* an error here: GraphDescription::validate
    /// reports that separately and with a better message, and reporting it
    /// twice would put two lines in the Output panel for one mistake.
    [[nodiscard]] Result validateParameters(std::string_view typeName,
                                            const NodeParameters& parameters,
                                            std::string_view nodeId) const;

    [[nodiscard]] bool contains(std::string_view typeName) const;

    /// Nullptr when unknown.
    [[nodiscard]] const NodeTypeInfo* find(std::string_view typeName) const;

    /// Every type, in registration order, so a palette's order is a decision
    /// rather than a hash.
    [[nodiscard]] std::span<const NodeTypeInfo> types() const noexcept { return m_order; }

    /// Builds one node. An unknown type fails with the name it was given and
    /// the names it could have had - a typo in a hand-edited project file is
    /// otherwise a silent empty graph.
    [[nodiscard]] Result create(std::string_view typeName,
                                const NodeParameters& parameters,
                                const NodeBuildContext& context,
                                std::string_view nodeId,
                                std::unique_ptr<IPipelineNode>& out) const;

private:
    std::vector<NodeTypeInfo> m_order;
    std::map<std::string, NodeCreator, std::less<>> m_creators;
    std::map<std::string, NodeValidator, std::less<>> m_validators;
    std::map<std::string, std::size_t, std::less<>> m_index;
};

} // namespace torquebus
