// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The canvas, as QtNodes sees it.
//
// This is an adapter and nothing else: every question QtNodes asks is answered
// out of a GraphDescription, and every edit it makes is written straight back
// into that same GraphDescription. There is no second copy of the graph.
//
// That is the whole design decision, and it is why this derives from
// AbstractGraphModel rather than using QtNodes' own DataFlowGraphModel.
// DataFlowGraphModel keeps the graph in its own structures, which would mean
// holding the user's pipeline twice and syncing the two - and two copies of a
// thing that can be edited from both ends diverge. They always do. The graph is
// the data path (rule #11); the canvas is a view onto it.
//
// Reading direction:
//
//     GraphDescription   what the user made, and what the project file stores
//            ^
//            | reads and writes, no copy
//            |
//     PipelineGraphModel     <- this file, the QtNodes seam
//            ^
//            |
//     BasicGraphicsScene / GraphicsView       QtNodes' own widgets

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <QtNodes/AbstractGraphModel>
#include <QtNodes/Definitions>

#include <QString>

#include <string>
#include <unordered_map>

namespace torquebus::ui {

class PipelineGraphModel final : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    /// Neither argument is owned. The description outlives the canvas - it is
    /// the project - and the catalog outlives everything.
    PipelineGraphModel(GraphDescription& description,
                       const NodeCatalog& catalog,
                       QObject* parent = nullptr);

    // --- Identity ---------------------------------------------------------

    [[nodiscard]] QtNodes::NodeId newNodeId() override;
    [[nodiscard]] std::unordered_set<QtNodes::NodeId> allNodeIds() const override;
    [[nodiscard]] bool nodeExists(QtNodes::NodeId nodeId) const override;

    // --- Connections ------------------------------------------------------

    [[nodiscard]] std::unordered_set<QtNodes::ConnectionId>
    allConnectionIds(QtNodes::NodeId nodeId) const override;

    [[nodiscard]] std::unordered_set<QtNodes::ConnectionId>
    connections(QtNodes::NodeId nodeId,
                QtNodes::PortType portType,
                QtNodes::PortIndex index) const override;

    [[nodiscard]] bool connectionExists(QtNodes::ConnectionId connectionId) const override;

    /// Refuses a wire the pipeline could not build, while it is being dragged.
    ///
    /// Same rules as GraphDescription::validate - matching port types, and one
    /// edge per input - so the canvas cannot draw a graph that then fails at
    /// Start. Saying no during the drag is the only moment the user has any
    /// context for why.
    [[nodiscard]] bool connectionPossible(QtNodes::ConnectionId connectionId) const override;

    void addConnection(QtNodes::ConnectionId connectionId) override;
    bool deleteConnection(QtNodes::ConnectionId connectionId) override;

    // --- Nodes ------------------------------------------------------------

    /// `nodeType` is a catalog type name: "can.source", "lua.ecu".
    QtNodes::NodeId addNode(QString nodeType = QString{}) override;
    bool deleteNode(QtNodes::NodeId nodeId) override;

    [[nodiscard]] QVariant nodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role) const override;

    bool setNodeData(QtNodes::NodeId nodeId, QtNodes::NodeRole role, QVariant value) override;

    [[nodiscard]] QVariant portData(QtNodes::NodeId nodeId,
                                    QtNodes::PortType portType,
                                    QtNodes::PortIndex index,
                                    QtNodes::PortRole role) const override;

    bool setPortData(QtNodes::NodeId nodeId,
                     QtNodes::PortType portType,
                     QtNodes::PortIndex index,
                     const QVariant& value,
                     QtNodes::PortRole role = QtNodes::PortRole::Data) override;

    // --- Our side ---------------------------------------------------------

    /// The description's id for a canvas node, or empty when unknown.
    [[nodiscard]] std::string descriptionId(QtNodes::NodeId nodeId) const;

    /// The canvas id for a description node, or InvalidNodeId.
    [[nodiscard]] QtNodes::NodeId canvasId(const std::string& descriptionId) const;

    /// Rebuilds the id mapping from the description and tells the scene.
    ///
    /// For when the description changed underneath us - a project was opened,
    /// or a graph was replaced wholesale. Incremental edits do not need it;
    /// they go through the methods above and emit their own signals.
    void reload();

Q_SIGNALS:
    /// A node's settings should be shown. The properties panel listens.
    void nodeSelected(const QString& descriptionId);

private:
    [[nodiscard]] const NodeDescription* description(QtNodes::NodeId nodeId) const;
    [[nodiscard]] NodeDescription* description(QtNodes::NodeId nodeId);
    [[nodiscard]] const NodeTypeInfo* typeInfo(QtNodes::NodeId nodeId) const;

    /// Assigns the next free canvas id to a description node.
    QtNodes::NodeId adopt(const std::string& id);

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;

    // Two maps rather than one plus a linear search: the scene asks for these
    // on every paint, for every node and every port.
    std::unordered_map<QtNodes::NodeId, std::string> m_toDescription;
    std::unordered_map<std::string, QtNodes::NodeId> m_toCanvas;

    QtNodes::NodeId m_nextNodeId{0};
};

} // namespace torquebus::ui
