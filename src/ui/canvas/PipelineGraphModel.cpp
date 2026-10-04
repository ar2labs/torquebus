// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/PipelineGraphModel.h"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/NodeData>
#include <QtNodes/StyleCollection>

#include <QJsonObject>
#include <QPointF>
#include <QSize>

#include <algorithm>
#include <utility>
#include <vector>

namespace torquebus::ui {
namespace {

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

    // One edge per input. Enforced here as well as in GraphDescription because
    // this is the copy the user feels: the wire simply refuses to land.
    return connections(connectionId.inNodeId, QtNodes::PortType::In, connectionId.inPortIndex)
        .empty();
}

void PipelineGraphModel::addConnection(QtNodes::ConnectionId connectionId)
{
    const std::string from = descriptionId(connectionId.outNodeId);
    const std::string to = descriptionId(connectionId.inNodeId);

    if (from.empty() || to.empty()) {
        return;
    }

    if (connectionExists(connectionId)) {
        return;
    }

    if (!connectionPossible(connectionId)) {
        return;
    }

    m_description.addEdge(
        EdgeDescription{from, connectionId.outPortIndex, to, connectionId.inPortIndex});

    Q_EMIT connectionCreated(connectionId);
}

bool PipelineGraphModel::deleteConnection(QtNodes::ConnectionId connectionId)
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

    Q_EMIT nodeDeleted(nodeId);
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
        return QSize{310, 196};

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
        const QPointF position = value.value<QPointF>();
        node->x = position.x();
        node->y = position.y();

        Q_EMIT nodePositionUpdated(nodeId);
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
        // merging two streams needs a policy.
        return QVariant::fromValue(portType == QtNodes::PortType::Out
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

} // namespace torquebus::ui
