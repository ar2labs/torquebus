// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/NodeCatalog.h"

#include "core/can/CanChannel.h"
#include "core/database/DbcParser.h"
#include "core/pipeline/nodes/DbcDecoderNode.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/transmit/TransmitListNode.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/log/LogNodes.h"
#include "core/plot/SignalPlotNode.h"
#include "core/trace/TraceSinkNode.h"
#include "core/trace/TraceStore.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace torquebus {
namespace {

/// A parameter path, made absolute against the project's directory.
///
/// An absolute path is returned unchanged: a user who typed one meant it, and
/// silently reinterpreting it against a project folder would be worse than the
/// problem this solves.
[[nodiscard]] std::string resolvePath(const NodeBuildContext& context, std::string path)
{
    if (path.empty() || context.basePath.empty()) {
        return path;
    }

    const std::filesystem::path candidate{path};
    if (candidate.is_absolute()) {
        return path;
    }

    // lexically_normal so that "../scripts/ecu.lua" comes out readable in an
    // error message rather than as a base directory with a "/../" in the
    // middle of it.
    return (std::filesystem::path{context.basePath} / candidate).lexically_normal().string();
}

/// Reads a script from disk, so a project can reference a .lua by path.
[[nodiscard]] Result readFile(const std::string& path, std::string& out)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Cannot open script '{}'", path));
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    out = contents.str();

    return Result::ok();
}

/// The channel a node names, or a failure that says which one was missing.
[[nodiscard]] Result resolveChannel(const NodeParameters& parameters,
                                    const NodeBuildContext& context,
                                    std::string_view nodeId,
                                    CanChannel*& out)
{
    const auto index = static_cast<std::uint8_t>(parameters.integer("channel", 0));

    if (!context.channel) {
        return Result::error(ErrorCode::InvalidState,
                             std::format("Node '{}' needs a CAN channel, but this graph is "
                                         "being built without an engine",
                                         nodeId));
    }

    out = context.channel(index);

    if (out == nullptr) {
        // The number the user sees is 1-based, so report it that way. A message
        // saying "channel 0" when the panel says "CAN 1" is a message that
        // costs somebody ten minutes.
        return Result::error(ErrorCode::DeviceNotFound,
                             std::format("Node '{}' is set to CAN {}, which is not configured",
                                         nodeId, index + 1));
    }

    return Result::ok();
}

/// The one rule about lua.ecu's parameters that a descriptor cannot express.
///
/// Neither `script` nor `scriptPath` is required on its own, and exactly one of
/// them is required together - which is a sentence about two parameters, and so
/// has nowhere to live but here.
///
/// Called twice on purpose: once by the catalog's validator, so the problem is
/// reported while the block is on screen, and once by the creator, which must
/// not assume anybody validated first.
[[nodiscard]] Result checkLuaScriptChoice(const NodeParameters& parameters,
                                          std::string_view nodeId)
{
    const bool hasSource = parameters.contains("script");
    const bool hasPath = parameters.contains("scriptPath");

    if (hasSource && hasPath) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Block '{}' has both a script and a script file. Keep one - "
                        "otherwise which one runs depends on this code, not on you.",
                        nodeId));
    }

    if (!hasSource && !hasPath) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Block '{}' has no script yet. Select it and set either Script "
                        "or Script file in its settings.",
                        nodeId));
    }

    return Result::ok();
}

} // namespace

void NodeCatalog::registerType(NodeTypeInfo info, NodeCreator creator, NodeValidator validator)
{
    const std::string typeName = info.typeName;

    // Re-registering replaces, so a plugin can override a built-in type
    // deliberately - but the order stays put, or a palette would reshuffle
    // itself whenever a plugin loaded.
    if (const auto existing = m_index.find(typeName); existing != m_index.end()) {
        m_order[existing->second] = std::move(info);
        m_creators.insert_or_assign(typeName, std::move(creator));
        m_validators.insert_or_assign(typeName, std::move(validator));
        return;
    }

    m_index.emplace(typeName, m_order.size());
    m_order.push_back(std::move(info));
    m_creators.emplace(typeName, std::move(creator));
    m_validators.emplace(typeName, std::move(validator));
}

Result NodeCatalog::validateParameters(std::string_view typeName,
                                       const NodeParameters& parameters,
                                       std::string_view nodeId) const
{
    const NodeTypeInfo* info = find(typeName);
    if (info == nullptr) {
        return Result::ok();
    }

    for (const ParameterDescriptor& parameter : info->parameters) {
        // A string from the view, because the parameter map is keyed by
        // std::string with no transparent comparator. One allocation per
        // declared parameter, on an edit - not on the frame path.
        if (!parameter.required || parameters.contains(std::string{parameter.name})) {
            continue;
        }

        // Named by the label the properties editor shows, not by the key the
        // project file uses. The person reading this is looking at a form, and
        // "Channel" is what is written next to the empty field.
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Block '{}' needs its {} set before the pipeline can run.",
                        nodeId, parameter.displayName));
    }

    if (const auto validator = m_validators.find(typeName);
        validator != m_validators.end() && validator->second) {
        return validator->second(parameters, nodeId);
    }

    return Result::ok();
}

bool NodeCatalog::contains(std::string_view typeName) const
{
    return m_index.find(typeName) != m_index.end();
}

const NodeTypeInfo* NodeCatalog::find(std::string_view typeName) const
{
    const auto it = m_index.find(typeName);
    return it == m_index.end() ? nullptr : &m_order[it->second];
}

Result NodeCatalog::create(std::string_view typeName,
                           const NodeParameters& parameters,
                           const NodeBuildContext& context,
                           std::string_view nodeId,
                           std::unique_ptr<IPipelineNode>& out) const
{
    const auto creator = m_creators.find(typeName);

    if (creator == m_creators.end()) {
        // List what was available. A hand-edited project file with "can.trace"
        // where "trace.sink" was meant is otherwise an empty graph and no clue.
        std::string known;
        for (const NodeTypeInfo& info : m_order) {
            known += known.empty() ? "" : ", ";
            known += info.typeName;
        }

        return Result::error(ErrorCode::NotImplemented,
                             std::format("Node '{}' has unknown type '{}'. Known types: {}",
                                         nodeId, typeName, known));
    }

    return creator->second(parameters, context, nodeId, out);
}

// ---------------------------------------------------------------------------
// The built-in types
// ---------------------------------------------------------------------------

NodeCatalog NodeCatalog::withBuiltinTypes()
{
    NodeCatalog catalog;

    // --- Sources ----------------------------------------------------------

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "can.source",
            .displayName = "CAN Channel",
            .category = "Sources",
            .description = "Frames received on one application channel.",
            .inputs = {},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            // Not required: it defaults to CAN 1, which is the channel a block
            // dropped on the canvas almost always means. `required` says the
            // node *cannot be built* without it, and this one can.
            .parameters = {ParameterDescriptor{.name = "channel",
                                               .displayName = "Channel",
                                               .type = ParameterValue::Type::Integer,
                                               .required = false,
                                               .description = "Application channel, 0 for CAN 1."}},
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            CanChannel* channel = nullptr;
            if (Result result = resolveChannel(parameters, context, nodeId, channel);
                result.failed()) {
                return result;
            }

            out = std::make_unique<ChannelSourceNode>(*channel);
            return Result::ok();
        });

    // --- Transforms -------------------------------------------------------

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "can.filter",
            .displayName = "Frame Filter",
            .category = "Transforms",
            .description = "Passes only the frames that match an identifier range.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters =
                {
                    ParameterDescriptor{.name = "from",
                                        .displayName = "First identifier",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "Lowest identifier to pass."},
                    ParameterDescriptor{.name = "to",
                                        .displayName = "Last identifier",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "Highest identifier to pass."},
                    ParameterDescriptor{.name = "mask",
                                        .displayName = "Mask",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Optional (id & mask) == (value & mask) test."},
                    ParameterDescriptor{.name = "value",
                                        .displayName = "Value",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "The value the mask is compared against."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext&, std::string_view,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            CanFilter filter;
            filter.name = "graph";
            filter.identifierFrom =
                static_cast<std::uint32_t>(parameters.integer("from", 0));
            filter.identifierTo = static_cast<std::uint32_t>(
                parameters.integer("to", kMaxExtendedIdentifier));
            filter.mask = static_cast<std::uint32_t>(parameters.integer("mask", 0));
            filter.value = static_cast<std::uint32_t>(parameters.integer("value", 0));

            CanFilterSet filters;
            filters.add(std::move(filter));

            out = std::make_unique<FrameFilterNode>(std::move(filters));
            return Result::ok();
        });

    // --- Simulation -------------------------------------------------------

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "lua.ecu",
            .displayName = "Lua ECU",
            .category = "Simulation",
            .description = "A simulated ECU whose behaviour is a Lua script.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters =
                {
                    ParameterDescriptor{.name = "script",
                                        .displayName = "Script",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description = "The Lua source, held in the project."},
                    ParameterDescriptor{.name = "scriptPath",
                                        .displayName = "Script file",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description = "A .lua file to load instead."},
                    ParameterDescriptor{.name = "database",
                                        .displayName = "Database",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description =
                                            "Optional .dbc, so the script can use "
                                            "emit_signal() and decode()."},
                    ParameterDescriptor{.name = "channel",
                                        .displayName = "Transmit channel",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Channel stamped onto the frames it emits."},
                },
            .acceptsExtraParameters = true,
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            // Inline source or a path, never both silently: a project holding
            // one copy of the script and a stale path to another is a bug that
            // only shows up on the machine where the path resolves.
            if (Result result = checkLuaScriptChoice(parameters, nodeId); result.failed()) {
                return result;
            }

            // Exactly one of the two is present - checkLuaScriptChoice just
            // said so - so asking about one answers for both.
            const bool hasPath = parameters.contains("scriptPath");

            std::string source;
            std::string name{nodeId};

            if (hasPath) {
                const std::string path = resolvePath(context, parameters.text("scriptPath"));
                if (Result result = readFile(path, source); result.failed()) {
                    return Result::error(result.code(),
                                         std::format("Node '{}': {}", nodeId,
                                                     std::string{result.message()}));
                }

                // Errors read "ecu_motor.lua:42", not the node id, because that
                // is the file the user has open.
                const std::size_t separator = path.find_last_of("/\\");
                name = separator == std::string::npos ? path : path.substr(separator + 1);
            } else {
                source = parameters.text("script");
            }

            auto node = std::make_unique<LuaEcuNode>(
                std::move(source), std::move(name),
                static_cast<std::uint8_t>(parameters.integer("channel", 0)));

            // Everything the node itself did not consume becomes the script's
            // `parameters` table. That is what makes one script reusable: a
            // temperature sensor with can_id and update_interval as parameters
            // is four sensors on four identifiers, not four copies of a file.
            //
            // The three reserved names are excluded because they configure the
            // node rather than the behaviour, and a script reading
            // parameters.script would be reading its own source back.
            // Settings the *node* reads. Everything else in the parameter map
            // is the script's, and reaches it through the `parameters` global.
            static constexpr std::string_view kReserved[] = {"script", "scriptPath",
                                                             "channel", "database"};

            std::map<std::string, LuaValue> scriptParameters;

            for (const auto& [key, value] : parameters.values()) {
                if (std::find(std::begin(kReserved), std::end(kReserved), key)
                    != std::end(kReserved)) {
                    continue;
                }

                switch (value.type()) {
                case ParameterValue::Type::Boolean:
                    scriptParameters.emplace(key, LuaValue::fromBoolean(value.asBoolean()));
                    break;
                case ParameterValue::Type::Integer:
                    scriptParameters.emplace(key, LuaValue::fromInteger(value.asInteger()));
                    break;
                case ParameterValue::Type::Real:
                    scriptParameters.emplace(key, LuaValue::fromNumber(value.asReal()));
                    break;
                case ParameterValue::Type::Text:
                    scriptParameters.emplace(key, LuaValue::fromString(value.asText()));
                    break;
                }
            }

            // Optional. A script that only calls emit() with packed bytes
            // needs no database, and a block that has not been given one yet
            // must still build - the alternative is a canvas that cannot be
            // assembled in any order but one.
            if (const std::string databasePath =
                    resolvePath(context, parameters.text("database", ""));
                !databasePath.empty()) {
                auto database = std::make_shared<CanDatabase>();

                if (Result result = DbcParser::parseFile(databasePath, *database);
                    result.failed()) {
                    return Result::error(result.code(),
                                         std::format("Node '{}': {}", nodeId,
                                                     std::string{result.message()}));
                }

                node->setDatabase(std::move(database));
            }

            node->setScriptParameters(std::move(scriptParameters));

            if (context.log) {
                node->setLogHandler(context.log);
            }

            out = std::move(node);
            return Result::ok();
        },
        // The same rule the creator checks, registered so it can also be
        // checked without a build context - which is what lets an unfinished
        // block be reported while it is on screen instead of at the next Start.
        checkLuaScriptChoice);

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "dbc.decoder",
            .displayName = "DBC Decoder",
            .category = "Transforms",
            .description = "Turns frames into named signal values using a .dbc database.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"signals", PortType::Signals}},
            // Not required, and the creator below says why at length: a
            // decoder with no database yet builds as a decoder that decodes
            // nothing, so a canvas can be assembled in any order.
            .parameters = {ParameterDescriptor{.name = "database",
                                               .displayName = "Database",
                                               .type = ParameterValue::Type::Text,
                                               .required = false,
                                               .description = "Path to a .dbc file."}},
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            const std::string path = resolvePath(context, parameters.text("database", ""));

            // An empty path builds a decoder with no database rather than
            // failing. A block dropped on the canvas has no path yet, and a
            // graph that will not compile until every block is configured
            // cannot be built up in any order but one.
            if (path.empty()) {
                out = std::make_unique<DbcDecoderNode>(nullptr, "DBC decoder");
                return Result::ok();
            }

            auto database = std::make_shared<CanDatabase>();
            if (Result result = DbcParser::parseFile(path, *database); result.failed()) {
                return Result::error(result.code(),
                                     std::format("Node '{}': {}", nodeId, result.message()));
            }

            // One database per node. Two decoders pointing at the same file
            // parse it twice and hold two copies - correct, and wasteful in a
            // way worth fixing when a project appears that does it. Caching in
            // the build context is the fix; guessing at it now would be a cache
            // with no measurement behind it.
            const std::string label =
                std::format("{} ({} messages)",
                            path.substr(path.find_last_of("/\\") + 1),
                            database->messageCount());

            out = std::make_unique<DbcDecoderNode>(std::move(database), label);
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "transmit.list",
            .displayName = "Transmit List",
            .category = "Sources",
            .description = "Frames a person asked to send, on a schedule or on demand.",
            .inputs = {},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters = {ParameterDescriptor{
                .name = "channel",
                .displayName = "Channel",
                .type = ParameterValue::Type::Integer,
                .required = false,
                .description = "Which channel's rows this block sends. 0 for CAN 1."}},
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            if (context.transmitList == nullptr) {
                return Result::error(
                    ErrorCode::InvalidState,
                    std::format("Node '{}' is a transmit list, but this graph is being "
                                "built without one",
                                nodeId));
            }

            const auto channel = static_cast<std::uint8_t>(parameters.integer("channel", 0));

            out = std::make_unique<TransmitListNode>(*context.transmitList, channel);
            return Result::ok();
        });

    // --- Sinks ------------------------------------------------------------

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "log.source",
            .displayName = "Log Replay",
            .category = "Sources",
            .description = "Replays a .tblog file as though it were a bus.",
            .inputs = {},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters =
                {
                    ParameterDescriptor{.name = "path",
                                        .displayName = "Log file",
                                        .type = ParameterValue::Type::Text,
                                        .required = true,
                                        .description = "The .tblog to replay."},
                    ParameterDescriptor{.name = "speed",
                                        .displayName = "Speed",
                                        .type = ParameterValue::Type::Real,
                                        .required = false,
                                        .description =
                                            "How much faster than real time. 1 replays a "
                                            "minute in a minute."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            const std::string path = resolvePath(context, parameters.text("path", ""));

            // Required, unlike every other path in this catalog, and the
            // difference is real: a decoder with no database decodes nothing
            // and is a block you have not finished configuring, while a replay
            // with no file is a source that will never produce a frame - a
            // measurement that runs and does nothing, with no error anywhere.
            if (path.empty()) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Block '{}' needs a log file to replay.", nodeId));
            }

            auto reader = std::make_unique<TraceLogReader>();
            if (Result result = reader->open(path); result.failed()) {
                return Result::error(result.code(),
                                     std::format("Node '{}': {}", nodeId, result.message()));
            }

            out = std::make_unique<LogSourceNode>(std::move(reader),
                                                  parameters.real("speed", 1.0));
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "can.transmit",
            .displayName = "CAN Transmit",
            .category = "Sinks",
            .description = "Puts the frames it receives on a channel.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {},
            // Defaults to CAN 1, like can.source and for the same reason.
            .parameters = {ParameterDescriptor{.name = "channel",
                                               .displayName = "Channel",
                                               .type = ParameterValue::Type::Integer,
                                               .required = false,
                                               .description = "Application channel to send on."}},
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            CanChannel* channel = nullptr;
            if (Result result = resolveChannel(parameters, context, nodeId, channel);
                result.failed()) {
                return result;
            }

            out = std::make_unique<ChannelSinkNode>(*channel);
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "trace.sink",
            .displayName = "CAN Trace",
            .category = "Sinks",
            .description = "Records frames into the measurement's trace.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {},
            .parameters = {},
        },
        [](const NodeParameters&, const NodeBuildContext& context, std::string_view nodeId,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            if (context.traceStore == nullptr) {
                return Result::error(ErrorCode::InvalidState,
                                     std::format("Node '{}' records into the trace, but this "
                                                 "graph is being built without one",
                                                 nodeId));
            }

            out = std::make_unique<TraceSinkNode>(*context.traceStore);
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "can.log",
            .displayName = "CAN Logger",
            .category = "Sinks",
            .description = "Records the frames it receives into a .tblog file.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {},
            .parameters = {},
        },
        [](const NodeParameters&, const NodeBuildContext& context, std::string_view nodeId,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            if (context.logWriter == nullptr) {
                return Result::error(
                    ErrorCode::InvalidState,
                    std::format("Node '{}' records to a log, but this graph is being built "
                                "without one. Use Record rather than Start.",
                                nodeId));
            }

            out = std::make_unique<LogSinkNode>(*context.logWriter);
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "signal.plot",
            .displayName = "Signal Plot",
            .category = "Sinks",
            .description = "Keeps the decoded signals it receives, for the Graph panel.",
            .inputs = {PortDescriptor{"signals", PortType::Signals}},
            .outputs = {},
            .parameters = {},
        },
        [](const NodeParameters&, const NodeBuildContext& context, std::string_view nodeId,
           std::unique_ptr<IPipelineNode>& out) -> Result {
            if (context.plotStore == nullptr) {
                return Result::error(ErrorCode::InvalidState,
                                     std::format("Node '{}' plots signals, but this graph "
                                                 "is being built without a plot store",
                                                 nodeId));
            }

            out = std::make_unique<SignalPlotNode>(*context.plotStore);
            return Result::ok();
        });

    return catalog;
}

} // namespace torquebus
