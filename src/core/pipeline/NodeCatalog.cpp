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
#include "core/j1939/J1939Node.h"
#include "core/scripting/LuaTestNode.h"
#include "core/simulation/RestBusNode.h"
#include "core/diagnostics/DiagnosticEvent.h"
#include "core/diagnostics/UdsClientNode.h"
#include "core/isotp/IsoTpNode.h"
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
            std::format("Block '{}' has no script yet. Set either Script or Script "
                        "file in the Block panel - or untick Enabled there to leave "
                        "it out of the run.",
                        nodeId));
    }

    return Result::ok();
}

/// Splits "a, b ,c" into three names, dropping the empty ones.
///
/// Comma-separated rather than a repeated parameter, because a project file is
/// read and diffed by people and `exclude = "BodyController, Gateway"` is one
/// line somebody can see the whole of.
[[nodiscard]] std::vector<std::string> splitNames(const std::string& list)
{
    std::vector<std::string> names;

    std::size_t start = 0;

    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::size_t end = comma == std::string::npos ? list.size() : comma;

        std::string_view name{list};
        name = name.substr(start, end - start);

        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.remove_prefix(1);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.remove_suffix(1);
        }

        if (!name.empty()) {
            names.emplace_back(name);
        }

        if (comma == std::string::npos) {
            break;
        }

        start = comma + 1;
    }

    return names;
}

/// Parses "Engine.EngineSpeed, Engine.Throttle=throttle_pedal".
///
/// The variable name is optional and defaults to the qualified signal name, so
/// the short form is the one somebody writes and the long form is there for
/// when a dashboard already has a variable by another name.
[[nodiscard]] Result parseDrivenSignals(const std::string& list,
                                        std::string_view nodeId,
                                        std::vector<RestBusNode::DrivenSignal>& out)
{
    for (const std::string& entry : splitNames(list)) {
        RestBusNode::DrivenSignal driven;

        std::string_view qualified{entry};

        if (const std::size_t equals = entry.find('='); equals != std::string::npos) {
            qualified = std::string_view{entry}.substr(0, equals);
            driven.variable = entry.substr(equals + 1);
        }

        const std::size_t dot = qualified.find('.');

        if (dot == std::string_view::npos) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Block '{}': '{}' does not name a signal. Signal names are "
                            "only unique within a message, so this wants "
                            "Message.Signal.",
                            nodeId, entry));
        }

        driven.message = std::string{qualified.substr(0, dot)};
        driven.signal = std::string{qualified.substr(dot + 1)};

        if (driven.message.empty() || driven.signal.empty()) {
            return Result::error(
                ErrorCode::InvalidArgument,
                std::format("Block '{}': '{}' is missing one half of Message.Signal",
                            nodeId, entry));
        }

        out.push_back(std::move(driven));
    }

    return Result::ok();
}

/// A rest bus with no database has nothing to say, and the block says so while
/// it is on screen rather than at the next Start.
[[nodiscard]] Result checkRestBus(const NodeParameters& parameters,
                                  std::string_view nodeId)
{
    if (parameters.text("database", "").empty()) {
        return Result::error(
            ErrorCode::InvalidArgument,
            std::format("Block '{}' has no database. A rest bus simulates the messages "
                        "a database describes, so it needs one - or untick Enabled to "
                        "leave it out of the run.",
                        nodeId));
    }

    std::vector<RestBusNode::DrivenSignal> ignored;
    return parseDrivenSignals(parameters.text("signals", ""), nodeId, ignored);
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
                    ParameterDescriptor{
                        .name = "udsRequestId",
                        .displayName = "Diagnostic request ID",
                        .type = ParameterValue::Type::Integer,
                        .required = false,
                        .description =
                            "Identifier this ECU *listens* on - what a tester sends to. "
                            "Set it and the block answers UDS; leave it and it does not."},
                    ParameterDescriptor{
                        .name = "udsResponseId",
                        .displayName = "Diagnostic response ID",
                        .type = ParameterValue::Type::Integer,
                        .required = false,
                        .description = "Identifier this ECU answers on. 2024 (0x7E8) "
                                       "answers a tester on 0x7E0."},
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

            // What the measurement has seen, so a script can ask about the bus
            // rather than only about the frames wired into it. Borrowed, read
            // from the executor thread that also writes it, and absent in a
            // graph built without a trace - which the bindings say rather than
            // crash on.
            node->setTraceStore(context.traceStore);

            // Where an edited script arrives from, keyed by this node's id
            // because that is what the editor knows it by. Null in every
            // headless build, and the node simply never looks.
            node->setScriptLibrary(context.scriptLibrary, std::string{nodeId});

            // What a dashboard's slider writes and its gauge reads. Null in a
            // headless build, and the bindings are then simply not there.
            node->setSystemVariables(context.variables);

            // The diagnostic layer, when the block has been given addresses.
            // The identifiers are the ECU's way round - it receives on what a
            // tester transmits - and getting that backwards is the commonest
            // way a simulated ECU is never heard from, so it is done here once
            // rather than in every script.
            if (parameters.contains("udsRequestId")) {
                IsoTpAddress address;
                address.receiveId =
                    static_cast<std::uint32_t>(parameters.integer("udsRequestId", 0x7E0));
                address.transmitId =
                    static_cast<std::uint32_t>(parameters.integer("udsResponseId", 0x7E8));
                address.channel = static_cast<std::uint8_t>(parameters.integer("channel", 0));
                address.format = address.receiveId > kMaxStandardIdentifier
                        || address.transmitId > kMaxStandardIdentifier
                    ? CanFrameFormat::Extended
                    : CanFrameFormat::Standard;

                node->enableDiagnostics(address, IsoTpConfig{});
            }

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
            static constexpr std::string_view kReserved[] = {
                "script", "scriptPath", "channel", "database", "udsRequestId",
                "udsResponseId"};

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
            .typeName = "sim.restbus",
            .displayName = "Rest Bus",
            .category = "Simulation",
            .description = "Sends every message the rest of the network would send, "
                           "from a database.",
            .inputs = {},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters =
                {
                    ParameterDescriptor{.name = "database",
                                        .displayName = "Database",
                                        .type = ParameterValue::Type::Text,
                                        .required = true,
                                        .description =
                                            "The .dbc describing the network to stand "
                                            "in for."},
                    ParameterDescriptor{
                        .name = "exclude",
                        .displayName = "Real nodes",
                        .type = ParameterValue::Type::Text,
                        .required = false,
                        .description =
                            "Nodes NOT to simulate, comma separated - the ECUs actually "
                            "on the bench. This is the list to fill in."},
                    ParameterDescriptor{
                        .name = "nodes",
                        .displayName = "Simulated nodes",
                        .type = ParameterValue::Type::Text,
                        .required = false,
                        .description =
                            "Nodes to simulate, comma separated. Empty means every node "
                            "the database describes."},
                    ParameterDescriptor{
                        .name = "signals",
                        .displayName = "Driven signals",
                        .type = ParameterValue::Type::Text,
                        .required = false,
                        .description =
                            "Signals that follow a variable rather than holding their "
                            "default: Engine.Speed, Engine.Throttle=throttle_pedal"},
                    ParameterDescriptor{
                        .name = "defaultCycleMs",
                        .displayName = "Default cycle time",
                        .type = ParameterValue::Type::Integer,
                        .required = false,
                        .description =
                            "For messages whose database declares none. Zero - the "
                            "default - leaves them unsent, because a message with no "
                            "cycle time is usually event-triggered and inventing a "
                            "period puts traffic on the bus the real network never "
                            "carries."},
                    ParameterDescriptor{.name = "channel",
                                        .displayName = "Transmit channel",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Channel stamped onto the frames it sends."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            if (Result result = checkRestBus(parameters, nodeId); result.failed()) {
                return result;
            }

            auto node = std::make_unique<RestBusNode>();

            const std::string databasePath =
                resolvePath(context, parameters.text("database"));

            auto database = std::make_shared<CanDatabase>();

            if (Result result = DbcParser::parseFile(databasePath, *database);
                result.failed()) {
                return Result::error(result.code(),
                                     std::format("Node '{}': {}", nodeId,
                                                 std::string{result.message()}));
            }

            node->setDatabase(std::move(database));

            node->setSimulatedNodes(splitNames(parameters.text("nodes", "")));
            node->setExcludedNodes(splitNames(parameters.text("exclude", "")));

            std::vector<RestBusNode::DrivenSignal> driven;

            if (Result result =
                    parseDrivenSignals(parameters.text("signals", ""), nodeId, driven);
                result.failed()) {
                return result;
            }

            node->setDrivenSignals(std::move(driven));
            node->setSystemVariables(context.variables);

            node->setDefaultCycleMs(
                static_cast<std::uint32_t>(parameters.integer("defaultCycleMs", 0)));
            node->setTransmitChannel(
                static_cast<std::uint8_t>(parameters.integer("channel", 0)));

            out = std::move(node);
            return Result::ok();
        },
        checkRestBus);

    // --- Testing ----------------------------------------------------------

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "lua.test",
            .displayName = "Test Sequence",
            .category = "Simulation",
            .description = "A Lua sequence that checks the bus and reports pass or fail.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"frames", PortType::Frames}},
            .parameters =
                {
                    ParameterDescriptor{.name = "script",
                                        .displayName = "Sequence",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description = "The Lua source, held in the project."},
                    ParameterDescriptor{.name = "scriptPath",
                                        .displayName = "Sequence file",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description = "A .lua file to load instead."},
                    ParameterDescriptor{.name = "channel",
                                        .displayName = "Transmit channel",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Channel stamped onto the frames it sends."},
                },
            .acceptsExtraParameters = true,
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            if (Result result = checkLuaScriptChoice(parameters, nodeId); result.failed()) {
                return result;
            }

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

                const std::size_t separator = path.find_last_of("/\\");
                name = separator == std::string::npos ? path : path.substr(separator + 1);
            } else {
                source = parameters.text("script");
            }

            auto node = std::make_unique<LuaTestNode>(
                std::move(source), std::move(name),
                static_cast<std::uint8_t>(parameters.integer("channel", 0)));

            // Where the verdict goes. Null in a graph built without one, and
            // the sequence then still runs and still logs - which is what a
            // headless run wants and what makes the node testable on its own.
            node->setReport(context.testReport);

            static constexpr std::string_view kReserved[] = {"script", "scriptPath", "channel"};

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

            node->setScriptParameters(std::move(scriptParameters));

            if (context.log) {
                node->setLogHandler(context.log);
            }

            out = std::move(node);
            return Result::ok();
        },
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
            .typeName = "j1939.decoder",
            .displayName = "J1939",
            .category = "Transforms",
            .description = "Reassembles J1939 transport, decodes by PGN, and watches "
                           "who is on the bus.",
            .inputs = {PortDescriptor{"frames", PortType::Frames}},
            .outputs = {PortDescriptor{"signals", PortType::Signals}},
            .parameters =
                {
                    ParameterDescriptor{.name = "database",
                                        .displayName = "Database",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description = "Path to a J1939 .dbc file. Matched "
                                                       "by PGN, so a database written for "
                                                       "one source address reads a bench "
                                                       "where the ECU answers from "
                                                       "another."},
                    ParameterDescriptor{
                        .name = "assembleSpn",
                        .displayName = "Assemble the SPN",
                        .type = ParameterValue::Type::Boolean,
                        .required = false,
                        .description = "On, the SPN of a trouble code is assembled the way "
                                       "the current standard packs it. Off, no SPN is "
                                       "produced and the four raw bytes stand - which is "
                                       "what a bus whose ECUs use an older packing needs, "
                                       "because a number read under the wrong convention "
                                       "still looks like an SPN."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            const std::string path = resolvePath(context, parameters.text("database", ""));

            std::shared_ptr<CanDatabase> database;
            std::string label = "J1939";

            // Like the DBC decoder: no database builds a block that decodes
            // nothing rather than refusing to compile, so a canvas can be
            // assembled in any order. Everything else this block does -
            // reassembly, the address table, trouble codes - works without one.
            if (!path.empty()) {
                database = std::make_shared<CanDatabase>();
                if (Result result = DbcParser::parseFile(path, *database); result.failed()) {
                    return Result::error(result.code(),
                                         std::format("Node '{}': {}", nodeId,
                                                     result.message()));
                }

                label = std::format("J1939 - {} ({} messages)",
                                    path.substr(path.find_last_of("/\\") + 1),
                                    database->messageCount());
            }

            auto node = std::make_unique<J1939Node>(std::move(database), std::move(label));

            node->setSpnReading(parameters.boolean("assembleSpn", true)
                                    ? J1939SpnReading::Version4
                                    : J1939SpnReading::RawOnly);

            // Null in a headless build, and the block then keeps its view of
            // the bus to itself rather than copying it for nobody.
            node->setNetwork(context.j1939Network);

            out = std::move(node);
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

            // How long the recording is, so the timeline has a total to draw
            // against. It costs a pass over the file - the format carries no
            // duration, on purpose - and Start is the one moment where that is
            // affordable: it is off the frame path, it happens once, and the
            // alternative is a playback bar with no end.
            //
            // A file that will not summarise is not a reason to refuse the
            // measurement. The replay works without a total; only the handle
            // does not know where it is going.
            if (context.replayControl != nullptr) {
                TraceLogSummary summary;
                if (summarize(path, summary).succeeded()) {
                    context.replayControl->setDurationNs(summary.durationNs);
                }
            }

            out = std::make_unique<LogSourceNode>(std::move(reader),
                                                  parameters.real("speed", 1.0),
                                                  "Log Replay",
                                                  context.replayControl);
            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "uds.client",
            .displayName = "UDS Client",
            .category = "Diagnostics",
            .description = "Talks ISO 14229 to an ECU: sessions, identifiers, DTCs. "
                           "Carries its own ISO-TP transport.",
            .inputs = {PortDescriptor{"frames", PortType::Frames},
                       PortDescriptor{"requests", PortType::Events}},
            .outputs = {PortDescriptor{"frames", PortType::Frames},
                        PortDescriptor{"messages", PortType::Events}},
            .parameters =
                {
                    ParameterDescriptor{.name = "transmitId",
                                        .displayName = "Request ID",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Identifier requests go out on. 2016 (0x7E0) "
                                            "is the legislated tester address."},
                    ParameterDescriptor{.name = "receiveId",
                                        .displayName = "Response ID",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "Identifier the ECU answers on."},
                    ParameterDescriptor{.name = "extendedId",
                                        .displayName = "29-bit identifiers",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description = "Heavy vehicles use 29-bit addresses."},
                    ParameterDescriptor{.name = "channel",
                                        .displayName = "Channel",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "Application channel, 0 for CAN 1."},
                    ParameterDescriptor{.name = "padding",
                                        .displayName = "Pad frames",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description =
                                            "Fill every frame to its full length. Many "
                                            "ECUs ignore a frame that is not padded."},
                    ParameterDescriptor{.name = "canFd",
                                        .displayName = "CAN FD",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description = "Send with FD frames."},
                    ParameterDescriptor{.name = "p2Ms",
                                        .displayName = "P2 timeout (ms)",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "How long the ECU has to answer. 50 ms is the "
                                            "standard's figure."},
                    ParameterDescriptor{.name = "p2StarMs",
                                        .displayName = "P2* timeout (ms)",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "How long after the ECU says it is still "
                                            "working. 5000 ms."},
                    ParameterDescriptor{.name = "keepSessionAlive",
                                        .displayName = "Hold the session open",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description =
                                            "Send TesterPresent so a non-default session "
                                            "does not expire while nobody is asking "
                                            "anything."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            static_cast<void>(nodeId);

            IsoTpAddress address;
            address.transmitId = static_cast<std::uint32_t>(parameters.integer("transmitId", 0x7E0));
            address.receiveId = static_cast<std::uint32_t>(parameters.integer("receiveId", 0x7E8));
            address.format = parameters.boolean("extendedId", false) ? CanFrameFormat::Extended
                                                                     : CanFrameFormat::Standard;
            address.channel = static_cast<std::uint8_t>(parameters.integer("channel", 0));

            IsoTpConfig transport;
            transport.padding = parameters.boolean("padding", true);
            transport.canFd = parameters.boolean("canFd", false);

            UdsTiming timing;
            timing.p2Ms = static_cast<std::uint32_t>(parameters.integer("p2Ms", 50));
            timing.p2StarMs = static_cast<std::uint32_t>(parameters.integer("p2StarMs", 5000));
            timing.keepSessionAlive = parameters.boolean("keepSessionAlive", true);

            out = std::make_unique<UdsClientNode>(address, transport, timing,
                                                  context.diagnosticSession);
            return Result::ok();
        },
        [](const NodeParameters& parameters, std::string_view nodeId) -> Result {
            // P2 is checked because it is the one timing value somebody is
            // tempted to "fix" by typing a bigger number, and a P2 above P2*
            // makes the extended deadline meaningless.
            const std::int64_t p2 = parameters.integer("p2Ms", 50);
            const std::int64_t p2Star = parameters.integer("p2StarMs", 5000);

            if (p2 <= 0 || p2Star <= 0) {
                return Result::error(ErrorCode::InvalidArgument,
                                     std::format("Block '{}': a timeout has to be positive.",
                                                 nodeId));
            }

            if (p2 > p2Star) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Block '{}': P2 ({} ms) is longer than P2* ({} ms). P2* is "
                                "the *extended* deadline the ECU gets after saying it is "
                                "still working, so it cannot be the shorter of the two.",
                                nodeId, p2, p2Star));
            }

            return Result::ok();
        });

    catalog.registerType(
        NodeTypeInfo{
            .typeName = "isotp.transport",
            .displayName = "ISO-TP Transport",
            .category = "Diagnostics",
            .description = "Carries diagnostic messages over CAN: ISO 15765-2 "
                           "segmentation, flow control and timing.",
            .inputs = {PortDescriptor{"frames", PortType::Frames},
                       PortDescriptor{"requests", PortType::Events}},
            .outputs = {PortDescriptor{"frames", PortType::Frames},
                        PortDescriptor{"messages", PortType::Events}},
            .parameters =
                {
                    ParameterDescriptor{.name = "transmitId",
                                        .displayName = "Request ID",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Identifier this end sends on. 2016 (0x7E0) is "
                                            "the legislated tester address."},
                    ParameterDescriptor{.name = "receiveId",
                                        .displayName = "Response ID",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Identifier the ECU answers on. 2024 (0x7E8) "
                                            "answers 0x7E0."},
                    ParameterDescriptor{.name = "extendedId",
                                        .displayName = "29-bit identifiers",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description =
                                            "Heavy vehicles use 29-bit addresses; cars "
                                            "usually do not."},
                    ParameterDescriptor{.name = "channel",
                                        .displayName = "Channel",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description = "Application channel, 0 for CAN 1."},
                    ParameterDescriptor{.name = "blockSize",
                                        .displayName = "Block size",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Frames this end accepts before asking again. "
                                            "0 means send it all."},
                    ParameterDescriptor{.name = "separationTimeMs",
                                        .displayName = "Separation time (ms)",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "Gap this end needs between consecutive "
                                            "frames, 0 to 127."},
                    ParameterDescriptor{.name = "padding",
                                        .displayName = "Pad frames",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description =
                                            "Fill every frame to its full length. Many "
                                            "ECUs ignore a frame that is not padded."},
                    ParameterDescriptor{.name = "canFd",
                                        .displayName = "CAN FD",
                                        .type = ParameterValue::Type::Boolean,
                                        .required = false,
                                        .description = "Send with FD frames."},
                    ParameterDescriptor{.name = "request",
                                        .displayName = "Request",
                                        .type = ParameterValue::Type::Text,
                                        .required = false,
                                        .description =
                                            "Bytes to send, as hex: \"22 F1 90\". Left "
                                            "empty, the block only carries what an "
                                            "upstream block asks it to."},
                    ParameterDescriptor{.name = "requestIntervalMs",
                                        .displayName = "Repeat every (ms)",
                                        .type = ParameterValue::Type::Integer,
                                        .required = false,
                                        .description =
                                            "0 asks once, at Start. Anything else repeats "
                                            "- which is what a tester watching a value "
                                            "while somebody drives does."},
                },
        },
        [](const NodeParameters& parameters, const NodeBuildContext& context,
           std::string_view nodeId, std::unique_ptr<IPipelineNode>& out) -> Result {
            static_cast<void>(context);

            IsoTpAddress address;
            address.transmitId = static_cast<std::uint32_t>(parameters.integer("transmitId", 0x7E0));
            address.receiveId = static_cast<std::uint32_t>(parameters.integer("receiveId", 0x7E8));
            address.format = parameters.boolean("extendedId", false) ? CanFrameFormat::Extended
                                                                     : CanFrameFormat::Standard;
            address.channel = static_cast<std::uint8_t>(parameters.integer("channel", 0));

            IsoTpConfig config;
            config.blockSize = static_cast<std::uint8_t>(parameters.integer("blockSize", 0));
            config.separationTime =
                static_cast<std::uint8_t>(parameters.integer("separationTimeMs", 0));
            config.padding = parameters.boolean("padding", true);
            config.canFd = parameters.boolean("canFd", false);

            std::vector<std::uint8_t> request;

            if (const std::string text = parameters.text("request", ""); !text.empty()) {
                if (!parseHexBytes(text, request)) {
                    return Result::error(
                        ErrorCode::InvalidArgument,
                        std::format("Block '{}': '{}' is not a whole number of hex bytes.",
                                    nodeId, text));
                }
            }

            out = std::make_unique<IsoTpNode>(
                address, config, std::move(request),
                static_cast<std::uint32_t>(parameters.integer("requestIntervalMs", 0)));

            return Result::ok();
        },
        [](const NodeParameters& parameters, std::string_view nodeId) -> Result {
            // Checked while the block is on screen, not at Start. Both of these
            // are values somebody types, and both have a range that is part of
            // the protocol rather than of this implementation.
            const std::int64_t separation = parameters.integer("separationTimeMs", 0);

            if (separation < 0 || separation > 127) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Block '{}': a separation time is 0 to 127 ms. The "
                                "microsecond range of the standard's encoding is not "
                                "something a person needs to type.",
                                nodeId));
            }

            const std::int64_t blockSize = parameters.integer("blockSize", 0);

            if (blockSize < 0 || blockSize > 255) {
                return Result::error(ErrorCode::InvalidArgument,
                                     std::format("Block '{}': a block size is 0 to 255.",
                                                 nodeId));
            }

            std::vector<std::uint8_t> ignored;

            if (const std::string text = parameters.text("request", "");
                !text.empty() && !parseHexBytes(text, ignored)) {
                return Result::error(
                    ErrorCode::InvalidArgument,
                    std::format("Block '{}': '{}' is not a whole number of hex bytes - "
                                "\"22 F1 90\" is three bytes, \"22 F1 9\" is not two and a "
                                "half.",
                                nodeId, text));
            }

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
