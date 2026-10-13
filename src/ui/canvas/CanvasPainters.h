// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What QtNodes asks of a canvas that is drawn rather than defaulted: where a
// block's ports are, how a block is painted and how a wire is.
//
// QtNodes' own are made for blocks with a title bar and a port label on each
// side, joined by curves. This canvas has chips with a port on the top and the
// bottom, a bus that is a line and not a block, and wires that are a drop down
// to that line - so all three are replaced, and everything the replacements need
// to know about the application (the theme, what a block is doing right now) is
// read from the CanvasPanel they are friends of.
//
// The painters draw into the item they are given. QtNodes caches each block as a
// picture and draws it again only when something changes, so what a painter does
// is paid for when a block changes and not on every frame - which is why nothing
// animated is drawn here. The glow of a running block and the packets that run
// along the wires are the canvas foreground's.

#pragma once

#include <QtNodes/Definitions>
#include <QtNodes/internal/AbstractConnectionPainter.hpp>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/AbstractNodePainter.hpp>

#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#include <array>
#include <cstddef>

class QGraphicsScene;

namespace QtNodes {
class ConnectionGraphicsObject;
class NodeGraphicsObject;
} // namespace QtNodes

namespace torquebus {
struct NodeDescription;
}

namespace torquebus::ui {

class CanvasPanel;
class PipelineGraphModel;

/// Sizes, and where the ports are.
class PipelineNodeGeometry final : public QtNodes::AbstractNodeGeometry {
public:
    explicit PipelineNodeGeometry(PipelineGraphModel& model);

    /// The scene the geometry is for, so it can tell whether a wire is being
    /// dragged. Not owned.
    void setScene(const QGraphicsScene* scene) noexcept { m_scene = scene; }

    [[nodiscard]] QRectF boundingRect(QtNodes::NodeId nodeId) const override;
    [[nodiscard]] QSize size(QtNodes::NodeId nodeId) const override;

    /// Sizes come from the model, which knows what a block is showing and where
    /// the modules of a bus are. There is nothing to measure.
    void recomputeSize(QtNodes::NodeId) const override { }

    [[nodiscard]] QPointF portPosition(QtNodes::NodeId nodeId,
                                       QtNodes::PortType portType,
                                       QtNodes::PortIndex index) const override;

    /// A bus is one port wide in every way that matters: a wire dropped anywhere
    /// on its rail lands on it. But only while a wire is being dragged - pressing
    /// on the rail has to pick the bus up, not start a wire from it.
    [[nodiscard]] QtNodes::PortIndex checkPortHit(QtNodes::NodeId nodeId,
                                                  QtNodes::PortType portType,
                                                  QPointF nodePoint) const override;

    // Blocks have no captions, port labels or embedded widgets: the painter
    // writes the one line a block has.
    [[nodiscard]] QPointF
    portTextPosition(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex) const override
    {
        return {};
    }
    [[nodiscard]] QPointF captionPosition(QtNodes::NodeId) const override { return {}; }
    [[nodiscard]] QRectF captionRect(QtNodes::NodeId) const override { return {}; }
    [[nodiscard]] QPointF widgetPosition(QtNodes::NodeId) const override { return {}; }
    [[nodiscard]] QRect resizeHandleRect(QtNodes::NodeId) const override { return {}; }

private:
    [[nodiscard]] bool wireIsBeingDragged() const;

    PipelineGraphModel& m_model;
    const QGraphicsScene* m_scene{nullptr};
};

/// Chips, cards and the rail of the bus.
class PipelineNodePainter final : public QtNodes::AbstractNodePainter {
public:
    explicit PipelineNodePainter(const CanvasPanel& panel);

    void paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const override;

private:
    void paintChip(QPainter* painter,
                   QtNodes::NodeGraphicsObject& ngo,
                   const NodeDescription& node) const;
    void paintCard(QPainter* painter,
                   QtNodes::NodeGraphicsObject& ngo,
                   const NodeDescription& node) const;
    void paintRail(QPainter* painter,
                   QtNodes::NodeGraphicsObject& ngo,
                   const NodeDescription& node) const;

    /// The ports of a block, and the points where its wires to a bus end.
    void paintPorts(QPainter* painter, QtNodes::NodeGraphicsObject& ngo, const QRectF& rect) const;

    const CanvasPanel& m_panel;
};

/// Wires: a drop - the two wires of a CAN bus, carried down to a module - for what is wired to a
/// bus, and a curve for anything else.
class PipelineConnectionPainter final : public QtNodes::AbstractConnectionPainter {
public:
    explicit PipelineConnectionPainter(const CanvasPanel& panel);

    void paint(QPainter* painter, const QtNodes::ConnectionGraphicsObject& cgo) const override;
    [[nodiscard]] QPainterPath
    getPainterStroke(const QtNodes::ConnectionGraphicsObject& cgo) const override;

private:
    const CanvasPanel& m_panel;
};

/// Which of a drop's two wires is which. CAN_H is the amber one, on the left; CAN_L is the green
/// one, on the right: the colours and the order the bus's own two wires are drawn in.
inline constexpr std::size_t kCanHigh = 0;
inline constexpr std::size_t kCanLow = 1;

/// One of the two wires of a drop, from the module to the wire of the bus it is joined to.
struct DropWire final {
    QPainterPath path;
    QPointF moduleEnd;
    QPointF railEnd; ///< Where it joins its wire of the bus, and the dot is.
};

/// A wire as it is drawn, in the coordinates of the connection item.
struct WireShape final {
    /// A curve between two blocks; for a drop, the middle of the pair, from the module to the
    /// middle of the bus - the line the arrows and the pill are on.
    QPainterPath path;

    /// Wired to a bus: a pair of wires, one to each wire of the bus. `wires[kCanHigh]` goes to the
    /// upper wire of the rail and `wires[kCanLow]` to the lower, so the one that has to go further
    /// passes the other wire on the way - as it does on a schematic, with a dot only where it
    /// joins.
    bool isTap{false};
    bool towardBus{false}; ///< What the wire carries runs from the module to the bus.
    bool moduleBelow{false}; ///< The module hangs under the rail, and not over it.
    QPointF moduleEnd; ///< Where the middle of the pair leaves the module.
    QPointF railEnd; ///< Where the middle of the pair arrives at the bus.
    std::array<DropWire, 2> wires;
};

/// The shape of a wire that is complete. Empty path for a wire that cannot be
/// drawn, which a wire to a block that was just deleted briefly is.
[[nodiscard]] WireShape wireShape(const QtNodes::ConnectionGraphicsObject& cgo,
                                  const PipelineGraphModel& model);

/// Where the pill that says which frames a wire carries is, in scene
/// coordinates; null for a wire that has none to show.
[[nodiscard]] QRectF frameFormatPillRect(const QtNodes::ConnectionGraphicsObject& cgo,
                                         const PipelineGraphModel& model);

/// Which side of the module the wire to a bus leaves from.
[[nodiscard]] bool moduleHangsBelow(const QRectF& moduleRect, const QRectF& railRect);

} // namespace torquebus::ui
