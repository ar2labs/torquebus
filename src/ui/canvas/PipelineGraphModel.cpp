// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/PipelineGraphModel.h"

#include "ui/canvas/CanvasLayout.h"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/NodeData>
#include <QtNodes/StyleCollection>

#include <QJsonObject>
#include <QPointF>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace torquebus::ui {
namespace {

constexpr std::size_t kNoPort = static_cast<std::size_t>(-1);

[[nodiscard]] bool isBusBlock(const NodeDescription* block)
{
    return block != nullptr && block->typeName == kCanBusType;
}

/// The other end of a wire as seen from `name`, or nullptr when the wire does
/// not touch it.
[[nodiscard]] const std::string* otherEnd(const EdgeDescription& edge, const std::string& name)
{
    if (edge.fromNode == name) {
        return &edge.toNode;
    }
    if (edge.toNode == name) {
        return &edge.fromNode;
    }
    return nullptr;
}

/// The colour family a port type belongs to, as QtNodes understands it.
///
/// QtNodes matches connection colours by NodeDataType::id, so these strings end
/// up deciding what colour a wire is drawn in. They are the same names
/// PortType::toString gives, which keeps one vocabulary between the canvas, the
/// validation messages and the architecture document.
[[nodiscard]] QtNodes::NodeDataType dataTypeFor(PortType type)
{
    const std::string_view text = toString(type);
    const QString name = QString::fromLatin1(text.data(), static_cast<int>(text.size()));

    // id is what QtNodes matches on (connection colour, type compatibility);
    // name is what it draws next to the port.
    return QtNodes::NodeDataType{name.toLower(), name};
}

} // namespace

PipelineGraphModel::PipelineGraphModel(GraphDescription& description,
                                       const NodeCatalog& catalog,
                                       QObject* parent)
    : m_description{description}
    , m_catalog{catalog}
{
    // setParent() and not a base initialiser: AbstractGraphModel declares no
    // constructor at all, so it has only the implicit default one and there is
    // nothing to pass `parent` to.
    setParent(parent);

    reload();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

QtNodes::NodeId PipelineGraphModel::adopt(const std::string& id)
{
    const QtNodes::NodeId canvas = m_nextNodeId++;
    m_toDescription.emplace(canvas, id);
    m_toCanvas.emplace(id, canvas);
    return canvas;
}

void PipelineGraphModel::reload()
{
    m_toDescription.clear();
    m_toCanvas.clear();
    m_nextNodeId = 0;

    for (const NodeDescription& node : m_description.nodes()) {
        adopt(node.id);
    }

    // A card left open for a block the new project does not have would stay
    // open for the next block to get that id.
    std::erase_if(m_expanded,
                  [this](const std::string& id) { return m_description.find(id) == nullptr; });

    fitAllBuses();

    Q_EMIT modelReset();
}

QtNodes::NodeId PipelineGraphModel::newNodeId()
{
    return m_nextNodeId++;
}

std::unordered_set<QtNodes::NodeId> PipelineGraphModel::allNodeIds() const
{
    std::unordered_set<QtNodes::NodeId> ids;
    ids.reserve(m_toDescription.size());

    for (const auto& [canvas, name] : m_toDescription) {
        ids.insert(canvas);
    }

    return ids;
}

bool PipelineGraphModel::nodeExists(QtNodes::NodeId nodeId) const
{
    return description(nodeId) != nullptr;
}

std::string PipelineGraphModel::descriptionId(QtNodes::NodeId nodeId) const
{
    const auto it = m_toDescription.find(nodeId);
    return it == m_toDescription.end() ? std::string{} : it->second;
}

QtNodes::NodeId PipelineGraphModel::canvasId(const std::string& descriptionId) const
{
    const auto it = m_toCanvas.find(descriptionId);
    return it == m_toCanvas.end() ? QtNodes::InvalidNodeId : it->second;
}

const NodeDescription* PipelineGraphModel::description(QtNodes::NodeId nodeId) const
{
    const auto it = m_toDescription.find(nodeId);
    return it == m_toDescription.end() ? nullptr : m_description.find(it->second);
}

NodeDescription* PipelineGraphModel::description(QtNodes::NodeId nodeId)
{
    // const_cast rather than a second lookup written twice: the map and the
    // vector search are identical, and duplicating them is how the two drift.
    return const_cast<NodeDescription*>(std::as_const(*this).description(nodeId));
}

const NodeTypeInfo* PipelineGraphModel::typeInfo(QtNodes::NodeId nodeId) const
{
    const NodeDescription* node = description(nodeId);
    return node == nullptr ? nullptr : m_catalog.find(node->typeName);
}

const NodeDescription* PipelineGraphModel::node(QtNodes::NodeId nodeId) const
{
    return description(nodeId);
}

const NodeTypeInfo* PipelineGraphModel::info(QtNodes::NodeId nodeId) const
{
    return typeInfo(nodeId);
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

std::unordered_set<QtNodes::ConnectionId>
PipelineGraphModel::allConnectionIds(QtNodes::NodeId nodeId) const
{
    std::unordered_set<QtNodes::ConnectionId> result;

    const std::string name = descriptionId(nodeId);
    if (name.empty()) {
        return result;
    }

    for (const EdgeDescription& edge : m_description.edges()) {
        if (edge.fromNode != name && edge.toNode != name) {
            continue;
        }

        const QtNodes::NodeId from = canvasId(edge.fromNode);
        const QtNodes::NodeId to = canvasId(edge.toNode);

        if (from == QtNodes::InvalidNodeId || to == QtNodes::InvalidNodeId) {
            continue;
        }

        result.insert(QtNodes::ConnectionId{from,
                                            static_cast<QtNodes::PortIndex>(edge.fromPort),
                                            to,
                                            static_cast<QtNodes::PortIndex>(edge.toPort)});
    }

    return result;
}

std::unordered_set<QtNodes::ConnectionId> PipelineGraphModel::connections(
    QtNodes::NodeId nodeId, QtNodes::PortType portType, QtNodes::PortIndex index) const
{
    std::unordered_set<QtNodes::ConnectionId> result;

    for (const QtNodes::ConnectionId& id : allConnectionIds(nodeId)) {
        const bool matches = portType == QtNodes::PortType::Out
                                 ? (id.outNodeId == nodeId && id.outPortIndex == index)
                                 : (id.inNodeId == nodeId && id.inPortIndex == index);

        if (matches) {
            result.insert(id);
        }
    }

    return result;
}

bool PipelineGraphModel::connectionExists(QtNodes::ConnectionId connectionId) const
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    return std::any_of(m_description.edges().begin(),
                       m_description.edges().end(),
                       [&](const EdgeDescription& edge) {
                           return edge.fromNode == from
                                  && edge.fromPort == connectionId.outPortIndex && edge.toNode == to
                                  && edge.toPort == connectionId.inPortIndex;
                       });
}

bool PipelineGraphModel::connectionPossible(QtNodes::ConnectionId connectionId) const
{
    const NodeTypeInfo* fromType = typeInfo(connectionId.outNodeId);
    const NodeTypeInfo* toType = typeInfo(connectionId.inNodeId);

    if (fromType == nullptr || toType == nullptr) {
        return false;
    }

    // A node wired to itself would be a cycle, and compile() would reject it
    // later with a message about a cycle. Refusing the drag is friendlier than
    // accepting it and failing at Start.
    if (connectionId.outNodeId == connectionId.inNodeId) {
        return false;
    }

    if (connectionId.outPortIndex >= fromType->outputs.size()
        || connectionId.inPortIndex >= toType->inputs.size()) {
        return false;
    }

    if (fromType->outputs[connectionId.outPortIndex].type
        != toType->inputs[connectionId.inPortIndex].type) {
        return false;
    }

    // A bus to a bus is something the graph could build - a gateway - and
    // nothing here could draw: a wire between two rails has no end to stand on.
    if (isBus(connectionId.outNodeId) && isBus(connectionId.inNodeId)) {
        return false;
    }

    // A fan-in port takes any number of wires, so the question there is only
    // whether this one is already drawn. Answering yes to a wire that exists
    // would hand QtNodes a connect command whose undo deletes the original.
    if (toType->inputs[connectionId.inPortIndex].fanIn) {
        return !connectionExists(connectionId);
    }

    // One edge per input. Enforced here as well as in GraphDescription because
    // this is the copy the user feels: the wire simply refuses to land.
    return connections(connectionId.inNodeId, QtNodes::PortType::In, connectionId.inPortIndex)
        .empty();
}

bool PipelineGraphModel::addEdge(QtNodes::ConnectionId connectionId)
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    if (from.empty() || to.empty() || connectionExists(connectionId)
        || !connectionPossible(connectionId)) {
        return false;
    }

    m_description.addEdge(
        EdgeDescription{from, connectionId.outPortIndex, to, connectionId.inPortIndex});

    Q_EMIT connectionCreated(connectionId);
    return true;
}

bool PipelineGraphModel::removeEdge(QtNodes::ConnectionId connectionId)
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    const std::size_t removed =
        std::erase_if(m_description.edges(), [&](const EdgeDescription& edge) {
            return edge.fromNode == from && edge.fromPort == connectionId.outPortIndex
                   && edge.toNode == to && edge.toPort == connectionId.inPortIndex;
        });

    if (removed == 0) {
        return false;
    }

    Q_EMIT connectionDeleted(connectionId);
    return true;
}

std::size_t PipelineGraphModel::firstFramesPort(QtNodes::NodeId nodeId, bool input) const
{
    const NodeTypeInfo* type = typeInfo(nodeId);
    if (type == nullptr) {
        return kNoPort;
    }

    const std::vector<PortDescriptor>& ports = input ? type->inputs : type->outputs;
    for (std::size_t index = 0; index < ports.size(); ++index) {
        if (ports[index].type == PortType::Frames) {
            return index;
        }
    }
    return kNoPort;
}

std::size_t PipelineGraphModel::firstFreeFramesInput(QtNodes::NodeId nodeId) const
{
    const NodeTypeInfo* type = typeInfo(nodeId);
    if (type == nullptr) {
        return kNoPort;
    }

    std::size_t first = kNoPort;

    for (std::size_t index = 0; index < type->inputs.size(); ++index) {
        const PortDescriptor& port = type->inputs[index];
        if (port.type != PortType::Frames) {
            continue;
        }

        first = first == kNoPort ? index : first;

        if (port.fanIn
            || connections(nodeId, QtNodes::PortType::In, static_cast<QtNodes::PortIndex>(index))
                   .empty()) {
            return index;
        }
    }

    return first;
}

bool PipelineGraphModel::completionOf(QtNodes::ConnectionId connectionId,
                                      QtNodes::ConnectionId& completion) const
{
    const bool toBus = isBus(connectionId.inNodeId);
    const bool fromBus = isBus(connectionId.outNodeId);

    if (toBus == fromBus) {
        return false; // Not a drop at all.
    }

    const QtNodes::NodeId module = toBus ? connectionId.outNodeId : connectionId.inNodeId;
    const QtNodes::NodeId bus = toBus ? connectionId.inNodeId : connectionId.outNodeId;

    // Only a module that speaks on a bus as well as listening to it has a second
    // half. A filter has a frames input and a frames output too, and wiring its
    // output back onto the bus it listens to is the loop talksOnBus is for.
    const NodeTypeInfo* type = typeInfo(module);
    if (type == nullptr || !type->talksOnBus) {
        return false;
    }

    if (toBus) {
        const std::size_t input = firstFramesPort(module, true);
        if (input == kNoPort) {
            return false;
        }
        completion = QtNodes::ConnectionId{bus, 0, module, static_cast<QtNodes::PortIndex>(input)};
    } else {
        const std::size_t output = firstFramesPort(module, false);
        if (output == kNoPort) {
            return false;
        }
        completion = QtNodes::ConnectionId{module, static_cast<QtNodes::PortIndex>(output), bus, 0};
    }

    return !connectionExists(completion) && connectionPossible(completion);
}

void PipelineGraphModel::addConnection(QtNodes::ConnectionId connectionId)
{
    if (!addEdge(connectionId)) {
        return;
    }

    // One drop is one thing to the person drawing it: dropping an ECU on the bus
    // is wiring it to listen and to speak, and asking for the second wire by
    // hand is asking them to know the graph holds a drop as two.
    QtNodes::ConnectionId completion{};
    if (m_pairing && completionOf(connectionId, completion)) {
        addEdge(completion);
    }

    if (isBusTap(connectionId)) {
        fitBus(isBus(connectionId.outNodeId) ? connectionId.outNodeId : connectionId.inNodeId);
    }
}

bool PipelineGraphModel::deleteConnection(QtNodes::ConnectionId connectionId)
{
    const bool tap = isBusTap(connectionId);
    const bool toBus = isBus(connectionId.inNodeId);
    const QtNodes::NodeId bus = toBus ? connectionId.inNodeId : connectionId.outNodeId;
    const QtNodes::NodeId module = toBus ? connectionId.outNodeId : connectionId.inNodeId;

    if (!removeEdge(connectionId)) {
        return false;
    }

    if (tap) {
        // And the other half of the drop: a module detached from a bus that is
        // still, for the other direction, wired to it is not detached.
        if (m_pairing) {
            for (const QtNodes::ConnectionId& other : tapsBetween(module, bus)) {
                if (isBus(other.inNodeId) != toBus) {
                    removeEdge(other);
                }
            }
        }

        fitBus(bus);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

QtNodes::NodeId PipelineGraphModel::addNode(QString nodeType)
{
    const std::string typeName = nodeType.toStdString();

    if (!m_catalog.contains(typeName)) {
        // The palette only offers registered types, so reaching this means a
        // caller invented one. Refusing beats creating a node that cannot build.
        return QtNodes::InvalidNodeId;
    }

    // The id is derived from the type - "lua.ecu" becomes "lua_ecu",
    // "lua_ecu_2" - so a project file reads as a description of the pipeline
    // rather than a list of node_1 through node_12.
    std::string base = typeName;
    std::replace(base.begin(), base.end(), '.', '_');

    NodeDescription node;
    node.id = m_description.uniqueId(base);
    node.typeName = typeName;

    m_description.addNode(node);

    const QtNodes::NodeId canvas = adopt(node.id);
    Q_EMIT nodeCreated(canvas);

    return canvas;
}

bool PipelineGraphModel::deleteNode(QtNodes::NodeId nodeId)
{
    const std::string name = descriptionId(nodeId);
    if (name.empty()) {
        return false;
    }

    // The buses it was on, while there is still a wire to find them by: they
    // are as long as their modules are spread out, and are one shorter now.
    const std::vector<QtNodes::NodeId> buses = busesOf(nodeId);

    // The scene has to be told about each connection separately, before the
    // node goes: it holds graphics items for them, and a ConnectionGraphicsObject
    // whose node has been deleted is a dangling item that crashes on the next
    // paint. GraphDescription::removeNode drops the edges too, but silently -
    // which is right for the data and wrong for the scene.
    for (const QtNodes::ConnectionId& id : allConnectionIds(nodeId)) {
        Q_EMIT connectionDeleted(id);
    }

    m_description.removeNode(name);

    m_toCanvas.erase(name);
    m_toDescription.erase(nodeId);
    m_expanded.erase(name);

    Q_EMIT nodeDeleted(nodeId);

    for (const QtNodes::NodeId bus : buses) {
        fitBus(bus);
    }

    return true;
}

QVariant PipelineGraphModel::nodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role) const
{
    const NodeDescription* node = description(nodeId);
    const NodeTypeInfo* info = typeInfo(nodeId);

    if (node == nullptr || info == nullptr) {
        return {};
    }

    switch (role) {
    case QtNodes::NodeRole::Type:
        return QString::fromStdString(node->typeName);

    case QtNodes::NodeRole::Position:
        return QPointF{node->x, node->y};

    case QtNodes::NodeRole::Size:
        return blockSize(nodeId);

    case QtNodes::NodeRole::CaptionVisible:
        return true;

    case QtNodes::NodeRole::Caption:
        // The node's own id, not the type's display name. Two Lua ECUs both
        // captioned "Lua ECU" would be indistinguishable on the canvas, and the
        // id is what every error message names.
        return QString::fromStdString(node->id);

    case QtNodes::NodeRole::InPortCount:
        return static_cast<unsigned int>(info->inputs.size());

    case QtNodes::NodeRole::OutPortCount:
        return static_cast<unsigned int>(info->outputs.size());

    case QtNodes::NodeRole::Widget:
        // No embedded widgets. A node is a block with ports; its settings live
        // in the Properties panel, which has room for them and does not have to
        // be zoomed into to be read.
        //
        // An empty QVariant rather than a null QWidget*: QtNodes reads this as
        // nodeData<QWidget*>(), and .value<QWidget*>() on an empty QVariant is
        // already nullptr.
        break;

    case QtNodes::NodeRole::Style:
        // QtNodes constructs a NodeStyle from this JSON on every node creation
        // and paint pass; returning an empty QVariant leaves Opacity = 0.0 and
        // makes the entire NodeGraphicsObject invisible.
        return QtNodes::StyleCollection::nodeStyle().toJson().toVariantMap();

    case QtNodes::NodeRole::InternalData:
    case QtNodes::NodeRole::ValidationState:
    case QtNodes::NodeRole::ProcessingStatus:
        break;
    }

    return {};
}

bool PipelineGraphModel::setNodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role, QVariant value)
{
    NodeDescription* node = description(nodeId);
    if (node == nullptr) {
        return false;
    }

    switch (role) {
    case QtNodes::NodeRole::Position: {
        // The one write the canvas makes constantly. It goes into the
        // description, which is what makes a dragged block still be there
        // tomorrow: the position is part of the project, not of the view.
        QPointF position = value.value<QPointF>();

        // A rail cannot be put to the right of a module it carries: it would not
        // reach it. Held there, and the rail follows the module when that is
        // what moves.
        const bool bus = isBusBlock(node);
        if (bus) {
            position.setX(std::min(position.x(), busLeftBound(*node)));
        }

        node->x = position.x();
        node->y = position.y();

        // Part of a batch: the position is written, and the scene hears of it once the
        // batch is over.
        if (m_moveDepth > 0) {
            m_moved.insert(nodeId);
            return true;
        }

        Q_EMIT nodePositionUpdated(nodeId);

        if (bus) {
            Q_EMIT nodeUpdated(nodeId); // Its width is how far it is from its modules.
        } else {
            fitBusesOf(nodeId);
        }
        return true;
    }

    case QtNodes::NodeRole::Caption: {
        // Renaming a node rewrites its id, and every edge that names it.
        const std::string requested = value.toString().toStdString();
        if (requested.empty() || requested == node->id) {
            return false;
        }

        if (m_description.find(requested) != nullptr) {
            return false; // Ids address edges; a duplicate would make one ambiguous.
        }

        const std::string previous = node->id;

        for (EdgeDescription& edge : m_description.edges()) {
            if (edge.fromNode == previous) {
                edge.fromNode = requested;
            }
            if (edge.toNode == previous) {
                edge.toNode = requested;
            }
        }

        node->id = requested;

        if (m_expanded.erase(previous) != 0) {
            m_expanded.insert(requested);
        }

        m_toCanvas.erase(previous);
        m_toCanvas.emplace(requested, nodeId);
        m_toDescription[nodeId] = requested;

        Q_EMIT nodeUpdated(nodeId);
        return true;
    }

    default:
        break;
    }

    return false;
}

QVariant PipelineGraphModel::portData(QtNodes::NodeId nodeId,
                                      QtNodes::PortType portType,
                                      QtNodes::PortIndex index,
                                      QtNodes::PortRole role) const
{
    const NodeTypeInfo* info = typeInfo(nodeId);
    if (info == nullptr) {
        return {};
    }

    const std::vector<PortDescriptor>& ports =
        portType == QtNodes::PortType::Out ? info->outputs : info->inputs;

    if (index >= ports.size()) {
        return {};
    }

    const PortDescriptor& port = ports[index];

    switch (role) {
    case QtNodes::PortRole::DataType:
        // A NodeDataType, not a QString - despite what the doc comment on
        // AbstractGraphModel::portData says. Every painter in QtNodes unwraps
        // this with .value<NodeDataType>(), and a QString there yields a
        // default-constructed one: ports with no type name and every wire drawn
        // in the same colour, with nothing to say why.
        return QVariant::fromValue(dataTypeFor(port.type));

    case QtNodes::PortRole::Caption:
        return QString::fromLatin1(port.name.data(), static_cast<int>(port.name.size()));

    case QtNodes::PortRole::CaptionVisible:
        return true;

    case QtNodes::PortRole::ConnectionPolicyRole:
        // The typed-graph rule, expressed where the user meets it: an output
        // may feed any number of consumers, an input takes exactly one, because
        // merging two streams needs a policy - except the one a bus has, where
        // each wire is given a transmit of its own and nothing is merged.
        return QVariant::fromValue(portType == QtNodes::PortType::Out || port.fanIn
                                       ? QtNodes::ConnectionPolicy::Many
                                       : QtNodes::ConnectionPolicy::One);

    case QtNodes::PortRole::Data:
        break;
    }

    return {};
}

bool PipelineGraphModel::setPortData(
    QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex, const QVariant&, QtNodes::PortRole)
{
    // Ports come from the node's type and are not editable. A node with a
    // different shape is a different type.
    return false;
}

QJsonObject PipelineGraphModel::saveNode(QtNodes::NodeId nodeId) const
{
    const NodeDescription* node = description(nodeId);
    if (node == nullptr) {
        return {};
    }

    QJsonObject positionJson;
    positionJson[QStringLiteral("x")] = node->x;
    positionJson[QStringLiteral("y")] = node->y;

    QJsonObject paramsJson;
    for (const auto& [key, param] : node->parameters.values()) {
        const QString qKey = QString::fromStdString(key);
        QJsonObject entry;
        switch (param.type()) {
        case ParameterValue::Type::Integer:
            entry[QStringLiteral("kind")] = QStringLiteral("int");
            entry[QStringLiteral("value")] = static_cast<qint64>(param.asInteger());
            break;
        case ParameterValue::Type::Real:
            entry[QStringLiteral("kind")] = QStringLiteral("real");
            entry[QStringLiteral("value")] = param.asReal();
            break;
        case ParameterValue::Type::Boolean:
            entry[QStringLiteral("kind")] = QStringLiteral("bool");
            entry[QStringLiteral("value")] = param.asBoolean();
            break;
        case ParameterValue::Type::Text:
            entry[QStringLiteral("kind")] = QStringLiteral("text");
            entry[QStringLiteral("value")] = QString::fromStdString(param.asText());
            break;
        }
        paramsJson[qKey] = entry;
    }

    QJsonObject internalData;
    internalData[QStringLiteral("descriptionId")] = QString::fromStdString(node->id);
    internalData[QStringLiteral("typeName")] = QString::fromStdString(node->typeName);
    internalData[QStringLiteral("enabled")] = node->enabled;
    internalData[QStringLiteral("parameters")] = paramsJson;

    QJsonObject root;
    root[QStringLiteral("id")] = static_cast<qint64>(nodeId);
    root[QStringLiteral("position")] = positionJson;
    root[QStringLiteral("internal-data")] = internalData;
    return root;
}

void PipelineGraphModel::loadNode(const QJsonObject& nodeJson)
{
    const QJsonObject internalData = nodeJson[QStringLiteral("internal-data")].toObject();
    const std::string typeName = internalData[QStringLiteral("typeName")].toString().toStdString();
    if (typeName.empty() || !m_catalog.contains(typeName)) {
        return;
    }

    std::string requestedId =
        internalData[QStringLiteral("descriptionId")].toString().toStdString();
    if (requestedId.empty() || m_description.find(requestedId) != nullptr) {
        std::string base = typeName;
        std::replace(base.begin(), base.end(), '.', '_');
        requestedId = m_description.uniqueId(base);
    }

    NodeDescription node;
    node.id = requestedId;
    node.typeName = typeName;
    node.enabled = internalData[QStringLiteral("enabled")].toBool(true);

    const QJsonObject positionJson = nodeJson[QStringLiteral("position")].toObject();
    node.x = positionJson[QStringLiteral("x")].toDouble(0.0);
    node.y = positionJson[QStringLiteral("y")].toDouble(0.0);

    const QJsonObject paramsJson = internalData[QStringLiteral("parameters")].toObject();
    for (auto it = paramsJson.begin(); it != paramsJson.end(); ++it) {
        const std::string key = it.key().toStdString();
        const QJsonObject entry = it.value().toObject();
        const QString kind = entry[QStringLiteral("kind")].toString();
        if (kind == QStringLiteral("int")) {
            node.parameters.set(key,
                                ParameterValue::fromInteger(static_cast<std::int64_t>(
                                    entry[QStringLiteral("value")].toInteger())));
        } else if (kind == QStringLiteral("real")) {
            node.parameters.set(
                key, ParameterValue::fromReal(entry[QStringLiteral("value")].toDouble()));
        } else if (kind == QStringLiteral("bool")) {
            node.parameters.set(
                key, ParameterValue::fromBoolean(entry[QStringLiteral("value")].toBool()));
        } else if (kind == QStringLiteral("text")) {
            node.parameters.set(
                key,
                ParameterValue::fromText(entry[QStringLiteral("value")].toString().toStdString()));
        }
    }

    m_description.addNode(node);

    QtNodes::NodeId canvas = static_cast<QtNodes::NodeId>(
        nodeJson[QStringLiteral("id")].toInteger(static_cast<qint64>(m_nextNodeId)));
    if (m_toDescription.find(canvas) != m_toDescription.end()) {
        canvas = m_nextNodeId++;
    } else if (canvas >= m_nextNodeId) {
        m_nextNodeId = canvas + 1;
    }

    m_toDescription.emplace(canvas, node.id);
    m_toCanvas.emplace(node.id, canvas);

    Q_EMIT nodeCreated(canvas);
}

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

double PipelineGraphModel::blockWidth(const NodeDescription& block) const
{
    if (isBusBlock(&block)) {
        return busWidth(block);
    }
    return m_expanded.contains(block.id) ? layout::kCardWidth : layout::kChipWidth;
}

QSize PipelineGraphModel::blockSize(QtNodes::NodeId nodeId) const
{
    const NodeDescription* block = description(nodeId);
    if (block == nullptr) {
        return {};
    }

    if (isBusBlock(block)) {
        return QSize{static_cast<int>(std::ceil(busWidth(*block))),
                     static_cast<int>(layout::kRailHeight)};
    }

    if (m_expanded.contains(block->id)) {
        return QSize{static_cast<int>(layout::kCardWidth), static_cast<int>(layout::kCardHeight)};
    }

    return QSize{static_cast<int>(layout::kChipWidth), static_cast<int>(layout::kChipHeight)};
}

bool PipelineGraphModel::isExpanded(QtNodes::NodeId nodeId) const
{
    const NodeDescription* block = description(nodeId);
    return block != nullptr && m_expanded.contains(block->id);
}

void PipelineGraphModel::setExpanded(QtNodes::NodeId nodeId, bool expanded)
{
    const NodeDescription* block = description(nodeId);
    if (block == nullptr || isBusBlock(block) || isExpanded(nodeId) == expanded) {
        return;
    }

    if (expanded) {
        m_expanded.insert(block->id);
    } else {
        m_expanded.erase(block->id);
    }

    Q_EMIT nodeUpdated(nodeId);
    fitBusesOf(nodeId);
}

void PipelineGraphModel::setAllExpanded(bool expanded)
{
    for (const NodeDescription& block : m_description.nodes()) {
        if (!isBusBlock(&block)) {
            setExpanded(canvasId(block.id), expanded);
        }
    }
}

// ---------------------------------------------------------------------------
// The bus
// ---------------------------------------------------------------------------

bool PipelineGraphModel::isBus(QtNodes::NodeId nodeId) const
{
    return isBusBlock(description(nodeId));
}

bool PipelineGraphModel::isBusTap(QtNodes::ConnectionId connectionId) const
{
    return isBus(connectionId.outNodeId) != isBus(connectionId.inNodeId);
}

std::int64_t PipelineGraphModel::busChannel(QtNodes::NodeId bus) const
{
    const NodeDescription* block = description(bus);
    return block == nullptr ? 0 : block->parameters.integer("channel", 0);
}

std::vector<QtNodes::NodeId> PipelineGraphModel::busesOf(QtNodes::NodeId module) const
{
    std::vector<QtNodes::NodeId> buses;

    const std::string name = descriptionId(module);
    if (name.empty() || isBus(module)) {
        return buses;
    }

    for (const EdgeDescription& edge : m_description.edges()) {
        const std::string* other = otherEnd(edge, name);
        if (other == nullptr) {
            continue;
        }

        const QtNodes::NodeId candidate = canvasId(*other);
        if (candidate != QtNodes::InvalidNodeId && isBus(candidate)
            && std::ranges::find(buses, candidate) == buses.end()) {
            buses.push_back(candidate);
        }
    }

    return buses;
}

std::vector<QtNodes::NodeId> PipelineGraphModel::modulesOn(QtNodes::NodeId bus) const
{
    std::vector<QtNodes::NodeId> modules;

    const std::string name = descriptionId(bus);
    if (name.empty() || !isBus(bus)) {
        return modules;
    }

    for (const EdgeDescription& edge : m_description.edges()) {
        const std::string* other = otherEnd(edge, name);
        if (other == nullptr) {
            continue;
        }

        const QtNodes::NodeId candidate = canvasId(*other);
        if (candidate != QtNodes::InvalidNodeId && !isBus(candidate)
            && std::ranges::find(modules, candidate) == modules.end()) {
            modules.push_back(candidate);
        }
    }

    return modules;
}

std::vector<QtNodes::ConnectionId> PipelineGraphModel::tapsBetween(QtNodes::NodeId module,
                                                                   QtNodes::NodeId bus) const
{
    std::vector<QtNodes::ConnectionId> taps;

    const std::string moduleName = descriptionId(module);
    const std::string busName = descriptionId(bus);
    if (moduleName.empty() || busName.empty()) {
        return taps;
    }

    for (const EdgeDescription& edge : m_description.edges()) {
        if (edge.fromNode == busName && edge.toNode == moduleName) {
            taps.push_back(QtNodes::ConnectionId{bus,
                                                 static_cast<QtNodes::PortIndex>(edge.fromPort),
                                                 module,
                                                 static_cast<QtNodes::PortIndex>(edge.toPort)});
        } else if (edge.fromNode == moduleName && edge.toNode == busName) {
            taps.push_back(QtNodes::ConnectionId{module,
                                                 static_cast<QtNodes::PortIndex>(edge.fromPort),
                                                 bus,
                                                 static_cast<QtNodes::PortIndex>(edge.toPort)});
        }
    }

    return taps;
}

bool PipelineGraphModel::canAttachToBus(QtNodes::NodeId module) const
{
    return nodeExists(module) && !isBus(module)
           && (firstFramesPort(module, true) != kNoPort
               || firstFramesPort(module, false) != kNoPort);
}

bool PipelineGraphModel::attachToBus(QtNodes::NodeId module, QtNodes::NodeId bus)
{
    if (!isBus(bus) || !canAttachToBus(module)) {
        return false;
    }

    if (!tapsBetween(module, bus).empty()) {
        return true; // Already on it.
    }

    // The first input with room on it: a module with several - the Frame Merge - is wired to the
    // next one by each bus it is dropped on, and not refused for the one that the first took.
    const std::size_t input = firstFreeFramesInput(module);
    const std::size_t output = firstFramesPort(module, false);

    // Listening first, because every module that is on a bus is listening to it
    // and the drop completes itself with the speaking half when it should.
    if (input != kNoPort) {
        const QtNodes::ConnectionId listening{
            bus, 0, module, static_cast<QtNodes::PortIndex>(input)};

        if (connectionPossible(listening)) {
            addConnection(listening);
            return connectionExists(listening);
        }
    }

    // Its input is taken by something else. A module that speaks can still be
    // heard, and a bus it is deaf to is better than the drop not being made.
    const NodeTypeInfo* type = typeInfo(module);
    if (output != kNoPort && (input == kNoPort || (type != nullptr && type->talksOnBus))) {
        const QtNodes::ConnectionId speaking{
            module, static_cast<QtNodes::PortIndex>(output), bus, 0};

        if (connectionPossible(speaking)) {
            addConnection(speaking);
            return connectionExists(speaking);
        }
    }

    return false;
}

void PipelineGraphModel::detachFromBus(QtNodes::NodeId module, QtNodes::NodeId bus)
{
    bool removedAny = false;

    for (const QtNodes::ConnectionId& tap : tapsBetween(module, bus)) {
        removedAny = removeEdge(tap) || removedAny;
    }

    if (removedAny) {
        fitBus(bus);
    }
}

PipelineGraphModel::BusDirection PipelineGraphModel::busDirection(QtNodes::NodeId module,
                                                                  QtNodes::NodeId bus) const
{
    BusDirection direction;

    for (const QtNodes::ConnectionId& tap : tapsBetween(module, bus)) {
        if (tap.outNodeId == bus) {
            direction.listens = true;
        } else {
            direction.talks = true;
        }
    }

    return direction;
}

void PipelineGraphModel::setBusDirection(QtNodes::NodeId module,
                                         QtNodes::NodeId bus,
                                         BusDirection direction)
{
    if (!isBus(bus) || !canAttachToBus(module)) {
        return;
    }

    // A module on a bus does at least one of the two; neither is detaching, and
    // that has a command of its own.
    if (!direction.listens && !direction.talks) {
        direction.listens = true;
    }

    const std::size_t input = firstFreeFramesInput(module);
    const std::size_t output = firstFramesPort(module, false);
    const BusDirection now = busDirection(module, bus);

    // Each half is changed on its own: the pairing that completes a drop would
    // put back the half that is being taken away.
    m_pairing = false;

    const auto change = [this, module, bus](
                            bool want, bool have, bool listening, std::size_t port) {
        if (port == kNoPort || want == have) {
            return;
        }

        if (want) {
            if (listening) {
                addEdge(
                    QtNodes::ConnectionId{bus, 0, module, static_cast<QtNodes::PortIndex>(port)});
            } else {
                addEdge(
                    QtNodes::ConnectionId{module, static_cast<QtNodes::PortIndex>(port), bus, 0});
            }
            return;
        }

        for (const QtNodes::ConnectionId& tap : tapsBetween(module, bus)) {
            if ((tap.outNodeId == bus) == listening) {
                removeEdge(tap);
            }
        }
    };

    change(direction.listens, now.listens, true, input);
    change(direction.talks, now.talks, false, output);

    m_pairing = true;

    fitBus(bus);
}

CanFormatMatch PipelineGraphModel::frameFormat(QtNodes::ConnectionId connectionId) const
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    for (const EdgeDescription& edge : m_description.edges()) {
        if (edge.fromNode == from && edge.fromPort == connectionId.outPortIndex && edge.toNode == to
            && edge.toPort == connectionId.inPortIndex) {
            return edge.frameFormat;
        }
    }

    return CanFormatMatch::Any;
}

void PipelineGraphModel::setFrameFormat(QtNodes::ConnectionId connectionId, CanFormatMatch format)
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    for (EdgeDescription& edge : m_description.edges()) {
        if (edge.fromNode == from && edge.fromPort == connectionId.outPortIndex && edge.toNode == to
            && edge.toPort == connectionId.inPortIndex) {
            if (edge.frameFormat == format) {
                return;
            }

            edge.frameFormat = format;

            // The scene repaints a wire when the block at either end is told it
            // changed, and has no other way of being asked.
            Q_EMIT nodeUpdated(connectionId.outNodeId);
            Q_EMIT nodeUpdated(connectionId.inNodeId);
            return;
        }
    }
}

double PipelineGraphModel::busLeftBound(const NodeDescription& bus) const
{
    double bound = std::numeric_limits<double>::infinity();

    for (const QtNodes::NodeId moduleId : modulesOn(canvasId(bus.id))) {
        if (const NodeDescription* module = description(moduleId)) {
            bound = std::min(bound, module->x - layout::kRailPlateWidth - layout::kRailInset);
        }
    }

    return bound;
}

double PipelineGraphModel::busWidth(const NodeDescription& bus) const
{
    double right = bus.x + layout::kRailMinWidth;

    for (const QtNodes::NodeId moduleId : modulesOn(canvasId(bus.id))) {
        if (const NodeDescription* module = description(moduleId)) {
            right = std::max(right, module->x + blockWidth(*module) + layout::kRailInset);
        }
    }

    return right - bus.x;
}

void PipelineGraphModel::fitBus(QtNodes::NodeId busId)
{
    NodeDescription* bus = description(busId);
    if (!isBusBlock(bus)) {
        return;
    }

    const double bound = busLeftBound(*bus);
    if (bus->x > bound) {
        bus->x = bound;
        Q_EMIT nodePositionUpdated(busId);
    }

    Q_EMIT nodeUpdated(busId);
}

void PipelineGraphModel::fitBusesOf(QtNodes::NodeId nodeId)
{
    for (const QtNodes::NodeId bus : busesOf(nodeId)) {
        fitBus(bus);
    }
}

void PipelineGraphModel::endMoves()
{
    if (--m_moveDepth > 0) {
        return;
    }

    m_moveDepth = 0;

    std::unordered_set<QtNodes::NodeId> moved;
    moved.swap(m_moved);

    // The blocks first, then the buses: a bus is as wide as the distance to the furthest
    // block it carries, and that distance is only the one that will be drawn once every
    // block has been told where it is.
    for (const QtNodes::NodeId nodeId : moved) {
        if (nodeExists(nodeId)) {
            Q_EMIT nodePositionUpdated(nodeId);
        }
    }

    // Every bus, whether or not one of its modules was among those moved: this is once per
    // bus, there are a handful of them, and asking which ones were touched costs a pass
    // over every wire for each.
    for (const NodeDescription& block : m_description.nodes()) {
        if (isBusBlock(&block)) {
            fitBus(canvasId(block.id));
        }
    }
}

void PipelineGraphModel::fitAllBuses()
{
    // Without a word to the scene: this runs while the model is being rebuilt,
    // and the scene is about to be told everything.
    for (NodeDescription& block : m_description.nodes()) {
        if (isBusBlock(&block)) {
            block.x = std::min(block.x, busLeftBound(block));
        }
    }
}

} // namespace torquebus::ui
