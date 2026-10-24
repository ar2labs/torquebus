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
//
// What the model adds to "a graph" is what the canvas draws that a graph does
// not say: how big a block is, and the bus.
//
// A CAN Bus is a block in the graph and a line on the canvas. Every module wired
// to it is one drop off that line, and to the person drawing, one drop is one
// thing even though the graph holds it as two wires - what the module hears and
// what it says. The model keeps the two together: wiring one half of a drop
// wires the other when the module is the kind that speaks on a bus, and deleting
// one half deletes both. Nothing here changes what the graph means; it changes
// how many things a person has to do to say it.

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <QtNodes/AbstractGraphModel>
#include <QtNodes/Definitions>

#include <QSize>
#include <QString>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    /// edge per input except at a fan-in port - so the canvas cannot draw a
    /// graph that then fails at Start. Saying no during the drag is the only
    /// moment the user has any context for why. A bus to a bus is refused too:
    /// the graph could build it, but nothing on the canvas could draw it.
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

    [[nodiscard]] QJsonObject saveNode(QtNodes::NodeId nodeId) const override;
    void loadNode(const QJsonObject& nodeJson) override;

    // --- Our side ---------------------------------------------------------

    /// The description's id for a canvas node, or empty when unknown.
    [[nodiscard]] std::string descriptionId(QtNodes::NodeId nodeId) const;

    /// The canvas id for a description node, or InvalidNodeId.
    [[nodiscard]] QtNodes::NodeId canvasId(const std::string& descriptionId) const;

    /// What a canvas node is, for a painter that has only the id QtNodes gave
    /// it. Nullptr when the node is not there.
    [[nodiscard]] const NodeDescription* node(QtNodes::NodeId nodeId) const;
    [[nodiscard]] const NodeTypeInfo* info(QtNodes::NodeId nodeId) const;

    /// Rebuilds the id mapping from the description and tells the scene.
    ///
    /// For when the description changed underneath us - a project was opened,
    /// or a graph was replaced wholesale. Incremental edits do not need it;
    /// they go through the methods above and emit their own signals.
    void reload();

    // --- Size -------------------------------------------------------------

    /// A block is a chip unless it has been opened into a card, and a bus is as
    /// long as the modules on it are spread out.
    [[nodiscard]] QSize blockSize(QtNodes::NodeId nodeId) const;

    [[nodiscard]] bool isExpanded(QtNodes::NodeId nodeId) const;

    /// Opens a block into a card, or closes it. Not part of the project: what a
    /// block shows is a way of looking, and reopening a project with every card
    /// open would be a way of looking nobody chose.
    void setExpanded(QtNodes::NodeId nodeId, bool expanded);
    void setAllExpanded(bool expanded);

    [[nodiscard]] bool anyExpanded() const noexcept { return !m_expanded.empty(); }

    // --- The bus ----------------------------------------------------------

    [[nodiscard]] bool isBus(QtNodes::NodeId nodeId) const;

    /// A wire between a bus and a module, in either direction.
    [[nodiscard]] bool isBusTap(QtNodes::ConnectionId connectionId) const;

    /// The bus ends of the wires that leave and arrive at a module.
    [[nodiscard]] std::vector<QtNodes::NodeId> busesOf(QtNodes::NodeId module) const;

    /// Every module wired to the bus, once each, whichever way the wire runs.
    [[nodiscard]] std::vector<QtNodes::NodeId> modulesOn(QtNodes::NodeId bus) const;

    /// The wires between a module and one bus.
    [[nodiscard]] std::vector<QtNodes::ConnectionId> tapsBetween(QtNodes::NodeId module,
                                                                 QtNodes::NodeId bus) const;

    /// Whether the module has ports a bus could be wired to at all.
    [[nodiscard]] bool canAttachToBus(QtNodes::NodeId module) const;

    /// Wires a module onto a bus the way dropping it there would: it listens, and
    /// speaks too when it is the kind that does. False when nothing could be wired
    /// - the module has no frames port, or the one it has is taken.
    bool attachToBus(QtNodes::NodeId module, QtNodes::NodeId bus);

    /// Takes every wire between a module and a bus away.
    void detachFromBus(QtNodes::NodeId module, QtNodes::NodeId bus);

    /// What a module does on a bus: whether the bus's frames reach it, and
    /// whether what it sends reaches the bus. One of the two is always kept.
    struct BusDirection final {
        bool listens{false};
        bool talks{false};
    };

    [[nodiscard]] BusDirection busDirection(QtNodes::NodeId module, QtNodes::NodeId bus) const;
    void setBusDirection(QtNodes::NodeId module, QtNodes::NodeId bus, BusDirection direction);

    /// Which of the frames on a wire get through. Meaningful on a wire from a
    /// bus, though it is a property of any wire that carries frames.
    [[nodiscard]] CanFormatMatch frameFormat(QtNodes::ConnectionId connectionId) const;
    void setFrameFormat(QtNodes::ConnectionId connectionId, CanFormatMatch format);

    /// Moves a bus so that it covers the modules wired to it: a rail that does
    /// not reach the module it carries has a drop that does not touch anything.
    void fitBus(QtNodes::NodeId bus);

    /// The channel a bus is: what its "CAN 2" refers to.
    [[nodiscard]] std::int64_t busChannel(QtNodes::NodeId bus) const;

    /// Moving a block tells the scene, and the scene lays out the bus it hangs from
    /// and every wire to it. Done once for each of eighty blocks in turn that is
    /// eighty times the work of moving them all, because every one of those eighty
    /// lays the bus out again with its own wires - and laying a bus out reads every
    /// wire it has. Arranging eighty modules took five seconds, and doubling them
    /// made it eight times worse.
    ///
    /// Between beginMoves() and endMoves() a block's position is written and
    /// nothing is said; endMoves() fits the buses once and tells the scene once.
    /// They nest, and MoveBatch is the way to use them.
    void beginMoves() noexcept { ++m_moveDepth; }
    void endMoves();

    class MoveBatch final {
    public:
        explicit MoveBatch(PipelineGraphModel& model)
            : m_model{model}
        {
            m_model.beginMoves();
        }

        ~MoveBatch() { m_model.endMoves(); }

        MoveBatch(const MoveBatch&) = delete;
        MoveBatch& operator=(const MoveBatch&) = delete;
        MoveBatch(MoveBatch&&) = delete;
        MoveBatch& operator=(MoveBatch&&) = delete;

    private:
        PipelineGraphModel& m_model;
    };

Q_SIGNALS:
    /// A node's settings should be shown. The properties panel listens.
    void nodeSelected(const QString& descriptionId);

private:
    [[nodiscard]] const NodeDescription* description(QtNodes::NodeId nodeId) const;
    [[nodiscard]] NodeDescription* description(QtNodes::NodeId nodeId);
    [[nodiscard]] const NodeTypeInfo* typeInfo(QtNodes::NodeId nodeId) const;

    /// Assigns the next free canvas id to a description node.
    QtNodes::NodeId adopt(const std::string& id);

    /// The one place an edge is added, so that the signal and the check cannot
    /// be forgotten by one of the callers.
    bool addEdge(QtNodes::ConnectionId connectionId);
    bool removeEdge(QtNodes::ConnectionId connectionId);

    /// The wire that completes a drop: the other half of what a module does on
    /// a bus, if it is the kind that does both and the ports are free.
    [[nodiscard]] bool completionOf(QtNodes::ConnectionId connectionId,
                                    QtNodes::ConnectionId& completion) const;

    /// The first frames input and output a module has, or npos.
    [[nodiscard]] std::size_t firstFramesPort(QtNodes::NodeId nodeId, bool input) const;

    /// The first frames input of a module that can take a wire: one with none on it, or one that
    /// takes any number. The first frames input when every one is taken, so that a drop on a module
    /// with a single input is tried - and refused - as it always was; npos when it has none. A
    /// Frame Merge has four, and each bus dropped on it is wired to the next.
    [[nodiscard]] std::size_t firstFreeFramesInput(QtNodes::NodeId nodeId) const;

    [[nodiscard]] double blockWidth(const NodeDescription& block) const;

    /// The furthest right a rail may start so that it still covers its modules.
    [[nodiscard]] double busLeftBound(const NodeDescription& bus) const;
    [[nodiscard]] double busWidth(const NodeDescription& bus) const;

    /// Fits every bus the node is wired to, after the node moved or changed size.
    void fitBusesOf(QtNodes::NodeId nodeId);
    void fitAllBuses();

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;

    // Two maps rather than one plus a linear search: the scene asks for these
    // on every paint, for every node and every port.
    std::unordered_map<QtNodes::NodeId, std::string> m_toDescription;
    std::unordered_map<std::string, QtNodes::NodeId> m_toCanvas;

    /// Description ids of the blocks shown as cards.
    std::unordered_set<std::string> m_expanded;

    /// False while a change is being made to one half of a drop on purpose.
    bool m_pairing{true};

    /// How many MoveBatch objects are alive, and the blocks moved under them.
    int m_moveDepth{0};
    std::unordered_set<QtNodes::NodeId> m_moved;

    QtNodes::NodeId m_nextNodeId{0};
};

} // namespace torquebus::ui
