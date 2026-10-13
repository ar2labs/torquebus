// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/CanvasPainters.h"

#include "core/pipeline/GraphDescription.h"
#include "ui/canvas/CanvasLayout.h"
#include "ui/canvas/CanvasPanel.h"
#include "ui/canvas/PipelineGraphModel.h"
#include "ui/common/NodeAppearance.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QtNodes/internal/ConnectionState.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/NodeState.hpp>

#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <vector>

namespace torquebus::ui {
namespace {

[[nodiscard]] QColor withAlpha(QColor colour, int alpha)
{
    colour.setAlpha(alpha);
    return colour;
}

/// `from` toward `to`, `amount` of the way (0 to 1), in RGB. Close enough for
/// tinting a border with an accent, which is all it is for.
[[nodiscard]] QColor mixed(const QColor& from, const QColor& to, qreal amount)
{
    const auto channel = [amount](int a, int b) {
        return static_cast<int>(std::lround(a + (b - a) * amount));
    };
    return QColor{channel(from.red(), to.red()),
                  channel(from.green(), to.green()),
                  channel(from.blue(), to.blue()),
                  channel(from.alpha(), to.alpha())};
}

/// The colour a wire or a port of this type is drawn in.
[[nodiscard]] QColor typeColour(PortType type, const Theme& theme)
{
    switch (type) {
    case PortType::Signals:
        return theme.success;
    case PortType::Events:
        return theme.warning;
    case PortType::Pgns:
        return theme.tx;
    case PortType::Frames:
        break;
    }
    return theme.accent;
}

/// What the arrows of a drop are drawn in when nothing is happening to it: quiet, so that a bus
/// with twenty modules on it is a calm picture and the colour is left for the two wires of the
/// drop, which are the two wires of the bus.
[[nodiscard]] QColor dropColour(const Theme& theme)
{
    return mixed(theme.textMuted, theme.text, 0.2);
}

[[nodiscard]] const PipelineGraphModel* modelOf(const QtNodes::ConnectionGraphicsObject& cgo)
{
    return qobject_cast<const PipelineGraphModel*>(&cgo.graphModel());
}

[[nodiscard]] QRectF sceneRectOf(QtNodes::BasicGraphicsScene& scene, QtNodes::NodeId nodeId)
{
    QtNodes::NodeGraphicsObject* object = scene.nodeGraphicsObject(nodeId);
    if (object == nullptr) {
        return {};
    }

    return object->mapRectToScene(
        QRectF{QPointF{0.0, 0.0}, QSizeF{scene.nodeGeometry().size(nodeId)}});
}

/// The path that goes through the corners, in order.
[[nodiscard]] QPainterPath pathThrough(const std::vector<QPointF>& corners)
{
    QPainterPath path;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        if (i == 0) {
            path.moveTo(corners[i]);
        } else {
            path.lineTo(corners[i]);
        }
    }
    return path;
}

/// A point on a path made of straight lines, and which way the path is going there.
struct PolylinePoint final {
    QPointF at;
    QPointF direction; ///< A unit vector, along the path from its start.
    bool found{false};
};

/// The point `distance` along a path that is only straight lines - a drop is - and which way it
/// runs there. Walked segment by segment, so that a point on a corner has the direction of one
/// of the two lines and not of the diagonal between them, which is what a percentage of the path
/// read at two nearby points gives.
[[nodiscard]] PolylinePoint alongPolyline(const QPainterPath& path, qreal distance)
{
    PolylinePoint result;

    QPointF previous;
    qreal walked = 0.0;

    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        const QPointF point{element.x, element.y};

        if (i > 0) {
            const QLineF segment{previous, point};
            const qreal length = segment.length();

            if (length > 0.0 && walked + length >= distance) {
                result.direction = QPointF{segment.dx(), segment.dy()} / length;
                result.at = previous + result.direction * (distance - walked);
                result.found = true;
                return result;
            }
            walked += length;
        }
        previous = point;
    }

    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

PipelineNodeGeometry::PipelineNodeGeometry(PipelineGraphModel& model)
    : QtNodes::AbstractNodeGeometry{model}
    , m_model{model}
{ }

QSize PipelineNodeGeometry::size(QtNodes::NodeId nodeId) const
{
    return m_model.blockSize(nodeId);
}

QRectF PipelineNodeGeometry::boundingRect(QtNodes::NodeId nodeId) const
{
    // Room round the block for what is painted outside it: the glow of a
    // selection, a port that sits on the edge, the dot where a drop meets it.
    constexpr qreal kMargin = 14.0;

    const QSize block = size(nodeId);
    return QRectF{
        -kMargin, -kMargin, block.width() + 2.0 * kMargin, block.height() + 2.0 * kMargin};
}

QPointF PipelineNodeGeometry::portPosition(QtNodes::NodeId nodeId,
                                           QtNodes::PortType portType,
                                           QtNodes::PortIndex index) const
{
    const QSize block = size(nodeId);

    // Both of a bus's ports are its middle. Where a wire meets the rail is the
    // module's business, so that is where the wire is drawn to; this is only
    // where QtNodes thinks it ends, and what it measures its drawing area by.
    if (m_model.isBus(nodeId)) {
        return QPointF{block.width() * 0.5, block.height() * 0.5};
    }

    const NodeTypeInfo* info = m_model.info(nodeId);
    if (info == nullptr || portType == QtNodes::PortType::None) {
        return {};
    }

    const bool input = portType == QtNodes::PortType::In;
    const std::size_t count = input ? info->inputs.size() : info->outputs.size();

    return layout::portPoint(input, index, count, block.width(), block.height());
}

bool PipelineNodeGeometry::wireIsBeingDragged() const
{
    if (m_scene == nullptr) {
        return false;
    }

    // The draft QtNodes draws while a wire is dragged is an item of the scene until the drag is
    // settled, which is *after* it has asked where the wire landed - so it is there to be found
    // when the question is asked on release, and absent when it is asked on a press. The mouse
    // grabber is not a way to tell: the draft lets go of the mouse before it asks.
    return std::ranges::any_of(m_scene->items(), [](const QGraphicsItem* item) {
        const auto* wire = qgraphicsitem_cast<const QtNodes::ConnectionGraphicsObject*>(item);
        return wire != nullptr && wire->connectionState().requiresPort();
    });
}

QtNodes::PortIndex PipelineNodeGeometry::checkPortHit(QtNodes::NodeId nodeId,
                                                      QtNodes::PortType portType,
                                                      QPointF nodePoint) const
{
    if (portType == QtNodes::PortType::None) {
        return QtNodes::InvalidPortIndex;
    }

    if (!m_model.isBus(nodeId)) {
        return QtNodes::AbstractNodeGeometry::checkPortHit(nodeId, portType, nodePoint);
    }

    // Only while a wire is being dragged does the whole rail count as the port: a
    // press has to move the bus, not draw from it.
    if (!wireIsBeingDragged()) {
        return QtNodes::InvalidPortIndex;
    }

    const QSize block = size(nodeId);
    constexpr qreal kSlack = 10.0;

    const QRectF rail{
        -kSlack, -kSlack, block.width() + 2.0 * kSlack, block.height() + 2.0 * kSlack};

    return rail.contains(nodePoint) ? 0U : QtNodes::InvalidPortIndex;
}

// ---------------------------------------------------------------------------
// Wires, as shapes
// ---------------------------------------------------------------------------

bool moduleHangsBelow(const QRectF& moduleRect, const QRectF& railRect)
{
    return moduleRect.center().y() > railRect.center().y();
}

WireShape wireShape(const QtNodes::ConnectionGraphicsObject& cgo, const PipelineGraphModel& model)
{
    WireShape shape;

    QtNodes::BasicGraphicsScene* scene = cgo.nodeScene();
    if (scene == nullptr) {
        return shape;
    }

    const QtNodes::ConnectionId id = cgo.connectionId();

    if (model.isBusTap(id)) {
        const bool towardBus = model.isBus(id.inNodeId);
        const QtNodes::NodeId moduleId = towardBus ? id.outNodeId : id.inNodeId;
        const QtNodes::NodeId busId = towardBus ? id.inNodeId : id.outNodeId;

        const QRectF module = sceneRectOf(*scene, moduleId);
        const QRectF rail = sceneRectOf(*scene, busId);

        if (!module.isNull() && !rail.isNull()) {
            const bool below = moduleHangsBelow(module, rail);

            const qreal x = module.center().x();
            const qreal landing = layout::tapX(x, rail.left(), rail.width());
            const qreal edge = below ? module.top() : module.bottom();
            const qreal busMiddle = rail.center().y();

            // The middle of the pair, which is where the arrows and the pill are put.
            const std::vector<QPointF> centre = layout::dropCentre(x, edge, landing, busMiddle);

            shape.path = cgo.mapFromScene(pathThrough(centre));
            shape.isTap = true;
            shape.towardBus = towardBus;
            shape.moduleBelow = below;
            shape.moduleEnd = cgo.mapFromScene(centre.front());
            shape.railEnd = cgo.mapFromScene(centre.back());

            // And the two wires, each joined to its own wire of the bus.
            for (const std::size_t which : {kCanHigh, kCanLow}) {
                const std::vector<QPointF> corners =
                    layout::dropWire(which == kCanHigh, x, edge, landing, busMiddle);

                DropWire& wire = shape.wires.at(which);
                wire.path = cgo.mapFromScene(pathThrough(corners));
                wire.moduleEnd = cgo.mapFromScene(corners.front());
                wire.railEnd = cgo.mapFromScene(corners.back());
            }
            return shape;
        }
    }

    QPainterPath path{cgo.endPoint(QtNodes::PortType::Out)};
    const auto [c1, c2] = cgo.pointsC1C2();
    path.cubicTo(c1, c2, cgo.endPoint(QtNodes::PortType::In));
    shape.path = path;
    return shape;
}

QRectF frameFormatPillRect(const QtNodes::ConnectionGraphicsObject& cgo,
                           const PipelineGraphModel& model)
{
    const WireShape shape = wireShape(cgo, model);

    // Only on what the bus delivers to a module: what a module sends has no
    // format to limit.
    if (!shape.isTap || shape.towardBus || shape.path.isEmpty()) {
        return {};
    }

    constexpr qreal kWidth = 44.0;
    constexpr qreal kHeight = 15.0;
    constexpr qreal kFromModule = 24.0;

    const qreal toward = shape.moduleBelow ? -1.0 : 1.0; // From the module to the rail.
    const QPointF centre = shape.moduleEnd + QPointF{0.0, toward * kFromModule};

    return cgo
        .mapToScene(QRectF{centre.x() - kWidth * 0.5, centre.y() - kHeight * 0.5, kWidth, kHeight})
        .boundingRect();
}

// ---------------------------------------------------------------------------
// The connection painter
// ---------------------------------------------------------------------------

PipelineConnectionPainter::PipelineConnectionPainter(const CanvasPanel& panel)
    : m_panel{panel}
{ }

QPainterPath
PipelineConnectionPainter::getPainterStroke(const QtNodes::ConnectionGraphicsObject& cgo) const
{
    const PipelineGraphModel* model = modelOf(cgo);
    if (model == nullptr || cgo.connectionState().requiresPort()) {
        QPainterPath path{cgo.endPoint(QtNodes::PortType::Out)};
        const auto [c1, c2] = cgo.pointsC1C2();
        path.cubicTo(c1, c2, cgo.endPoint(QtNodes::PortType::In));

        QPainterPathStroker stroker;
        stroker.setWidth(10.0);
        return stroker.createStroke(path);
    }

    const WireShape shape = wireShape(cgo, *model);

    // A curve is approximated by lines before it is thickened, as QtNodes'
    // own does: the stroker of a long cubic is slow and this is asked on every
    // mouse move over the canvas.
    QPainterPath polyline;
    if (shape.isTap) {
        polyline = shape.path;
    } else if (shape.path.elementCount() > 0) {
        constexpr int kSegments = 20;
        polyline.moveTo(shape.path.pointAtPercent(0.0));
        for (int i = 1; i <= kSegments; ++i) {
            polyline.lineTo(shape.path.pointAtPercent(static_cast<qreal>(i) / kSegments));
        }
    }

    // A drop is picked up by either of its wires, and by the gap between them: the pair is one
    // thing, and the stroke round the middle of it takes in both.
    QPainterPathStroker stroker;
    stroker.setWidth(shape.isTap ? 2.0 * layout::kDropWireOffset + 9.0 : 10.0);
    QPainterPath stroke = stroker.createStroke(polyline);

    // The pill on what a bus delivers is wider than the line it sits on. Left out of the shape, the
    // pointer would leave the wire - and the pill would go - on its way to click it.
    if (shape.isTap && !shape.towardBus) {
        stroke.addRect(cgo.mapFromScene(frameFormatPillRect(cgo, *model)).boundingRect());
    }

    return stroke;
}

void PipelineConnectionPainter::paint(QPainter* painter,
                                      const QtNodes::ConnectionGraphicsObject& cgo) const
{
    const PipelineGraphModel* model = modelOf(cgo);
    if (model == nullptr) {
        return;
    }

    const Theme& theme = m_panel.m_theme;
    const QtNodes::ConnectionState& state = cgo.connectionState();

    painter->setRenderHint(QPainter::Antialiasing, true);

    // --- A wire that is being drawn --------------------------------------
    if (state.requiresPort()) {
        QPen pen{withAlpha(theme.accentHover, 220), 2.0, Qt::DashLine, Qt::RoundCap};
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        QPainterPath path{cgo.endPoint(QtNodes::PortType::Out)};
        const auto [c1, c2] = cgo.pointsC1C2();
        path.cubicTo(c1, c2, cgo.endPoint(QtNodes::PortType::In));
        painter->drawPath(path);

        painter->setPen(Qt::NoPen);
        painter->setBrush(theme.accentHover);
        painter->drawEllipse(cgo.endPoint(state.requiredPort()), 4.5, 4.5);
        return;
    }

    const WireShape shape = wireShape(cgo, *model);
    if (shape.path.isEmpty()) {
        return;
    }

    const bool selected = cgo.isSelected();
    const bool hovered = state.hovered();

    // --- A drop to the bus -----------------------------------------------
    //
    // Two wires, as a CAN bus is: CAN_H in amber and CAN_L in green, each joined to the wire of the
    // bus that is the same colour. A module is wired to a bus by dragging one wire to it, and what
    // is drawn is the pair that wire stands for.
    if (shape.isTap) {
        const bool lit = selected || hovered;
        const QColor quiet = lit ? theme.accentHover : dropColour(theme);
        const std::array<QColor, 2> conductor{theme.warning, theme.success};

        // Under the pointer, or selected: a light behind the pair, which is one thing.
        if (lit) {
            painter->setPen(QPen{withAlpha(theme.accent, 90),
                                 2.0 * layout::kDropWireOffset + 8.0,
                                 Qt::SolidLine,
                                 Qt::RoundCap,
                                 Qt::RoundJoin});
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(shape.path);
        }

        painter->setBrush(Qt::NoBrush);
        for (const std::size_t which : {kCanHigh, kCanLow}) {
            painter->setPen(QPen{
                conductor.at(which), lit ? 2.4 : 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
            painter->drawPath(shape.wires.at(which).path);
        }

        // Where each one is joined to its wire of the bus. The one that went past the other wire on
        // the way has no dot there: on a schematic a crossing is a join only where there is a dot.
        painter->setPen(QPen{theme.panel, 1.2});
        for (const std::size_t which : {kCanHigh, kCanLow}) {
            painter->setBrush(conductor.at(which));
            painter->drawEllipse(shape.wires.at(which).railEnd, 3.0, 3.0);
        }

        // Which way the frames go: an arrowhead for each half of the drop that exists, between the
        // two wires, along the way the frames leave. The two halves of one drop are one pair, and
        // each puts its arrow at its own place on it.
        const qreal length = shape.path.length();
        if (length > 46.0) {
            const PolylinePoint place =
                alongPolyline(shape.path, length * (shape.towardBus ? 0.62 : 0.38));
            if (place.found) {
                const QPointF direction = shape.towardBus ? place.direction : -place.direction;
                const QPointF across{-direction.y(), direction.x()};
                const QPointF back = place.at - direction * 5.0;

                QPainterPath head;
                head.moveTo(place.at);
                head.lineTo(back + across * 2.7);
                head.lineTo(back - across * 2.7);
                head.closeSubpath();

                painter->setPen(Qt::NoPen);
                painter->setBrush(quiet);
                painter->drawPath(head);
            }
        }

        // What the wire carries: a pill, when it has been limited, and also when
        // the wire is under the pointer, so that there is something to click to
        // limit it.
        const CanFormatMatch format = model->frameFormat(cgo.connectionId());
        if (!shape.towardBus && (format != CanFormatMatch::Any || selected || hovered)) {
            const QRectF pill = cgo.mapFromScene(frameFormatPillRect(cgo, *model)).boundingRect();

            const bool limited = format != CanFormatMatch::Any;
            const QString text = format == CanFormatMatch::StandardOnly   ? QStringLiteral("11-bit")
                                 : format == CanFormatMatch::ExtendedOnly ? QStringLiteral("29-bit")
                                                                          : QStringLiteral("ALL");

            const QColor accent = theme.accent;
            painter->setPen(QPen{limited ? accent : theme.border, 1.0});
            painter->setBrush(limited ? mixed(theme.panel, accent, 0.25) : theme.panel);
            painter->drawRoundedRect(pill, 7.5, 7.5);

            painter->setFont(m_panel.m_badgeFont);
            painter->setPen(limited ? accentOnSurface(accent, theme, 140) : theme.textMuted);
            painter->drawText(pill, Qt::AlignCenter, text);
        }
        return;
    }

    // --- Any other wire ---------------------------------------------------
    PortType type = PortType::Frames;
    if (const NodeTypeInfo* info = model->info(cgo.connectionId().outNodeId);
        info != nullptr && cgo.connectionId().outPortIndex < info->outputs.size()) {
        type = info->outputs[cgo.connectionId().outPortIndex].type;
    }

    const QColor colour = typeColour(type, theme);

    if (selected || hovered) {
        painter->setPen(QPen{withAlpha(colour, 90), 7.0, Qt::SolidLine, Qt::RoundCap});
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(shape.path);
    }

    painter->setPen(QPen{selected ? colour.lighter(130) : withAlpha(colour, 215),
                         selected ? 2.6 : 2.0,
                         Qt::SolidLine,
                         Qt::RoundCap});
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(shape.path);
}

// ---------------------------------------------------------------------------
// The node painter
// ---------------------------------------------------------------------------

PipelineNodePainter::PipelineNodePainter(const CanvasPanel& panel)
    : m_panel{panel}
{ }

void PipelineNodePainter::paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const
{
    const PipelineGraphModel* model = m_panel.m_model.get();
    if (model == nullptr) {
        return;
    }

    const NodeDescription* node = model->node(ngo.nodeId());
    if (node == nullptr) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    if (model->isBus(ngo.nodeId())) {
        paintRail(painter, ngo, *node);
    } else if (model->isExpanded(ngo.nodeId())) {
        paintCard(painter, ngo, *node);
    } else {
        paintChip(painter, ngo, *node);
    }
}

void PipelineNodePainter::paintPorts(QPainter* painter,
                                     QtNodes::NodeGraphicsObject& ngo,
                                     const QRectF& rect) const
{
    const PipelineGraphModel* model = m_panel.m_model.get();
    const NodeTypeInfo* info = model->info(ngo.nodeId());
    if (info == nullptr) {
        return;
    }

    const Theme& theme = m_panel.m_theme;
    const QtNodes::NodeId id = ngo.nodeId();
    QtNodes::AbstractNodeGeometry& geometry = ngo.nodeScene()->nodeGeometry();

    // A wire is being dragged over this block: say which of its ports would take
    // it. QtNodes' own painter does this by growing the ports it would land on.
    const QtNodes::ConnectionGraphicsObject* dragged = ngo.nodeState().connectionForReaction();

    for (const QtNodes::PortType portType : {QtNodes::PortType::In, QtNodes::PortType::Out}) {
        const std::vector<PortDescriptor>& ports =
            portType == QtNodes::PortType::In ? info->inputs : info->outputs;

        for (std::size_t index = 0; index < ports.size(); ++index) {
            const auto portIndex = static_cast<QtNodes::PortIndex>(index);
            const auto wires = model->connections(id, portType, portIndex);

            // A port whose wires all go to a bus has no ring: the drop ends in
            // the dot painted where it meets the block, which is not always at
            // the port - a module under its bus is tapped from the top whether it
            // is its input or its output that is wired.
            const bool onlyBus =
                !wires.empty() && std::ranges::all_of(wires, [model](const auto& wire) {
                    return model->isBusTap(wire);
                });
            if (onlyBus) {
                continue;
            }

            const QPointF at = geometry.portPosition(id, portType, portIndex);
            QColor colour = typeColour(ports[index].type, theme);
            qreal radius = 4.2;

            if (dragged != nullptr && dragged->connectionState().requiredPort() == portType) {
                const bool possible = model->connectionPossible(
                    QtNodes::makeCompleteConnectionId(dragged->connectionId(), id, portIndex));

                radius = possible ? 6.4 : 3.6;
                colour = possible ? theme.success : theme.textMuted;
            }

            painter->setPen(QPen{theme.panel, 1.8});
            painter->setBrush(wires.empty() ? theme.panel : colour);
            painter->drawEllipse(at, radius, radius);

            if (wires.empty()) {
                painter->setPen(QPen{colour, 1.6});
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(at, radius - 0.6, radius - 0.6);
            }
        }
    }

    // And where the drops to a bus meet the block.
    QtNodes::BasicGraphicsScene* scene = ngo.nodeScene();
    for (const QtNodes::NodeId bus : model->busesOf(id)) {
        const QRectF railRect = sceneRectOf(*scene, bus);
        const QRectF selfRect = sceneRectOf(*scene, id);
        if (railRect.isNull() || selfRect.isNull()) {
            continue;
        }

        // Two, as it is two wires that leave: CAN_H on the left and CAN_L on the right.
        const bool below = moduleHangsBelow(selfRect, railRect);
        const qreal edge = below ? rect.top() : rect.bottom();

        painter->setPen(QPen{theme.panel, 1.2});
        painter->setBrush(theme.warning);
        painter->drawEllipse(QPointF{rect.center().x() - layout::kDropWireOffset, edge}, 2.8, 2.8);
        painter->setBrush(theme.success);
        painter->drawEllipse(QPointF{rect.center().x() + layout::kDropWireOffset, edge}, 2.8, 2.8);
    }

    // QtNodes' painter lets go of the wire it was shown once it has drawn the
    // ports, and the next paint of a block nobody is dragging over is a plain one.
    if (dragged != nullptr) {
        ngo.nodeState().resetConnectionForReaction();
    }
}

void PipelineNodePainter::paintChip(QPainter* painter,
                                    QtNodes::NodeGraphicsObject& ngo,
                                    const NodeDescription& node) const
{
    const Theme& theme = m_panel.m_theme;
    const QSizeF size{ngo.nodeScene()->nodeGeometry().size(ngo.nodeId())};
    const QRectF rect{QPointF{0.0, 0.0}, size};

    const bool selected = ngo.isSelected();
    const bool hovered = ngo.nodeState().hovered();

    // How far in the canvas is. Cached with the block, so it is asked again only
    // when the zoom changes and the picture is made again anyway.
    const qreal zoom = std::max(0.05, painter->worldTransform().m11());
    const bool overview = zoom < 0.5;

    const QColor accent = accentForNodeType(node.typeName, theme);
    const CanvasPanel::ChipInfo chip = m_panel.chipInfo(node);

    if (!node.enabled) {
        painter->setOpacity(0.55);
    }

    // Selected: a halo, so that the block that is selected is not a matter of a
    // border a pixel thicker.
    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(theme.accent, 64));
        painter->drawRoundedRect(rect.adjusted(-3.0, -3.0, 3.0, 3.0), 10.0, 10.0);
    }

    QLinearGradient body{0.0, 0.0, 0.0, size.height()};
    body.setColorAt(0.0, mixed(theme.panelAlternate, theme.panel, 0.3));
    body.setColorAt(1.0, theme.panel);

    QColor border = mixed(theme.border, accent, 0.55);
    if (chip.alert) {
        border = theme.error;
    } else if (selected) {
        border = theme.accent;
    } else if (hovered) {
        border = accent.lighter(135);
    }

    painter->setPen(QPen{border, selected || chip.alert ? 2.0 : 1.3});
    painter->setBrush(body);
    painter->drawRoundedRect(rect.adjusted(0.7, 0.7, -0.7, -0.7), 7.0, 7.0);

    // The accent stripe: what kind of block this is, readable from across the
    // room.
    QPainterPath stripe;
    stripe.addRoundedRect(QRectF{1.6, 1.6, 4.2, size.height() - 3.2}, 2.0, 2.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(chip.alert ? theme.error : accent);
    painter->drawPath(stripe);

    const QString name = QString::fromStdString(node.id);
    const bool isScript = isScriptNodeType(node.typeName);

    // --- Overview: the name, big enough to read from where the canvas is ----
    if (overview) {
        QFont font = m_panel.m_titleFont;
        font.setPixelSize(static_cast<int>(std::clamp(8.5 / zoom, 12.0, 26.0)));

        const QRectF area{14.0, 0.0, size.width() - 26.0, size.height()};
        painter->setFont(font);
        painter->setPen(theme.text);
        painter->drawText(area,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetricsF{font}.elidedText(name, Qt::ElideRight, area.width()));

        paintPorts(painter, ngo, rect);
        return;
    }

    // --- The chip -----------------------------------------------------------
    if (ThemeManager* themes = ThemeManager::instance()) {
        themes
            ->icon(iconNameForNodeType(QString::fromStdString(node.typeName)),
                   accentOnSurface(accent, theme, 130))
            .paint(painter, QRect{14, 15, 18, 18});
    }

    // Room kept at the right for the status light, and for the pencil of a
    // block that has a script.
    const qreal right = size.width() - (isScript ? 52.0 : 30.0);
    const QRectF titleArea{40.0, 6.0, std::max(20.0, right - 40.0), 18.0};

    painter->setFont(m_panel.m_titleFont);
    painter->setPen(theme.text);
    painter->drawText(
        titleArea,
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF{m_panel.m_titleFont}.elidedText(name, Qt::ElideRight, titleArea.width()));

    const QRectF lineArea{40.0, 25.0, size.width() - 40.0 - 28.0, 16.0};
    painter->setFont(m_panel.m_subFont);
    painter->setPen(chip.live ? accentOnSurface(accent, theme, 140) : theme.textMuted);
    painter->drawText(
        lineArea,
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF{m_panel.m_subFont}.elidedText(chip.line, Qt::ElideRight, lineArea.width()));

    // The status light: off, ready, running, or in trouble.
    QColor light = theme.textMuted;
    if (node.enabled) {
        light = m_panel.m_simulationRunning ? (chip.alert ? theme.error : theme.success)
                                            : withAlpha(accent, 190);
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(light);
    painter->drawEllipse(QPointF{size.width() - 13.0, 14.0}, 3.4, 3.4);

    // The Edit pencil of a block that is a script.
    if (isScript) {
        const bool over = m_panel.m_hoveringScriptEdit && m_panel.m_hoveredScriptNodeId == name;
        const QRectF pencil = layout::chipScriptButton(rect);

        painter->setPen(QPen{withAlpha(accent.lighter(over ? 140 : 110), over ? 255 : 160), 1.0});
        painter->setBrush(withAlpha(accent, over ? 90 : 40));
        painter->drawRoundedRect(pencil, 4.0, 4.0);

        if (ThemeManager* themes = ThemeManager::instance()) {
            themes->icon(QStringLiteral("edit"), accentOnSurface(accent, theme, 150))
                .paint(painter, pencil.adjusted(3.0, 3.0, -3.0, -3.0).toRect());
        }
    }

    // The way to open a block into a card, for the block that is under the
    // pointer or selected. Not on every block all the time: twenty chevrons are
    // a texture.
    if (hovered || selected) {
        const QRectF toggle = layout::chipExpandButton(rect);
        painter->setPen(QPen{theme.textMuted, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
        painter->setBrush(Qt::NoBrush);

        const QPointF c = toggle.center();
        QPainterPath chevron;
        chevron.moveTo(c + QPointF{-3.6, -1.8});
        chevron.lineTo(c + QPointF{0.0, 1.8});
        chevron.lineTo(c + QPointF{3.6, -1.8});
        painter->drawPath(chevron);
    }

    paintPorts(painter, ngo, rect);
}

void PipelineNodePainter::paintRail(QPainter* painter,
                                    QtNodes::NodeGraphicsObject& ngo,
                                    const NodeDescription& node) const
{
    const PipelineGraphModel* model = m_panel.m_model.get();
    const Theme& theme = m_panel.m_theme;
    const QSizeF size{ngo.nodeScene()->nodeGeometry().size(ngo.nodeId())};
    const QRectF rect{QPointF{0.0, 0.0}, size};

    const bool selected = ngo.isSelected();
    const bool hovered = ngo.nodeState().hovered();

    // A wire is being dragged over the rail, and it would land: say so by lighting the rail. A wire
    // that would be refused - frames to a plot's signals - gets no encouragement.
    bool receiving = false;
    if (const QtNodes::ConnectionGraphicsObject* dragged =
            ngo.nodeState().connectionForReaction()) {
        receiving = model->connectionPossible(
            QtNodes::makeCompleteConnectionId(dragged->connectionId(), ngo.nodeId(), 0));

        // Let go of it, as QtNodes' own painter does once it has looked: the next paint of a rail
        // nobody is dragging over is a plain one.
        ngo.nodeState().resetConnectionForReaction();
    }

    const qreal zoom = std::max(0.05, painter->worldTransform().m11());
    const bool overview = zoom < 0.5;

    if (!node.enabled) {
        painter->setOpacity(0.55);
    }

    const qreal middle = size.height() * 0.5;

    // --- The two wires ------------------------------------------------------
    //
    // CAN_H and CAN_L, in the colours the cable has them in. They are what the
    // modules are wired to, so they are what is brightest on the canvas.
    const qreal wiresFrom = layout::kRailPlateWidth - 2.0;
    const qreal wiresTo = size.width() - 12.0;

    if (selected || receiving) {
        painter->setPen(QPen{withAlpha(theme.accent, receiving ? 110 : 80),
                             layout::kRailWireOffset * 2.0 + 11.0,
                             Qt::SolidLine,
                             Qt::RoundCap});
        painter->drawLine(QPointF{wiresFrom, middle}, QPointF{wiresTo, middle});
    }

    const QColor canHigh = theme.warning;
    const QColor canLow = theme.success;

    painter->setPen(QPen{canHigh, 2.6, Qt::SolidLine, Qt::RoundCap});
    painter->drawLine(QPointF{wiresFrom, middle - layout::kRailWireOffset},
                      QPointF{wiresTo, middle - layout::kRailWireOffset});
    painter->setPen(QPen{canLow, 2.6, Qt::SolidLine, Qt::RoundCap});
    painter->drawLine(QPointF{wiresFrom, middle + layout::kRailWireOffset},
                      QPointF{wiresTo, middle + layout::kRailWireOffset});

    // The terminating resistor at the far end, as on the real cable: 120 ohm
    // between the two wires.
    painter->setPen(QPen{theme.textMuted, 1.4});
    painter->setBrush(theme.panel);
    painter->drawRoundedRect(QRectF{wiresTo - 2.5, middle - 6.0, 5.0, 12.0}, 1.5, 1.5);

    // --- The plate that says which bus it is ---------------------------------
    const QRectF plate{0.0, 2.0, layout::kRailPlateWidth, size.height() - 4.0};

    QColor border = mixed(theme.border, theme.accent, 0.55);
    if (selected) {
        border = theme.accent;
    } else if (hovered || receiving) {
        border = theme.accentHover;
    }

    painter->setPen(QPen{border, selected ? 2.0 : 1.3});
    painter->setBrush(theme.panel);
    painter->drawRoundedRect(plate.adjusted(0.7, 0.7, -0.7, -0.7), 7.0, 7.0);

    const std::int64_t channel = model->busChannel(ngo.nodeId());
    const QString name = QString::fromStdString(node.id);

    // What the interface behind the channel is, and how it is doing.
    QString line = CanvasPanel::tr("CAN %1").arg(channel + 1);
    if (channel >= 0 && channel < m_panel.m_channelStatuses.size()) {
        const ChannelStatus& status = m_panel.m_channelStatuses.at(static_cast<qsizetype>(channel));

        if (m_panel.m_simulationRunning) {
            line = CanvasPanel::tr("CAN %1 • %2 f/s • %3%")
                       .arg(channel + 1)
                       .arg(status.framesPerSecond, 0, 'f', 0)
                       .arg(status.busLoadPercent, 0, 'f', 0);
        } else if (status.bitrate > 0) {
            line = CanvasPanel::tr("CAN %1 • %2 kbit/s")
                       .arg(channel + 1)
                       .arg(status.bitrate / 1000.0, 0, 'g', 4);
        }
    }

    if (overview) {
        QFont font = m_panel.m_titleFont;
        font.setPixelSize(static_cast<int>(std::clamp(9.0 / zoom, 12.0, 24.0)));
        painter->setFont(font);
        painter->setPen(theme.text);
        painter->drawText(
            plate.adjusted(12.0, 0.0, -8.0, 0.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetricsF{font}.elidedText(name, Qt::ElideRight, plate.width() - 20.0));
        return;
    }

    if (ThemeManager* themes = ThemeManager::instance()) {
        themes->icon(QStringLiteral("network"), accentOnSurface(theme.accent, theme, 125))
            .paint(painter, QRect{11, static_cast<int>(middle) - 9, 18, 18});
    }

    const QRectF titleArea{36.0, plate.top() + 3.0, plate.width() - 44.0, 15.0};
    painter->setFont(m_panel.m_titleFont);
    painter->setPen(theme.text);
    painter->drawText(
        titleArea,
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF{m_panel.m_titleFont}.elidedText(name, Qt::ElideRight, titleArea.width()));

    const QRectF lineArea{36.0, plate.top() + 17.0, plate.width() - 44.0, 12.0};
    painter->setFont(m_panel.m_subFont);
    painter->setPen(m_panel.m_simulationRunning ? accentOnSurface(theme.accent, theme, 135)
                                                : theme.textMuted);
    painter->drawText(
        lineArea,
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF{m_panel.m_subFont}.elidedText(line, Qt::ElideRight, lineArea.width()));
}

void PipelineNodePainter::paintCard(QPainter* painter,
                                    QtNodes::NodeGraphicsObject& ngo,
                                    const NodeDescription& node) const
{
    const QSizeF size{ngo.nodeScene()->nodeGeometry().size(ngo.nodeId())};
    const QRectF rect{QPointF{0.0, 0.0}, size};

    if (!node.enabled) {
        painter->setOpacity(0.55);
    }

    m_panel.paintCardBody(painter, ngo, rect, node);
    paintPorts(painter, ngo, rect);
}

} // namespace torquebus::ui
