// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/CanvasPanel.h"

#include "core/pipeline/BusGrouping.h"
#include "ui/canvas/CanvasArrange.h"
#include "ui/canvas/CanvasLayout.h"
#include "ui/canvas/CanvasPainters.h"
#include "ui/canvas/PipelineGraphModel.h"
#include "ui/common/Motion.h"
#include "ui/common/NodeAppearance.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFrame>
#include <QGraphicsItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QRadialGradient>
#include <QShowEvent>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QToolTip>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>
#include <utility>

namespace torquebus::ui {
namespace {

/// The role holding a palette entry's catalog type name.
constexpr int kTypeNameRole = Qt::UserRole + 1;

[[nodiscard]] QColor withAlpha(QColor colour, int alpha)
{
    colour.setAlpha(alpha);
    return colour;
}

/// Duration of one full cycle of packet motion along a wire on the Canvas.
constexpr int kFlowCycleDurationMs = 1350;

/// How far the view zooms. QtNodes allows 0.3 to 2, and a pipeline of a few dozen blocks does not
/// fit in a window of ordinary height at 0.3 - the J1939 example needs 0.31 in 730 pixels - so Fit
/// would have stopped short of "everything".
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 2.0;

/// Fit does not enlarge past real size: one block alone in a wide window is not blown up to twice
/// its size.
constexpr double kFitMaxZoom = 1.0;

/// What Fit leaves free around the blocks, in viewport pixels.
constexpr int kFitMarginPx = 24;

/// The card the foreground paints under a TinyML block that has been opened: where it starts,
/// and its size.
[[nodiscard]] QRectF tinyMlCardRect(const QRectF& nodeRect)
{
    const qreal width = std::max(nodeRect.width(), layout::kTinyMlCardMinWidth);
    return QRectF{nodeRect.center().x() - (width * 0.5),
                  nodeRect.bottom() + layout::kTinyMlCardGap,
                  width,
                  layout::kTinyMlCardHeight};
}

/// An action on the canvas that QtNodes' own undo commands do not cover - arranging the blocks,
/// grouping a pipeline on a bus - as a pair of closures.
///
/// Pushed *after* the action has been done, so the redo() that pushing makes is the first thing
/// skipped.
class CanvasActionCommand final : public QUndoCommand {
public:
    CanvasActionCommand(const QString& text, std::function<void()> undo, std::function<void()> redo)
        : QUndoCommand{text}
        , m_undo{std::move(undo)}
        , m_redo{std::move(redo)}
    { }

    void undo() override { m_undo(); }

    void redo() override
    {
        if (m_pushed) {
            m_pushed = false;
            return;
        }
        m_redo();
    }

private:
    std::function<void()> m_undo;
    std::function<void()> m_redo;
    bool m_pushed{true};
};

/// The canvas view. QtNodes' own view fits the whole scene to the window every time it is shown:
/// that is how it lays itself out the first time, and also how it throws away the zoom and the
/// position the user chose each time they come back to the tab. Here the panel says when the canvas
/// is fitted - once it has been filled, and whenever it is asked to - and the view otherwise stays
/// where it was left.
class CanvasView final : public QtNodes::GraphicsView {
public:
    using QtNodes::GraphicsView::GraphicsView;

    /// Called each time the view is shown, once the layout above it has given it its size.
    void onShown(std::function<void()> callback) { m_shown = std::move(callback); }

protected:
    void showEvent(QShowEvent* event) override
    {
        // Past QtNodes' own showEvent, which is the one that fits the scene.
        QGraphicsView::showEvent(event);

        if (m_shown) {
            m_shown();
        }
    }

private:
    std::function<void()> m_shown;
};

/// Scene subclass that supplies the right-click context menu and delegates
/// foreground painting (animated wire packets, live node frame/signal cards,
/// and the TinyML neural network card) to `CanvasPanel`.
class CanvasScene final : public QtNodes::BasicGraphicsScene {
public:
    using MenuFactory = std::function<QMenu*(QPointF)>;
    using ForegroundPainter = std::function<void(QPainter*, const QRectF&)>;

    CanvasScene(QtNodes::AbstractGraphModel& model,
                MenuFactory factory,
                ForegroundPainter foregroundPainter,
                QObject* parent)
        : QtNodes::BasicGraphicsScene{model, parent}
        , m_factory{std::move(factory)}
        , m_foregroundPainter{std::move(foregroundPainter)}
    { }

    QMenu* createSceneMenu(QPointF scenePos) override
    {
        return m_factory ? m_factory(scenePos) : nullptr;
    }

    void onConnectionCreated(QtNodes::ConnectionId connectionId) override
    {
        QtNodes::BasicGraphicsScene::onConnectionCreated(connectionId);
        raiseWhatTheBusDelivers(connectionId);
    }

    void onModelReset() override
    {
        QtNodes::BasicGraphicsScene::onModelReset();

        for (const QtNodes::NodeId node : graphModel().allNodeIds()) {
            for (const QtNodes::ConnectionId& wire : graphModel().allConnectionIds(node)) {
                raiseWhatTheBusDelivers(wire);
            }
        }
    }

protected:
    void drawForeground(QPainter* painter, const QRectF& rect) override
    {
        QtNodes::BasicGraphicsScene::drawForeground(painter, rect);
        if (m_foregroundPainter) {
            m_foregroundPainter(painter, rect);
        }
    }

private:
    /// The two halves of a drop - what a module puts on the bus and what the bus delivers to it -
    /// are two items drawn along one pair of wires, and the half that carries the pill has to be
    /// the one painted last, or the wires of the other are drawn across it. Which was created first
    /// depends on which end somebody dragged from, or on the order of a project file, so the order
    /// is made here: what the bus delivers goes above what the module sends. (Connections are all
    /// below blocks.)
    void raiseWhatTheBusDelivers(QtNodes::ConnectionId connectionId)
    {
        const auto* model = qobject_cast<const PipelineGraphModel*>(&graphModel());
        if (model == nullptr || !model->isBusTap(connectionId)
            || model->isBus(connectionId.inNodeId)) {
            return;
        }

        if (QtNodes::ConnectionGraphicsObject* item = connectionGraphicsObject(connectionId)) {
            item->setZValue(layout::kDeliveredToModuleZ);
        }
    }

    MenuFactory m_factory;
    ForegroundPainter m_foregroundPainter;
};

[[nodiscard]] QFont makeMonospaceFont(int pixelSize, QFont::Weight weight = QFont::Medium)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("SF Mono"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Courier New"),
    });
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

[[nodiscard]] QString styleJson(const Theme& theme)
{
    const auto rgb = [](const QColor& colour) {
        return QJsonArray{colour.red(), colour.green(), colour.blue()};
    };

    QJsonObject view;
    view["BackgroundColor"] = rgb(theme.canvas);
    view["FineGridColor"] = rgb(theme.canvasGridFine);
    view["CoarseGridColor"] = rgb(theme.canvasGridCoarse);

    QJsonObject node;
    node["NormalBoundaryColor"] = rgb(theme.border);
    node["SelectedBoundaryColor"] = rgb(theme.accent);
    node["GradientColor0"] = rgb(theme.panelAlternate);
    node["GradientColor1"] = rgb(theme.panel);
    node["GradientColor2"] = rgb(theme.panel);
    node["GradientColor3"] = rgb(theme.panel);
    node["ShadowColor"] = rgb(theme.separator);
    node["ShadowEnabled"] = false;
    node["FontColor"] = rgb(theme.text);
    node["FontColorFaded"] = rgb(theme.textMuted);
    node["ConnectionPointColor"] = rgb(theme.textMuted);
    node["FilledConnectionPointColor"] = rgb(theme.accent);
    node["ErrorColor"] = rgb(theme.error);
    node["WarningColor"] = rgb(theme.warning);
    node["ToolTipIconColor"] = rgb(theme.text);
    node["PenWidth"] = 1.4;
    node["HoveredPenWidth"] = 2.0;
    node["ConnectionPointDiameter"] = 9.0;
    node["Opacity"] = 1.0;

    QJsonObject connection;
    connection["ConstructionColor"] = rgb(theme.textMuted);
    connection["NormalColor"] = rgb(theme.accent);
    connection["SelectedColor"] = rgb(theme.accentHover);
    connection["SelectedHaloColor"] = rgb(theme.accent);
    connection["HoveredColor"] = rgb(theme.accentHover);
    connection["LineWidth"] = 2.4;
    connection["ConstructionLineWidth"] = 2.0;
    // Also how far QtNodes grows the area a wire is drawn in beyond its two ends. The pill on a
    // drop to a bus sits a little off the line, so the area has to reach it.
    connection["PointDiameter"] = 26.0;
    connection["UseDataDefinedColors"] = false;

    QJsonObject root;
    root["GraphicsViewStyle"] = view;
    root["NodeStyle"] = node;
    root["ConnectionStyle"] = connection;

    return QString::fromUtf8(QJsonDocument{root}.toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString regimeName(quint64 regimeCode)
{
    switch (regimeCode) {
    case 0:
        return QStringLiteral("IDLE");
    case 1:
        return QStringLiteral("CRUISE");
    case 2:
        return QStringLiteral("HIGH LOAD");
    case 3:
        return QStringLiteral("THERMAL STRESS");
    case 4:
        return QStringLiteral("ANOMALY");
    default:
        return QStringLiteral("ACTIVE");
    }
}

[[nodiscard]] quint64 counterValue(const NodeStatus* status, const QString& label)
{
    if (status == nullptr) {
        return 0;
    }
    for (const NodeCounter& counter : status->counters) {
        if (counter.label == label) {
            return counter.value;
        }
    }
    return 0;
}

[[nodiscard]] QString defaultVehicleEcuScript()
{
    return QStringLiteral(
        "-- Cyclic Virtual Vehicle ECU (Speed 0x101 + Engine Temp 0x102)\n"
        "local kSpeedId = parameters.speed_id or 0x101\n"
        "local kTempId = parameters.temp_id or 0x102\n"
        "local speed = 0.0\n"
        "local temperature = 72.0\n"
        "local next_speed_us = 0\n"
        "local next_temp_us = 0\n\n"
        "function on_enable()\n"
        "    log_message(string.format(\"Virtual Vehicle ECU active on 0x%03X / 0x%03X\", "
        "kSpeedId, kTempId))\n"
        "    set_timer(80)\n"
        "end\n\n"
        "function on_timer()\n"
        "    local now = get_time_us()\n"
        "    speed = speed + 1.8\n"
        "    if speed > 110.0 then speed = 0.0 end\n"
        "    temperature = temperature + (speed > 55.0 and 0.08 or -0.04)\n"
        "    if temperature < 72.0 then temperature = 72.0 end\n"
        "    if temperature > 96.0 then temperature = 96.0 end\n\n"
        "    if now >= next_speed_us then\n"
        "        next_speed_us = now + 80000\n"
        "        emit(kSpeedId, string.pack(\"<I2\", math.floor(speed / 0.1 + 0.5) & 0xFFFF))\n"
        "    end\n"
        "    if now >= next_temp_us then\n"
        "        next_temp_us = now + 240000\n"
        "        emit(kTempId, string.pack(\"<I1\", math.floor(temperature + 40.5) & 0xFF))\n"
        "    end\n"
        "end\n");
}

[[nodiscard]] QString formatPhysicalValue(double value, std::string_view unit)
{
    const double absVal = std::abs(value);
    QString number;
    if (std::abs(value - std::round(value)) < 1e-4 && absVal < 1.0e9) {
        number = QString::number(static_cast<qint64>(std::llround(value)));
    } else if (absVal >= 100.0) {
        number = QString::number(value, 'f', 1);
    } else {
        number = QString::number(value, 'f', 2);
    }

    if (!unit.empty()) {
        number +=
            QLatin1Char(' ') + QString::fromUtf8(unit.data(), static_cast<qsizetype>(unit.size()));
    }
    return number;
}

[[nodiscard]] bool frameMatchesNode(const CanFrame& frame,
                                    const NodeDescription& node,
                                    const GraphDescription* graph = nullptr)
{
    if (frame.rtr || frame.error) {
        return false;
    }

    const std::int64_t nodeChannel = node.parameters.integer("channel", -1);
    if (nodeChannel >= 0 && static_cast<std::int64_t>(frame.channel) != nodeChannel) {
        return false;
    }

    if (node.typeName == "can.filter") {
        const std::int64_t pgn = node.parameters.integer("pgn", -1);
        if (pgn >= 0) {
            return frame.isExtended()
                   && (((frame.identifier >> 8U) & 0x3FFFFU) == static_cast<std::uint32_t>(pgn));
        }
        const auto fromId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("from", 0)));
        const auto toId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("to", 0x1FFFFFFF)));
        return frame.identifier >= fromId && frame.identifier <= toId;
    }

    if (node.typeName == "tinyml.ecu") {
        const bool isJ1939 = node.parameters.boolean("j1939", false);
        const auto defaultId = isJ1939 ? 0x18FD0555 : 0x105;
        const auto outId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("outputCanId", defaultId)));
        return frame.identifier == outId;
    }

    if (node.typeName == "lua.ecu") {
        const std::int64_t sa = node.parameters.integer("sa", -1);
        if (sa >= 0) {
            return frame.isExtended()
                   && ((frame.identifier & 0xFFU) == static_cast<std::uint32_t>(sa));
        }
        const std::int64_t speedId = node.parameters.integer("speed_id", -1);
        const std::int64_t tempId = node.parameters.integer("temp_id", -1);
        if (speedId >= 0 || tempId >= 0) {
            return static_cast<std::int64_t>(frame.identifier) == speedId
                   || static_cast<std::int64_t>(frame.identifier) == tempId;
        }
    }

    if (node.typeName == "can.transmit" && graph != nullptr) {
        for (const EdgeDescription& edge : graph->edges()) {
            if (edge.toNode == node.id) {
                if (const NodeDescription* source = graph->find(edge.fromNode)) {
                    return frameMatchesNode(frame, *source, nullptr);
                }
            }
        }
    }

    if (node.typeName == "j1939.decoder") {
        return frame.isExtended();
    }

    return true;
}

} // namespace

void CanvasPanel::decodeFromDatabasesOrBuiltin(const NodeDescription& node,
                                               NodeFrameTelemetry& out) const
{
    // If not running, display rich block configuration & specs so panels are never empty in
    // standby:
    if (!m_simulationRunning) {
        if (node.typeName == "can.source") {
            const std::int64_t ch = node.parameters.integer("channel", 0);
            out.messageName = QStringLiteral("CAN_Hardware_Rx");
            out.decodedLine1 =
                QStringLiteral("Channel: CAN %1  •  Bitrate: 500 kbps  •  Classic CAN").arg(ch + 1);
            out.decodedLine2 =
                QStringLiteral("Mode: Normal  •  RX FIFO Buffer: Active  •  Status: Ready");
            return;
        }
        if (node.typeName == "can.transmit") {
            const std::int64_t ch = node.parameters.integer("channel", 0);
            out.messageName = QStringLiteral("CAN_Hardware_Tx");
            out.decodedLine1 =
                QStringLiteral("Channel: CAN %1  •  Transmit Buffer: Ready").arg(ch + 1);
            out.decodedLine2 = QStringLiteral("TX Queue: Non-blocking  •  Status: Standby");
            return;
        }
        if (node.typeName == "lua.ecu") {
            const std::int64_t ch = node.parameters.integer("channel", 0);
            const std::int64_t sa = node.parameters.integer("sa", -1);
            if (sa >= 0) {
                if (sa == 0x00 || node.id == "ecu_engine") {
                    out.messageName = QStringLiteral("J1939_EMS_Engine");
                    out.decodedLine1 =
                        QStringLiteral("SA: 0x00 (EMS)  •  TX: EEC1, EEC2, ETC2, ET1");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_engine.lua  •  Cycle: 10 ms (100 Hz)");
                    return;
                }
                if (sa == 0x3D || node.id == "ecu_aftertreatment") {
                    out.messageName = QStringLiteral("J1939_ACM_Aftertreatment");
                    out.decodedLine1 =
                        QStringLiteral("SA: 0x3D (ACM)  •  TX: AT1T1I1, A1SCR, DPF1");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_aftertreatment.lua  •  SCR/DEF State");
                    return;
                }
                if (sa == 0x21 || node.id == "ecu_body") {
                    out.messageName = QStringLiteral("J1939_BCM_BodyControl");
                    out.decodedLine1 = QStringLiteral("SA: 0x21 (BCM)  •  TX: DD1, AIR1, VDC1");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_body.lua  •  Pneumatic & Fuel Tank");
                    return;
                }
                if (sa == 0x0B || node.id == "ecu_brakes") {
                    out.messageName = QStringLiteral("J1939_EBS_BrakeControl");
                    out.decodedLine1 = QStringLiteral("SA: 0x0B (EBS)  •  TX: EBC1, EBC2");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_brakes.lua  •  ABS & Deceleration");
                    return;
                }
                if (sa == 0x37 || node.id == "ecu_switches") {
                    out.messageName = QStringLiteral("J1939_CAB_Switches");
                    out.decodedLine1 = QStringLiteral("SA: 0x37 (CAB)  •  TX: OEL (Cab Switches)");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_switches.lua  •  Indicators & Lamps");
                    return;
                }
                if (sa == 0x17 || node.id == "ecu_cluster_core") {
                    out.messageName = QStringLiteral("J1939_IC_ClusterCore");
                    out.decodedLine1 = QStringLiteral("SA: 0x17 (IC)  •  TX: VDHR, CCVS1");
                    out.decodedLine2 =
                        QStringLiteral("Script: ecu_cluster_core.lua  •  High-Res Odometer");
                    return;
                }
                out.messageName = QStringLiteral("J1939_Virtual_ECU");
                out.decodedLine1 = QStringLiteral("CAN %1  •  SA: 0x%2")
                                       .arg(ch + 1)
                                       .arg(sa, 2, 16, QLatin1Char{'0'})
                                       .toUpper();
                out.decodedLine2 = QStringLiteral("Embedded LuaJIT  •  J1939 Transceiver");
                return;
            }
            const std::int64_t speedId = node.parameters.integer("speed_id", 0x101);
            const std::int64_t tempId = node.parameters.integer("temp_id", 0x102);
            out.messageName = QStringLiteral("VirtualVehicle_ECU");
            out.decodedLine1 = QStringLiteral("CAN %1  •  Speed: 0x%2  •  Temp: 0x%3")
                                   .arg(ch + 1)
                                   .arg(speedId, 3, 16, QLatin1Char{'0'})
                                   .arg(tempId, 3, 16, QLatin1Char{'0'})
                                   .toUpper();
            out.decodedLine2 =
                QStringLiteral("Engine: Embedded LuaJIT 2.1  •  Timer: 80 ms (12.5 Hz)");
            return;
        }
        if (node.typeName == "tinyml.ecu") {
            const bool isJ1939 = node.parameters.boolean("j1939", false);
            const double threshold = node.parameters.real("anomalyThreshold", 65.0);
            if (isJ1939) {
                const std::int64_t outId = node.parameters.integer("outputCanId", 0x18FD0555);
                out.messageName = QStringLiteral("TinyML_J1939_ECU");
                out.decodedLine1 = QStringLiteral("Model: 1D-CNN + TFLM Int8  •  TX: 0x%1")
                                       .arg(outId, 8, 16, QLatin1Char{'0'})
                                       .toUpper();
                out.decodedLine2 =
                    QStringLiteral("J1939 Powertrain Anomaly Detector  •  Threshold: %1%")
                        .arg(threshold, 0, 'f', 0);
                return;
            }
            const std::int64_t outId = node.parameters.integer("outputCanId", 0x105);
            out.messageName = QStringLiteral("TinyML_Virtual_ECU");
            out.decodedLine1 = QStringLiteral("Model: 1D-CNN + TFLM Int8  •  TX ID: 0x%1")
                                   .arg(outId, 3, 16, QLatin1Char{'0'})
                                   .toUpper();
            out.decodedLine2 = QStringLiteral("Anomaly Threshold: %1%  •  Arena: 16 KB Static")
                                   .arg(threshold, 0, 'f', 0);
            return;
        }
        if (node.typeName == "can.filter") {
            const std::int64_t pgn = node.parameters.integer("pgn", -1);
            if (pgn >= 0) {
                QString pgnName = (pgn == 65265)   ? QStringLiteral("CCVS1 (Vehicle Speed)")
                                  : (pgn == 61444) ? QStringLiteral("EEC1 (Engine Speed)")
                                                   : QStringLiteral("J1939 PGN");
                out.messageName = QStringLiteral("J1939_PGN_Filter");
                out.decodedLine1 = QStringLiteral("Pass PGN: 0x%1 (%2)  •  %3")
                                       .arg(pgn, 4, 16, QLatin1Char{'0'})
                                       .toUpper()
                                       .arg(pgn)
                                       .arg(pgnName);
                out.decodedLine2 =
                    QStringLiteral("Policy: Accept matching PGN  •  Drop all outside");
                return;
            }
            const std::int64_t fromId = node.parameters.integer("from", 0x000);
            const std::int64_t toId = node.parameters.integer("to", 0x7FF);
            out.messageName = QStringLiteral("Range_Filter");
            out.decodedLine1 = QStringLiteral("Pass Range: 0x%1 .. 0x%2")
                                   .arg(fromId, 3, 16, QLatin1Char{'0'})
                                   .arg(toId, 3, 16, QLatin1Char{'0'})
                                   .toUpper();
            out.decodedLine2 = QStringLiteral("Policy: Accept within range  •  Drop all outside");
            return;
        }
        if (node.typeName == "j1939.decoder") {
            const std::string path = node.parameters.text("database");
            QString dbName = QStringLiteral("j1939.dbc");
            if (!path.empty()) {
                const QString qp = QString::fromStdString(path);
                const qsizetype sl = std::max(qp.lastIndexOf('/'), qp.lastIndexOf('\\'));
                dbName = sl >= 0 ? qp.mid(sl + 1) : qp;
            }
            out.messageName = QStringLiteral("J1939_Decoder");
            out.decodedLine1 = QStringLiteral("Database: %1  •  SAE J1939-71/73").arg(dbName);
            out.decodedLine2 =
                QStringLiteral("Decodes 29-bit CAN IDs into PGN, SA and physical signals");
            return;
        }
        if (node.typeName == "dbc.decoder") {
            const std::string path = node.parameters.text("database");
            QString dbName = QStringLiteral("Default Database");
            if (!path.empty()) {
                const QString qp = QString::fromStdString(path);
                const qsizetype sl = std::max(qp.lastIndexOf('/'), qp.lastIndexOf('\\'));
                dbName = sl >= 0 ? qp.mid(sl + 1) : qp;
            }
            out.messageName = QStringLiteral("DBC_Decoder");
            out.decodedLine1 = QStringLiteral("Database: %1").arg(dbName);
            out.decodedLine2 =
                QStringLiteral("Decodes raw 8-byte CAN payloads to physical signals");
            return;
        }
        if (node.typeName == "signal.plot") {
            out.messageName = QStringLiteral("TimeSeries_Sink");
            out.decodedLine1 = QStringLiteral("Multi-Signal Time-Series Sink");
            out.decodedLine2 = QStringLiteral("Real-time ring buffer: 100k samples  •  60 FPS");
            return;
        }
        if (node.typeName == "trace.sink") {
            out.messageName = QStringLiteral("Trace_Logger");
            out.decodedLine1 = QStringLiteral("Trace Storage Buffer Sink");
            out.decodedLine2 = QStringLiteral("High-speed lock-free circular frame ring");
            return;
        }
        out.messageName = QString::fromStdString(node.id);
        out.decodedLine1 = QStringLiteral("Type: %1  •  Enabled: %2")
                               .arg(QString::fromStdString(node.typeName))
                               .arg(node.enabled ? QStringLiteral("Yes") : QStringLiteral("No"));
        out.decodedLine2 = QStringLiteral("Pipeline ready  •  Click Start to run");
        return;
    }

    // 1. Check loaded DBC databases first for an exact message + signal match.
    for (const std::shared_ptr<const CanDatabase>& db : m_databases) {
        if (!db) {
            continue;
        }
        const CanMessage* msg = db->find(out.frame.identifier, out.frame.format);
        if (msg == nullptr) {
            continue;
        }

        out.messageName = QString::fromStdString(msg->name);
        const std::vector<const CanSignal*> activeSignals =
            msg->signalsIn(out.frame.data.data(), out.frame.length);

        QStringList decodedItems;
        decodedItems.reserve(static_cast<qsizetype>(activeSignals.size()));
        for (const CanSignal* sig : activeSignals) {
            if (sig == nullptr) {
                continue;
            }
            const std::int64_t raw = sig->rawValue(out.frame.data.data(), out.frame.length);
            const std::string_view enumName = sig->nameForValue(raw);
            QString valStr;
            if (!enumName.empty()) {
                valStr =
                    QString::fromUtf8(enumName.data(), static_cast<qsizetype>(enumName.size()));
            } else {
                const double physical = sig->decode(out.frame);
                valStr = formatPhysicalValue(physical, sig->unit);
            }
            decodedItems.append(
                QStringLiteral("%1: %2").arg(QString::fromStdString(sig->name), valStr));
            if (decodedItems.size() >= 4) {
                break;
            }
        }

        if (!decodedItems.isEmpty()) {
            const qsizetype half = (decodedItems.size() + 1) / 2;
            out.decodedLine1 = decodedItems.mid(0, half).join(QStringLiteral("  •  "));
            if (decodedItems.size() > half) {
                out.decodedLine2 = decodedItems.mid(half).join(QStringLiteral("  •  "));
            } else if (msg->cycleTimeMs > 0) {
                out.decodedLine2 = QStringLiteral("DBC TX: %1  •  Cycle %2 ms")
                                       .arg(QString::fromStdString(msg->transmitter))
                                       .arg(msg->cycleTimeMs);
            }
            return;
        }
    }

    // 2. If this is a Signal Plot node and the PlotStore has live series, show them.
    if (node.typeName == "signal.plot" && m_plotStore != nullptr) {
        const std::vector<SeriesInfo> series = m_plotStore->listSeries();
        if (!series.empty()) {
            QStringList items;
            for (const SeriesInfo& info : series) {
                double val = 0.0;
                std::uint64_t ts = 0;
                if (m_plotStore->latest(info.id, val, ts)) {
                    QString shortName = QString::fromStdString(info.name);
                    const qsizetype dot = shortName.lastIndexOf(QLatin1Char('.'));
                    if (dot >= 0) {
                        shortName = shortName.mid(dot + 1);
                    }
                    items.append(QStringLiteral("%1: %2").arg(shortName,
                                                              formatPhysicalValue(val, info.unit)));
                    if (items.size() >= 4) {
                        break;
                    }
                }
            }
            if (!items.isEmpty()) {
                const qsizetype half = (items.size() + 1) / 2;
                out.decodedLine1 = items.mid(0, half).join(QStringLiteral("  •  "));
                if (items.size() > half) {
                    out.decodedLine2 = items.mid(half).join(QStringLiteral("  •  "));
                }
                if (out.messageName.isEmpty()) {
                    out.messageName = QStringLiteral("SignalSeriesStore");
                }
                return;
            }
        }
    }

    // 3. Built-in Automotive & TinyML Dictionary definitions.
    const std::uint32_t id = out.frame.identifier;
    const auto& d = out.frame.data;

    const auto speedId = static_cast<std::uint32_t>(node.parameters.integer("speed_id", 0x101));
    const auto tempId = static_cast<std::uint32_t>(node.parameters.integer("temp_id", 0x102));
    const auto tinyMlOutId =
        static_cast<std::uint32_t>(node.parameters.integer("outputCanId", 0x105));

    if (id == tinyMlOutId || id == 0x105) {
        out.messageName = QStringLiteral("TinyML_Telemetry");
        const quint64 regime = d[0];
        const quint64 conf = d[1];
        const quint64 anom = d[2];
        const quint64 health = d[3];
        const quint64 infUs = (static_cast<quint64>(d[4]) << 8U) | static_cast<quint64>(d[5]);
        out.decodedLine1 = QStringLiteral("Regime: %1 (%2%)  •  Anom: %3%")
                               .arg(regimeName(regime))
                               .arg(conf)
                               .arg(anom);
        out.decodedLine2 =
            QStringLiteral("ThermalHealth: %1%  •  Latency: %2 us").arg(health).arg(infUs);
        return;
    }

    if (id == speedId || id == 0x101) {
        out.messageName = QStringLiteral("VehicleMotion_ECU");
        const std::uint16_t rawSpeed =
            static_cast<std::uint16_t>(d[0]) | (static_cast<std::uint16_t>(d[1]) << 8U);
        const double speedKmh = static_cast<double>(rawSpeed) * 0.1;
        const int estGear =
            speedKmh < 1.0 ? 0 : std::clamp(static_cast<int>(speedKmh / 18.0) + 1, 1, 6);
        const int estRpm = speedKmh < 1.0 ? 780 : static_cast<int>(950.0 + speedKmh * 34.0);
        out.decodedLine1 = QStringLiteral("VehicleSpeed: %1 km/h").arg(speedKmh, 0, 'f', 1);
        out.decodedLine2 =
            QStringLiteral("EstEngineSpeed: %1 rpm  •  Gear: D%2").arg(estRpm).arg(estGear);
        return;
    }

    if (id == tempId || id == 0x102) {
        out.messageName = QStringLiteral("EngineThermal_ECU");
        const int tempC = static_cast<int>(d[0]) - 40;
        const QString state =
            tempC > 95 ? QStringLiteral("HIGH")
                       : (tempC >= 70 ? QStringLiteral("OPTIMAL") : QStringLiteral("WARMUP"));
        out.decodedLine1 = QStringLiteral("EngineCoolantTemp: %1 °C").arg(tempC);
        out.decodedLine2 =
            QStringLiteral("ThermalState: %1  •  Fan: %2")
                .arg(state, tempC >= 90 ? QStringLiteral("ON") : QStringLiteral("OFF"));
        return;
    }

    if (out.frame.isExtended()) {
        const std::uint32_t pgn = (id >> 8U) & 0x3FFFFU;
        const std::uint32_t sa = id & 0xFFU;
        if (pgn == 61444 && out.frame.length >= 5) {
            out.messageName =
                QStringLiteral("J1939 EEC1 (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            const std::uint16_t rawRpm =
                static_cast<std::uint16_t>(d[3]) | (static_cast<std::uint16_t>(d[4]) << 8U);
            out.decodedLine1 =
                QStringLiteral("EEC1 EngineSpeed: %1 rpm").arg(rawRpm * 0.125, 0, 'f', 0);
            out.decodedLine2 =
                QStringLiteral("DriverDemandTorque: %1%").arg(static_cast<int>(d[1]) - 125);
        } else if (pgn == 65265 && out.frame.length >= 3) {
            out.messageName =
                QStringLiteral("J1939 CCVS1 (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            const std::uint16_t rawSpeed =
                static_cast<std::uint16_t>(d[1]) | (static_cast<std::uint16_t>(d[2]) << 8U);
            const double speedKmh = static_cast<double>(rawSpeed) * 0.00390625;
            out.decodedLine1 = QStringLiteral("VehicleSpeed: %1 km/h").arg(speedKmh, 0, 'f', 1);
            out.decodedLine2 =
                QStringLiteral("Brake: %1  •  Cruise: %2")
                    .arg(((d[3] >> 4U) & 0x3U) == 1 ? QStringLiteral("ON") : QStringLiteral("OFF"))
                    .arg((d[3] & 0x1U) != 0 ? QStringLiteral("ON") : QStringLiteral("OFF"));
        } else if (pgn == 65276 && out.frame.length >= 2) {
            out.messageName =
                QStringLiteral("J1939 DD1 (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            const double fuelPct = static_cast<double>(d[1]) * 0.4;
            out.decodedLine1 = QStringLiteral("DashDisplay FuelLevel: %1%").arg(fuelPct, 0, 'f', 1);
            out.decodedLine2 = QStringLiteral("Source: BCM (0x21)  •  Fuel Sensor");
        } else if (pgn == 65262 && out.frame.length >= 1) {
            out.messageName =
                QStringLiteral("J1939 ET1 (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            const int coolantC = static_cast<int>(d[0]) - 40;
            out.decodedLine1 = QStringLiteral("EngineCoolantTemp: %1 °C").arg(coolantC);
            out.decodedLine2 = QStringLiteral("Engine Thermal Management");
        } else if (pgn == 65270 && out.frame.length >= 4) {
            out.messageName = QStringLiteral("J1939 AT1T1I1 (SA 0x%1)")
                                  .arg(sa, 2, 16, QLatin1Char{'0'})
                                  .toUpper();
            out.decodedLine1 = QStringLiteral("Aftertreatment DPF/SCR Sensors");
            out.decodedLine2 = QStringLiteral("Catalyst Temp: %1 °C  •  Status: Active")
                                   .arg(static_cast<int>(d[0]) * 2 - 40);
        } else if (pgn == 61441) {
            out.messageName =
                QStringLiteral("J1939 EBC1 (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            out.decodedLine1 = QStringLiteral("Electronic Brake Controller 1");
            out.decodedLine2 = QStringLiteral("ASR/ABS Status: Active  •  Brake State");
        } else if (pgn == 65269) {
            out.messageName =
                QStringLiteral("J1939 OEL (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            out.decodedLine1 = QStringLiteral("Cab / Dash Switch Status");
            out.decodedLine2 = QStringLiteral("Lamps, Turn Signals, Hazards, Horn");
        } else if (pgn == 65217 && out.frame.length >= 4) {
            out.messageName =
                QStringLiteral("J1939 VDHR (SA 0x%1)").arg(sa, 2, 16, QLatin1Char{'0'}).toUpper();
            const std::uint32_t rawDist = static_cast<std::uint32_t>(d[0])
                                          | (static_cast<std::uint32_t>(d[1]) << 8U)
                                          | (static_cast<std::uint32_t>(d[2]) << 16U)
                                          | (static_cast<std::uint32_t>(d[3]) << 24U);
            out.decodedLine1 =
                QStringLiteral("High-Res Total Distance: %1 km").arg(rawDist * 0.005, 0, 'f', 1);
            out.decodedLine2 = QStringLiteral("Instrument Cluster (SA 0x17)");
        } else {
            out.messageName = QStringLiteral("J1939 PGN %1 (SA 0x%2)")
                                  .arg(pgn)
                                  .arg(sa, 2, 16, QLatin1Char{'0'})
                                  .toUpper();
            out.decodedLine1 = QStringLiteral("PGN: 0x%1 (%2)")
                                   .arg(pgn, 4, 16, QLatin1Char{'0'})
                                   .toUpper()
                                   .arg(pgn);
            out.decodedLine2 = QStringLiteral("Source Addr: 0x%1  •  Pri: %2")
                                   .arg(sa, 2, 16, QLatin1Char{'0'})
                                   .toUpper()
                                   .arg((id >> 26U) & 0x7U);
        }
        return;
    }

    if (id >= 0x7E0 && id <= 0x7EF) {
        out.messageName = (id & 0x8U) != 0 ? QStringLiteral("UDS_DiagResponse")
                                           : QStringLiteral("UDS_DiagRequest");
        const std::uint8_t pci = d[0] & 0xF0U;
        const std::uint8_t sid = d[1];
        out.decodedLine1 = QStringLiteral("ISO-TP PCI: 0x%1  •  SID: 0x%2")
                               .arg(pci >> 4U, 1, 16)
                               .arg(sid, 2, 16, QLatin1Char{'0'})
                               .toUpper();
        out.decodedLine2 = QStringLiteral("DID: 0x%1%2")
                               .arg(d[2], 2, 16, QLatin1Char{'0'})
                               .arg(d[3], 2, 16, QLatin1Char{'0'})
                               .toUpper();
        return;
    }

    // 4. Raw CAN Frame breakdown when no DBC or protocol definition matches
    out.messageName = QStringLiteral("CAN_Frame_0x%1").arg(id, 3, 16, QLatin1Char{'0'}).toUpper();
    const std::uint16_t word0 =
        static_cast<std::uint16_t>(d[0]) | (static_cast<std::uint16_t>(d[1]) << 8U);
    const std::uint16_t word1 =
        static_cast<std::uint16_t>(d[2]) | (static_cast<std::uint16_t>(d[3]) << 8U);
    QString ascii;
    for (std::size_t i = 0; i < out.frame.length; ++i) {
        const char c = static_cast<char>(d[i]);
        ascii += (c >= 32 && c <= 126) ? QLatin1Char{c} : QLatin1Char{'.'};
    }
    out.decodedLine1 = QStringLiteral("Word0: 0x%1 (%2)  •  Word1: 0x%3")
                           .arg(word0, 4, 16, QLatin1Char{'0'})
                           .arg(word0)
                           .arg(word1, 4, 16, QLatin1Char{'0'})
                           .toUpper();
    out.decodedLine2 = QStringLiteral("ASCII: \"%1\"  •  DLC: %2 B  •  B0: %3")
                           .arg(ascii)
                           .arg(out.frame.length)
                           .arg(d[0], 2, 16, QLatin1Char{'0'})
                           .toUpper();
}

CanvasPanel::NodeFrameTelemetry CanvasPanel::computeNodeTelemetry(const NodeDescription& node,
                                                                  const NodeStatus* status,
                                                                  bool faultInjected) const
{
    NodeFrameTelemetry result;

    // 1. Read the latest frame & precomputed changed-byte mask from TraceStore::identifiers().
    if (m_simulationRunning && m_traceStore != nullptr) {
        const auto& idStats = m_traceStore->identifiers();
        std::uint64_t newestTimestampNs = 0;

        for (const TraceIdentifierStats& stats : idStats) {
            if (stats.count == 0 || !frameMatchesNode(stats.lastFrame, node, &m_description)) {
                continue;
            }
            if (!result.hasLiveFrame || stats.lastFrame.timestampNs >= newestTimestampNs) {
                newestTimestampNs = stats.lastFrame.timestampNs;
                result.hasLiveFrame = true;
                result.frame = stats.lastFrame;
                result.cycleUs = stats.lastCycleUs;
                result.changedMask = stats.changedBytes;
            }
        }
    }

    // 2. For `tinyml.ecu`, synthesize live 0x105 telemetry directly from node counters
    //    if not yet captured or to reflect fault injection immediately.
    if (node.typeName == "tinyml.ecu") {
        const quint64 inferences = counterValue(status, QStringLiteral("Inferences run"));
        if (m_simulationRunning && (inferences > 0 || faultInjected)) {
            quint64 regime = counterValue(status, QStringLiteral("Regime class"));
            quint64 conf = counterValue(status, QStringLiteral("Confidence (%)"));
            quint64 anom = counterValue(status, QStringLiteral("Anomaly score (%)"));
            quint64 health = counterValue(status, QStringLiteral("Thermal health (%)"));
            const quint64 infUs = counterValue(status, QStringLiteral("Last inference (us)"));

            if (faultInjected) {
                regime = 4;
                anom = std::max<quint64>(anom, 89);
                health = std::min<quint64>(health > 0 ? health : 28, 28);
            }

            const bool isJ1939 = node.parameters.boolean("j1939", false);
            const auto defaultId = isJ1939 ? 0x18FD0555 : 0x105;

            result.hasLiveFrame = true;
            result.frame.channel = static_cast<std::uint8_t>(
                std::max<std::int64_t>(0, node.parameters.integer("channel", 0)));
            result.frame.identifier = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, node.parameters.integer("outputCanId", defaultId)));
            result.frame.format = isJ1939 ? CanFrameFormat::Extended : CanFrameFormat::Standard;
            result.frame.dlc = 8;
            result.frame.length = 8;
            result.frame.data[0] = static_cast<std::uint8_t>(regime & 0xFFU);
            result.frame.data[1] = static_cast<std::uint8_t>(std::min<quint64>(100, conf));
            result.frame.data[2] = static_cast<std::uint8_t>(std::min<quint64>(100, anom));
            result.frame.data[3] = static_cast<std::uint8_t>(std::min<quint64>(100, health));
            result.frame.data[4] = static_cast<std::uint8_t>((infUs >> 8U) & 0xFFU);
            result.frame.data[5] = static_cast<std::uint8_t>(infUs & 0xFFU);
            result.frame.data[6] = static_cast<std::uint8_t>((inferences >> 8U) & 0xFFU);
            result.frame.data[7] = static_cast<std::uint8_t>(inferences & 0xFFU);
            result.changedMask = 0xDFULL; // Highlight active telemetry bytes
        }
    }

    // 3. When stopped or waiting for first bus frame, construct a preview frame
    //    so the 8-byte grid and DBC dictionary panel are informative at all times.
    if (!result.hasLiveFrame) {
        result.isPreview = true;
        result.frame.channel = static_cast<std::uint8_t>(
            std::max<std::int64_t>(0, node.parameters.integer("channel", 0)));
        result.frame.format = CanFrameFormat::Standard;
        result.frame.dlc = 8;
        result.frame.length = 8;

        if (node.typeName == "tinyml.ecu") {
            const bool isJ1939 = node.parameters.boolean("j1939", false);
            if (isJ1939) {
                result.frame.identifier = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(0, node.parameters.integer("outputCanId", 0x18FD0555)));
                result.frame.format = CanFrameFormat::Extended;
                result.frame.data = {1, 98, 4, 96, 0, 2, 0, 0};
            } else {
                result.frame.identifier = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(0, node.parameters.integer("outputCanId", 0x105)));
                result.frame.data = {1, 98, 4, 96, 0, 2, 0, 0};
            }
        } else if (node.typeName == "lua.ecu") {
            const std::int64_t sa = node.parameters.integer("sa", -1);
            if (sa >= 0) {
                result.frame.format = CanFrameFormat::Extended;
                if (sa == 0x00 || node.id == "ecu_engine") {
                    result.frame.identifier = 0x0CF00400U; // EEC1
                    result.frame.data = {0xF0, 0x7D, 0x82, 0x40, 0x38, 0xFF, 0xFF, 0xFF};
                } else if (sa == 0x3D || node.id == "ecu_aftertreatment") {
                    result.frame.identifier = 0x18FE1E3DU; // AT1T1I1
                    result.frame.data = {0x32, 0x48, 0x50, 0x60, 0x20, 0x00, 0x00, 0x00};
                } else if (sa == 0x21 || node.id == "ecu_body") {
                    result.frame.identifier = 0x18FEFC21U; // DD1
                    result.frame.data = {0x4E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                } else if (sa == 0x0B || node.id == "ecu_brakes") {
                    result.frame.identifier = 0x18F0010BU; // EBC1
                    result.frame.data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                } else if (sa == 0x37 || node.id == "ecu_switches") {
                    result.frame.identifier = 0x18FED537U; // OEL
                    result.frame.data = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                } else if (sa == 0x17 || node.id == "ecu_cluster_core") {
                    result.frame.identifier = 0x18FEC117U; // VDHR
                    result.frame.data = {0x20, 0x4E, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00};
                } else {
                    result.frame.identifier =
                        0x18FEF100U | (static_cast<std::uint32_t>(sa) & 0xFFU);
                    result.frame.data = {0x00, 0x44, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00};
                }
            } else {
                result.frame.identifier = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(0, node.parameters.integer("speed_id", 0x101)));
                result.frame.data = {0xE8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 74.4 km/h
            }
        } else if (node.typeName == "can.filter") {
            const std::int64_t pgn = node.parameters.integer("pgn", -1);
            if (pgn >= 0) {
                result.frame.format = CanFrameFormat::Extended;
                result.frame.identifier = 0x18000000U | (static_cast<std::uint32_t>(pgn) << 8U);
                result.frame.data = {0x00, 0x44, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00};
            } else {
                result.frame.identifier = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(0, node.parameters.integer("from", 0x101)));
                result.frame.data = {0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 40.0 km/h
            }
        } else if (node.typeName == "can.transmit") {
            bool foundSource = false;
            for (const EdgeDescription& edge : m_description.edges()) {
                if (edge.toNode == node.id) {
                    if (const NodeDescription* sourceNode = m_description.find(edge.fromNode)) {
                        const NodeFrameTelemetry srcTelem =
                            computeNodeTelemetry(*sourceNode, nullptr, false);
                        result.frame = srcTelem.frame;
                        foundSource = true;
                        break;
                    }
                }
            }
            if (!foundSource) {
                result.frame.identifier = 0x101U;
                result.frame.data = {0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            }
        } else if (node.typeName == "j1939.decoder") {
            result.frame.identifier = 0x0CF00400U;
            result.frame.format = CanFrameFormat::Extended;
            result.frame.data = {0x00, 0x96, 0x96, 0x20, 0x1C, 0x00, 0x00, 0xFF};
        } else if (node.typeName == "uds.client" || node.typeName == "isotp.transport") {
            result.frame.identifier = 0x7E0U;
            result.frame.data = {0x03, 0x22, 0xF1, 0x90, 0x55, 0x55, 0x55, 0x55};
        } else {
            // Check if a loaded DBC database has a message we can preview.
            bool foundDbcMsg = false;
            for (const std::shared_ptr<const CanDatabase>& db : m_databases) {
                if (db && !db->messages().empty()) {
                    const CanMessage& firstMsg = db->messages().front();
                    result.frame = firstMsg.makeFrame();
                    foundDbcMsg = true;
                    break;
                }
            }
            if (!foundDbcMsg) {
                result.frame.identifier = 0x101U;
                result.frame.data = {0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 60.0 km/h
            }
        }
    }

    if (!m_simulationRunning) {
        result.directionTag = QStringLiteral("STANDBY");
    } else if (!result.hasLiveFrame) {
        result.directionTag = QStringLiteral("WAIT RX");
    } else if (node.typeName == "can.transmit" || node.typeName == "lua.ecu"
               || node.typeName == "tinyml.ecu" || node.typeName == "transmit.list"
               || node.typeName == "sim.restbus") {
        result.directionTag = QStringLiteral("LIVE TX");
    } else if (node.typeName == "can.filter") {
        result.directionTag = QStringLiteral("PASS");
    } else {
        result.directionTag = QStringLiteral("LIVE RX");
    }

    const QString idHex = result.frame.isExtended()
                              ? QStringLiteral("ID 0x%1X")
                                    .arg(result.frame.identifier, 8, 16, QLatin1Char{'0'})
                                    .toUpper()
                              : QStringLiteral("ID 0x%1")
                                    .arg(result.frame.identifier, 3, 16, QLatin1Char{'0'})
                                    .toUpper();
    result.idDlcText = QStringLiteral("%1 • DLC %2").arg(idHex).arg(result.frame.length);

    if (result.cycleUs > 0) {
        result.cycleText = QStringLiteral("Δt %1 ms").arg(result.cycleUs / 1000.0, 0, 'f', 1);
    }

    for (std::size_t b = 0; b < 8; ++b) {
        if (b < result.frame.length) {
            result.hexBytes[b] =
                QStringLiteral("%1").arg(result.frame.data[b], 2, 16, QLatin1Char{'0'}).toUpper();
        } else {
            result.hexBytes[b] = QStringLiteral("--");
        }
    }

    decodeFromDatabasesOrBuiltin(node, result);
    return result;
}

void CanvasPanel::refreshTelemetryCache()
{
    const bool faultInjected =
        m_injectFaultButton != nullptr && m_injectFaultButton->isChecked() && m_simulationRunning;

    m_telemetryCache.clear();
    m_telemetryCache.reserve(static_cast<qsizetype>(m_description.nodes().size()));

    for (const NodeDescription& node : m_description.nodes()) {
        const QString idString = QString::fromStdString(node.id);
        const auto statusIt = m_nodeStatuses.constFind(idString);
        const NodeStatus* status =
            (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;

        m_telemetryCache.insert(idString, computeNodeTelemetry(node, status, faultInjected));
    }
}

CanvasPanel::CanvasPanel(GraphDescription& description, const NodeCatalog& catalog, QWidget* parent)
    : QWidget{parent}
    , m_description{description}
    , m_catalog{catalog}
    , m_theme{Theme::dark()}
    , m_monoBoldFont{makeMonospaceFont(10, QFont::Bold)}
    , m_monoSmallFont{makeMonospaceFont(8, QFont::Medium)}
    , m_monoSignalFont{makeMonospaceFont(9, QFont::DemiBold)}
{
    m_badgeFont = font();
    m_badgeFont.setPixelSize(9);
    m_badgeFont.setWeight(QFont::Bold);

    m_titleFont = font();
    m_titleFont.setPixelSize(11);
    m_titleFont.setWeight(QFont::Bold);

    m_subFont = font();
    m_subFont.setPixelSize(9);
    m_subFont.setWeight(QFont::Medium);

    m_dbcTitleFont = font();
    m_dbcTitleFont.setPixelSize(8);
    m_dbcTitleFont.setWeight(QFont::Bold);

    m_model = std::make_unique<PipelineGraphModel>(m_description, m_catalog);

    if (ThemeManager* themes = ThemeManager::instance()) {
        m_theme = themes->theme();
    }

    // Not only with a theme manager: the size QtNodes draws a wire's area to is part of this
    // canvas's painting, and a canvas built without one is drawn with the same room.
    applyStyles(m_theme);

    auto* scene = new CanvasScene{
        *m_model,
        [this](QPointF scenePosition) { return buildSceneMenu(scenePosition); },
        [this](QPainter* painter, const QRectF& rect) { paintSceneForeground(painter, rect); },
        this};

    m_scene = scene;

    // Ports along the top and the bottom of a block, wires that drop to a bus, and a bus that is a
    // line: none of it is what QtNodes draws by default. Vertical first - that is what makes a wire
    // leave a block downwards - and then the geometry that overrides what vertical means.
    m_scene->setOrientation(Qt::Vertical);

    auto geometry = std::make_unique<PipelineNodeGeometry>(*m_model);
    geometry->setScene(m_scene);
    m_scene->setNodeGeometry(std::move(geometry));
    m_scene->setNodePainter(std::make_unique<PipelineNodePainter>(*this));
    m_scene->setConnectionPainter(std::make_unique<PipelineConnectionPainter>(*this));

    // Whatever the model already held was drawn before any of that was set.
    m_scene->onModelReset();

    auto* canvasView = new CanvasView{m_scene};
    canvasView->onShown([this] { fitIfPending(); });

    m_view = canvasView;
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                           | QPainter::SmoothPixmapTransform);
    m_view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    m_view->setScaleRange(kMinZoom, kMaxZoom);
    m_view->setAcceptDrops(true);
    m_view->viewport()->setAcceptDrops(true);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);

    connect(m_scene,
            &QtNodes::BasicGraphicsScene::nodeContextMenu,
            this,
            [this](QtNodes::NodeId nodeId, QPointF position) { showNodeMenu(nodeId, position); });

    m_palette = new QTreeWidget;
    m_palette->setHeaderHidden(true);
    m_palette->setRootIsDecorated(true);
    m_palette->setSelectionMode(QAbstractItemView::SingleSelection);
    m_palette->setDragEnabled(true);
    m_palette->setDragDropMode(QAbstractItemView::DragOnly);
    m_palette->setMinimumWidth(160);
    m_palette->setMaximumWidth(260);
    m_palette->setIconSize(QSize(16, 16));
    m_palette->setToolTip(
        tr("Double-click or drag a block onto the Pipeline canvas, or select one and click "
           "'Add Block'."));

    buildPalette();

    // --- Pipeline control bar above the Canvas (left-aligned) ---
    // Left-aligned, so it starts where the strip of view tools below it does, and grouped by what
    // a button acts on with a thin rule between the groups: what is in the Pipeline (add, delete),
    // how it is shown (animate) and the TinyML demo - set up first, then break it, the order it is
    // used in.
    m_hudBar = new QWidget{this};
    m_hudBar->setObjectName(QStringLiteral("torquebus.canvas.hud"));
    m_hudBar->setProperty("hudBar", true);
    auto* hudLayout = new QHBoxLayout{m_hudBar};
    hudLayout->setContentsMargins(10, 5, 10, 5);
    hudLayout->setSpacing(6);

    m_addBlockButton = new QPushButton{tr("Add Block"), m_hudBar};
    m_addBlockButton->setToolTip(
        tr("Add the block selected in the left palette (or choose from a menu) at a free "
           "position on the Canvas."));

    m_deleteBlockButton = new QPushButton{tr("Delete Selected"), m_hudBar};
    m_deleteBlockButton->setEnabled(false);
    m_deleteBlockButton->setToolTip(
        tr("Delete the selected block(s) or connection wire(s) from the Pipeline (Del / "
           "Backspace). Select something on the Canvas first."));

    m_animateButton = new QPushButton{tr("Animate Flow"), m_hudBar};
    m_animateButton->setCheckable(true);
    m_animateButton->setChecked(true);
    m_animateButton->setToolTip(tr("Toggle live wire packet flow, node telemetry halos, and TinyML "
                                   "neural network animations on the Canvas."));

    m_injectFaultButton = new QPushButton{tr("Inject TinyML Anomaly"), m_hudBar};
    m_injectFaultButton->setCheckable(true);
    m_injectFaultButton->setToolTip(
        tr("Inject simulated powertrain thermal runaway and CAN timing jitter into the "
           "TinyML Virtual ECU (`tinyml.inject_fault`) to observe real-time anomaly detection."));

    m_arrangeButton = new QPushButton{tr("Arrange"), m_hudBar};
    m_arrangeButton->setToolTip(
        tr("Tidy the Pipeline: each bus a line across the top of what is wired to it, the modules "
           "that speak on it above, the ones that only listen below, and what they feed under "
           "them."));

    m_detailsButton = new QPushButton{tr("Details"), m_hudBar};
    m_detailsButton->setCheckable(true);
    m_detailsButton->setToolTip(
        tr("Open every block into a card that shows the frame it last carried, byte by byte, and "
           "what the database says it means. Close them again for the compact view."));

    m_demoPipelineButton = new QPushButton{tr("TinyML Demo Setup"), m_hudBar};
    m_demoPipelineButton->setToolTip(
        tr("Populate the Canvas with a complete Virtual Vehicle ECU + TinyML Virtual ECU + "
           "Signal Plot + CAN Trace simulation pipeline."));

    const auto addGroupRule = [this, hudLayout] {
        auto* rule = new QFrame{m_hudBar};
        rule->setFrameShape(QFrame::NoFrame);
        rule->setProperty("hudRule", true);
        rule->setFixedSize(1, 18);
        hudLayout->addWidget(rule);
    };

    hudLayout->addWidget(m_addBlockButton);
    hudLayout->addWidget(m_deleteBlockButton);
    addGroupRule();
    hudLayout->addWidget(m_arrangeButton);
    hudLayout->addWidget(m_detailsButton);
    hudLayout->addWidget(m_animateButton);
    addGroupRule();
    hudLayout->addWidget(m_demoPipelineButton);
    hudLayout->addWidget(m_injectFaultButton);
    hudLayout->addStretch(1);

    // What each button says, kept for when the bar has to show icons alone.
    for (QPushButton* button : m_hudBar->findChildren<QPushButton*>()) {
        button->setProperty("hudLabel", button->text());
        button->setProperty("hudTip", button->toolTip());
    }
    m_hudBar->installEventFilter(this);

    applyHudIcons();

    auto* rightPane = new QWidget{this};
    auto* rightLayout = new QVBoxLayout{rightPane};
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_hudBar);

    // The canvas, with the buttons that move it down its left edge. Not in the bar above: that one
    // already needs most of the width a Pipeline panel has by default, and a toolbar that is
    // squeezed folds its buttons away behind a menu.
    buildNavigationToolBar();

    auto* canvasRow = new QHBoxLayout;
    canvasRow->setContentsMargins(0, 0, 0, 0);
    canvasRow->setSpacing(0);
    canvasRow->addWidget(m_navToolBar);
    canvasRow->addWidget(m_view, 1);
    rightLayout->addLayout(canvasRow, 1);

    auto* paletteContainer = new QWidget{this};
    paletteContainer->setMinimumWidth(160);
    paletteContainer->setMaximumWidth(260);
    auto* paletteLayout = new QVBoxLayout{paletteContainer};
    paletteLayout->setContentsMargins(4, 4, 4, 4);
    paletteLayout->setSpacing(4);

    m_paletteFilter = new QLineEdit{paletteContainer};
    m_paletteFilter->setPlaceholderText(tr("Search blocks..."));
    m_paletteFilter->setClearButtonEnabled(true);
    connect(m_paletteFilter, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString query = text.trimmed();
        for (int i = 0; i < m_palette->topLevelItemCount(); ++i) {
            QTreeWidgetItem* group = m_palette->topLevelItem(i);
            int visibleChildren = 0;
            for (int j = 0; j < group->childCount(); ++j) {
                QTreeWidgetItem* child = group->child(j);
                const bool match = query.isEmpty()
                                   || child->text(0).contains(query, Qt::CaseInsensitive)
                                   || child->toolTip(0).contains(query, Qt::CaseInsensitive);
                child->setHidden(!match);
                if (match) {
                    ++visibleChildren;
                }
            }
            group->setHidden(!query.isEmpty() && visibleChildren == 0);
            if (!query.isEmpty() && visibleChildren > 0) {
                group->setExpanded(true);
            }
        }
    });

    paletteLayout->addWidget(m_paletteFilter);
    paletteLayout->addWidget(m_palette, 1);

    m_splitter = new QSplitter{Qt::Horizontal, this};
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.canvas"));
    m_splitter->addWidget(paletteContainer);
    m_splitter->addWidget(rightPane);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_splitter);

    m_flowAnimation = new QVariantAnimation{this};
    m_flowAnimation->setStartValue(0.0);
    m_flowAnimation->setEndValue(1.0);
    m_flowAnimation->setDuration(kFlowCycleDurationMs);
    m_flowAnimation->setEasingCurve(QEasingCurve::Linear);
    m_flowAnimation->setLoopCount(-1);

    connect(m_flowAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        const qreal nextPhase = value.toReal();
        if (std::abs(nextPhase - m_phase) < 0.040 && nextPhase >= m_phase) {
            return;
        }
        m_phase = nextPhase;
        if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(m_addBlockButton, &QPushButton::clicked, this, &CanvasPanel::addSelectedPaletteNode);
    connect(m_deleteBlockButton, &QPushButton::clicked, this, &CanvasPanel::deleteSelectedItems);
    connect(m_arrangeButton, &QPushButton::clicked, this, &CanvasPanel::arrangeBlocks);
    connect(m_detailsButton, &QPushButton::toggled, this, &CanvasPanel::setDetailsShown);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &CanvasPanel::updateEditButtons);

    connect(m_animateButton, &QPushButton::toggled, this, [this](bool checked) {
        m_animationsEnabled = checked;
        updateAnimationState();
        if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(m_injectFaultButton, &QPushButton::toggled, this, [this](bool checked) {
        if (m_variables != nullptr) {
            m_variables->set("tinyml.inject_fault", checked ? 1.0 : 0.0);
        }
        refreshTelemetryCache();
        updateHudLabels();
        if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(
        m_demoPipelineButton, &QPushButton::clicked, this, [this] { buildTinyMlDemoPipeline(); });

    connect(m_palette, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        addNodeFromPalette(item);
    });

    connect(m_model.get(), &PipelineGraphModel::nodeCreated, this, [this](QtNodes::NodeId) {
        refreshTelemetryCache();
        updateHudLabels();
        Q_EMIT graphEdited();
    });
    connect(m_model.get(), &PipelineGraphModel::nodeDeleted, this, [this](QtNodes::NodeId) {
        refreshTelemetryCache();
        updateHudLabels();
        Q_EMIT graphEdited();
    });
    connect(
        m_model.get(), &PipelineGraphModel::connectionCreated, this, [this](QtNodes::ConnectionId) {
            updateHudLabels();
            Q_EMIT graphEdited();
        });
    connect(
        m_model.get(), &PipelineGraphModel::connectionDeleted, this, [this](QtNodes::ConnectionId) {
            updateHudLabels();
            Q_EMIT graphEdited();
        });

    connect(
        m_scene, &QtNodes::BasicGraphicsScene::nodeSelected, this, [this](QtNodes::NodeId nodeId) {
            Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(nodeId)));
        });

    refreshTelemetryCache();

    if (ThemeManager* themes = ThemeManager::instance()) {
        applyTheme(themes->theme());
        connect(themes, &ThemeManager::themeChanged, this, &CanvasPanel::applyTheme);
    } else {
        updateHudLabels();
    }
}

CanvasPanel::~CanvasPanel()
{
    releaseGraph();
}

void CanvasPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateHudCompactness();
    refreshTelemetryCache();
    updateAnimationState();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

void CanvasPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateAnimationState();
}

void CanvasPanel::setSystemVariables(SystemVariables* variables)
{
    m_variables = variables;
    if (m_variables != nullptr && m_injectFaultButton != nullptr
        && m_injectFaultButton->isChecked()) {
        m_variables->set("tinyml.inject_fault", 1.0);
    }
}

void CanvasPanel::setTraceStore(const TraceStore* store)
{
    m_traceStore = store;
    refreshTelemetryCache();
}

void CanvasPanel::setPlotStore(const SignalSeriesStore* store)
{
    m_plotStore = store;
    refreshTelemetryCache();
}

void CanvasPanel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_databases = std::move(databases);
    refreshTelemetryCache();
    invalidateItems(); // A card says what the database makes of the frame.
    if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

CanvasPanel::ChipInfo CanvasPanel::chipInfo(const NodeDescription& node) const
{
    ChipInfo info;
    info.line = nodeSubtitle(node);

    if (!m_simulationRunning || !node.enabled) {
        return info;
    }

    const QString idString = QString::fromStdString(node.id);
    const auto statusIt = m_nodeStatuses.constFind(idString);
    const NodeStatus* status = (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;

    const bool faultInjected = m_injectFaultButton != nullptr && m_injectFaultButton->isChecked();

    if (node.typeName == "tinyml.ecu") {
        const quint64 inferences = counterValue(status, QStringLiteral("Inferences run"));
        const quint64 anomalyPct = counterValue(status, QStringLiteral("Anomaly score (%)"));
        const quint64 regimeCode = counterValue(status, QStringLiteral("Regime class"));

        info.alert = (anomalyPct >= 65) || (regimeCode == 4) || faultInjected;
        info.live = true;
        info.line = QStringLiteral("%1 • %2 inf • %3%")
                        .arg(regimeName(faultInjected ? 4 : regimeCode))
                        .arg(inferences)
                        .arg(faultInjected ? std::max<quint64>(anomalyPct, 89) : anomalyPct);
        return info;
    }

    if (status != nullptr
        && counterValue(status, QStringLiteral("Stopped after repeated errors")) > 0) {
        info.alert = true;
    }

    // The frame the block last carried: which way, which identifier, how often.
    const auto telemetry = m_telemetryCache.constFind(idString);
    if (telemetry != m_telemetryCache.cend() && telemetry->hasLiveFrame && !telemetry->isPreview) {
        QString direction = telemetry->directionTag;
        if (direction == QLatin1String("LIVE TX")) {
            direction = QStringLiteral("TX");
        } else if (direction == QLatin1String("LIVE RX")) {
            direction = QStringLiteral("RX");
        }

        const quint32 identifier = telemetry->frame.identifier;
        QString line = QStringLiteral("%1 %2")
                           .arg(direction,
                                telemetry->frame.isExtended()
                                    ? QStringLiteral("%1").arg(identifier, 8, 16, QLatin1Char{'0'})
                                    : QStringLiteral("%1").arg(identifier, 3, 16, QLatin1Char{'0'}))
                           .toUpper();

        if (telemetry->cycleUs > 0) {
            line +=
                QStringLiteral(" • %1 ms")
                    .arg(telemetry->cycleUs / 1000.0, 0, 'f', telemetry->cycleUs >= 100000 ? 0 : 1);
        }

        info.line = line;
        info.live = true;
        return info;
    }

    // Nothing carried yet: what the block has counted, if it counts.
    if (status != nullptr && !status->counters.isEmpty()) {
        const NodeCounter& primary = status->counters.front();
        info.line = QStringLiteral("%1: %2").arg(primary.label).arg(primary.value);
        info.live = true;
    }

    return info;
}

void CanvasPanel::invalidateItems()
{
    if (m_scene == nullptr) {
        return;
    }

    m_chipLines.clear();

    for (QGraphicsItem* item : m_scene->items()) {
        if (qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item) != nullptr
            || qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item) != nullptr) {
            item->update();
        }
    }
}

void CanvasPanel::refreshChips()
{
    if (!m_model || m_scene == nullptr) {
        return;
    }

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QString id = QString::fromStdString(node.id);

        // A card shows the bytes of the frame, which change whenever a frame does.
        if (m_model->isExpanded(nodeId)) {
            ngo->update();
            continue;
        }

        QString signature;
        if (m_model->isBus(nodeId)) {
            // The plate of a bus: what its interface is doing.
            const std::int64_t channel = m_model->busChannel(nodeId);
            if (channel >= 0 && channel < m_channelStatuses.size()) {
                const ChannelStatus& status = m_channelStatuses.at(static_cast<qsizetype>(channel));
                signature = QStringLiteral("%1|%2|%3|%4|%5")
                                .arg(status.deviceName)
                                .arg(status.bitrate)
                                .arg(static_cast<int>(status.framesPerSecond))
                                .arg(static_cast<int>(status.busLoadPercent))
                                .arg(m_simulationRunning);
            }
        } else {
            const ChipInfo info = chipInfo(node);
            signature = info.line + (info.alert ? QLatin1String("!") : QLatin1String(""))
                        + (info.live ? QLatin1String("*") : QLatin1String(""));
        }

        const auto known = m_chipLines.constFind(id);
        if (known != m_chipLines.cend() && known.value() == signature) {
            continue;
        }

        m_chipLines.insert(id, signature);
        ngo->update();
    }
}

QString CanvasPanel::findScriptNodeAt(const QPointF& scenePosition, bool onlyEditButtons) const
{
    if (!m_model || m_scene == nullptr) {
        return {};
    }

    const QFontMetricsF badgeMetrics{m_badgeFont};

    for (const NodeDescription& node : m_description.nodes()) {
        if (!isScriptNodeType(node.typeName)) {
            continue;
        }

        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QSize nodeSize = m_scene->nodeGeometry().size(nodeId);
        const QRectF nodeRect = ngo->mapRectToScene(QRectF{QPointF{0.0, 0.0}, QSizeF{nodeSize}});

        if (onlyEditButtons) {
            if (m_model->isExpanded(nodeId)) {
                // The card has two: in its header, and in the box that says what the block is.
                const QString typeTag = shortTypeBadge(node.typeName);
                const qreal tagWidth = badgeMetrics.horizontalAdvance(typeTag) + 12.0;
                const QRectF tagRect{
                    nodeRect.right() - tagWidth - 28.0, nodeRect.top() + 4.5, tagWidth, 15.0};
                const QRectF headerEditRect{
                    tagRect.left() - 72.0, nodeRect.top() + 4.0, 66.0, 16.0};

                const QRectF dbcRect{nodeRect.left() + 10.0,
                                     nodeRect.top() + layout::kCardDecodeTop,
                                     nodeRect.width() - 20.0,
                                     layout::kCardDecodeHeight};
                const QRectF specsEditRect{dbcRect.right() - 76.0, dbcRect.top() + 2.5, 72.0, 16.0};

                if (headerEditRect.contains(scenePosition)
                    || specsEditRect.contains(scenePosition)) {
                    return QString::fromStdString(node.id);
                }
            } else if (layout::chipScriptButton(nodeRect)
                           .adjusted(-2.0, -2.0, 2.0, 2.0)
                           .contains(scenePosition)) {
                return QString::fromStdString(node.id);
            }
        } else {
            if (nodeRect.contains(scenePosition)) {
                return QString::fromStdString(node.id);
            }
        }
    }

    return {};
}

QString CanvasPanel::findExpandButtonAt(const QPointF& scenePosition) const
{
    if (!m_model || m_scene == nullptr) {
        return {};
    }

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId || m_model->isBus(nodeId)) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QRectF nodeRect = ngo->mapRectToScene(
            QRectF{QPointF{0.0, 0.0}, QSizeF{m_scene->nodeGeometry().size(nodeId)}});

        const QRectF button = m_model->isExpanded(nodeId) ? layout::cardCollapseButton(nodeRect)
                                                          : layout::chipExpandButton(nodeRect);

        // A chip's chevron is there to be clicked only while it is drawn, which is
        // while the chip is under the pointer or selected.
        if (button.adjusted(-2.0, -2.0, 2.0, 2.0).contains(scenePosition)
            && (m_model->isExpanded(nodeId) || ngo->isSelected() || ngo->nodeState().hovered())) {
            return QString::fromStdString(node.id);
        }
    }

    return {};
}

QtNodes::ConnectionId CanvasPanel::findFrameFormatPillAt(const QPointF& scenePosition,
                                                         bool& found) const
{
    found = false;
    if (!m_model || m_scene == nullptr) {
        return {};
    }

    for (const EdgeDescription& edge : m_description.edges()) {
        const QtNodes::ConnectionId id{m_model->canvasId(edge.fromNode),
                                       static_cast<QtNodes::PortIndex>(edge.fromPort),
                                       m_model->canvasId(edge.toNode),
                                       static_cast<QtNodes::PortIndex>(edge.toPort)};
        if (id.outNodeId == QtNodes::InvalidNodeId || id.inNodeId == QtNodes::InvalidNodeId
            || !m_model->isBusTap(id)) {
            continue;
        }

        const QtNodes::ConnectionGraphicsObject* wire = m_scene->connectionGraphicsObject(id);
        if (wire == nullptr) {
            continue;
        }

        // The pill is drawn when the wire has been limited, and while it is the
        // one under the pointer or selected - only then is there anything to click.
        const bool shown = edge.frameFormat != CanFormatMatch::Any || wire->isSelected()
                           || wire->connectionState().hovered();

        if (shown
            && frameFormatPillRect(*wire, *m_model)
                   .adjusted(-2.0, -2.0, 2.0, 2.0)
                   .contains(scenePosition)) {
            found = true;
            return id;
        }
    }

    return {};
}

QtNodes::ConnectionId CanvasPanel::findBusWireAt(const QPointF& scenePosition, bool& found) const
{
    found = false;
    if (!m_model || m_scene == nullptr || m_view == nullptr) {
        return {};
    }

    const QList<QGraphicsItem*> hits = m_scene->items(
        scenePosition, Qt::IntersectsItemShape, Qt::DescendingOrder, m_view->transform());

    for (QGraphicsItem* item : hits) {
        if (const auto* wire = qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            if (m_model->isBusTap(wire->connectionId())) {
                found = true;
                return wire->connectionId();
            }
        }
    }

    return {};
}

bool CanvasPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event != nullptr && watched == m_hudBar && event->type() == QEvent::Resize) {
        updateHudCompactness();
    }

    if (event == nullptr || m_view == nullptr) {
        return QWidget::eventFilter(watched, event);
    }

    // With the Pan tool on, a drag anywhere moves the canvas and nothing on it answers the pointer
    // - the "edit script" pencil included: a click there is part of what the tool sets aside.
    if (m_panToolActive && watched == m_view->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::ToolTip:
        case QEvent::ContextMenu:
            return QWidget::eventFilter(watched, event);
        default:
            break;
        }
    }

    if (watched == m_view->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QPointF scenePos = m_view->mapToScene(mouseEvent->position().toPoint());
            const QString hitId = findScriptNodeAt(scenePos, true);
            bool pill = false;
            (void)findFrameFormatPillAt(scenePos, pill);
            const bool hovering =
                !hitId.isEmpty() || pill || !findExpandButtonAt(scenePos).isEmpty();

            if (hovering != m_hoveringScriptEdit || hitId != m_hoveredScriptNodeId) {
                const QString previous = m_hoveredScriptNodeId;
                m_hoveringScriptEdit = hovering;
                m_hoveredScriptNodeId = hitId;
                if (hovering) {
                    m_view->viewport()->setCursor(Qt::PointingHandCursor);
                } else {
                    m_view->viewport()->unsetCursor();
                }

                // The pencil lights while the pointer is on it, and that is part of the
                // block QtNodes has cached.
                for (const QString& id : {previous, hitId}) {
                    if (id.isEmpty() || !m_model) {
                        continue;
                    }
                    if (auto* ngo =
                            m_scene->nodeGraphicsObject(m_model->canvasId(id.toStdString()))) {
                        ngo->update();
                    }
                }
                m_view->viewport()->update();
            }
        } else if (event->type() == QEvent::Leave) {
            if (m_hoveringScriptEdit) {
                const QString previous = m_hoveredScriptNodeId;
                m_hoveringScriptEdit = false;
                m_hoveredScriptNodeId.clear();
                m_view->viewport()->unsetCursor();
                if (!previous.isEmpty() && m_model) {
                    if (auto* ngo = m_scene->nodeGraphicsObject(
                            m_model->canvasId(previous.toStdString()))) {
                        ngo->update();
                    }
                }
                m_view->viewport()->update();
            }
        } else if (event->type() == QEvent::ToolTip) {
            auto* helpEvent = static_cast<QHelpEvent*>(event);
            const QPointF scenePos = m_view->mapToScene(helpEvent->pos());
            const QString hitId = findScriptNodeAt(scenePos, true);
            if (!hitId.isEmpty()) {
                QToolTip::showText(
                    helpEvent->globalPos(),
                    tr("Click to open and edit Lua script for '%1' in the Script Editor")
                        .arg(hitId),
                    m_view->viewport());
                return true;
            }

            const QString expandId = findExpandButtonAt(scenePos);
            if (!expandId.isEmpty()) {
                const bool open = m_model->isExpanded(m_model->canvasId(expandId.toStdString()));
                QToolTip::showText(helpEvent->globalPos(),
                                   open ? tr("Close this card to a compact block")
                                        : tr("Open this block into a card: the frame it last "
                                             "carried, byte by byte"),
                                   m_view->viewport());
                return true;
            }

            bool pill = false;
            (void)findFrameFormatPillAt(scenePos, pill);
            if (pill) {
                QToolTip::showText(helpEvent->globalPos(),
                                   tr("Which frames this module hears from the bus. Click to "
                                      "change: all, 11-bit only, 29-bit only."),
                                   m_view->viewport());
                return true;
            }

            // A block, by what it is.
            if (m_model && m_scene != nullptr) {
                const QList<QGraphicsItem*> under = m_scene->items(
                    scenePos, Qt::IntersectsItemShape, Qt::DescendingOrder, m_view->transform());
                for (QGraphicsItem* item : under) {
                    const auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
                    if (ngo == nullptr) {
                        continue;
                    }

                    const NodeDescription* node = m_model->node(ngo->nodeId());
                    const NodeTypeInfo* info = m_model->info(ngo->nodeId());
                    if (node == nullptr || info == nullptr) {
                        break;
                    }

                    QStringList lines;
                    lines << QStringLiteral("<b>%1</b> — %2")
                                 .arg(QString::fromStdString(node->id).toHtmlEscaped(),
                                      QString::fromStdString(info->displayName).toHtmlEscaped());
                    if (!info->description.empty()) {
                        lines << QString::fromStdString(info->description).toHtmlEscaped();
                    }
                    if (m_model->isBus(ngo->nodeId())) {
                        // What the channel is, as the interface behind it says.
                        const std::int64_t channel = m_model->busChannel(ngo->nodeId());
                        if (channel >= 0 && channel < m_channelStatuses.size()) {
                            const ChannelStatus& status =
                                m_channelStatuses.at(static_cast<qsizetype>(channel));
                            QString interface =
                                status.deviceName.isEmpty()
                                    ? tr("CAN %1").arg(channel + 1)
                                    : tr("CAN %1 on %2").arg(channel + 1).arg(status.deviceName);
                            if (status.bitrate > 0) {
                                interface +=
                                    tr(" at %1 kbit/s").arg(status.bitrate / 1000.0, 0, 'g', 4);
                            }
                            lines << interface.toHtmlEscaped();
                        }
                        lines << tr("Drop a block's port on the rail to wire it to this bus. "
                                    "Right-click a drop to choose which frames - 11-bit, 29-bit - "
                                    "its module hears.");
                    }
                    if (!node->enabled) {
                        lines << tr("Switched off: it is kept in the project and skipped when the "
                                    "pipeline is built.");
                    }

                    QToolTip::showText(helpEvent->globalPos(),
                                       lines.join(QStringLiteral("<br>")),
                                       m_view->viewport());
                    return true;
                }
            }
        } else if (event->type() == QEvent::ContextMenu) {
            auto* contextEvent = static_cast<QContextMenuEvent*>(event);
            const QPointF scenePos = m_view->mapToScene(contextEvent->pos());

            // A wire to a bus has a menu of its own. A block over it is the block's.
            const QGraphicsItem* top = m_scene->itemAt(scenePos, m_view->transform());
            if (qgraphicsitem_cast<const QtNodes::NodeGraphicsObject*>(top) == nullptr) {
                bool found = false;
                const QtNodes::ConnectionId wire = findBusWireAt(scenePos, found);
                if (found) {
                    showWireMenu(wire, contextEvent->globalPos());
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                const QPointF scenePos = m_view->mapToScene(mouseEvent->position().toPoint());

                // The pill on a drop: which frames the module hears.
                bool pill = false;
                const QtNodes::ConnectionId wire = findFrameFormatPillAt(scenePos, pill);
                if (pill) {
                    m_swallowRelease = true;
                    cycleFrameFormat(wire);
                    return true;
                }

                // The chevron: open the block into a card, or close it.
                const QString expandId = findExpandButtonAt(scenePos);
                if (!expandId.isEmpty()) {
                    m_swallowRelease = true;
                    const QtNodes::NodeId nodeId = m_model->canvasId(expandId.toStdString());
                    setBlockExpanded(nodeId, !m_model->isExpanded(nodeId));
                    return true;
                }

                const QString hitId = findScriptNodeAt(scenePos, true);
                if (!hitId.isEmpty()) {
                    m_swallowRelease = true;
                    const QtNodes::NodeId nodeId = m_model->canvasId(hitId.toStdString());
                    if (nodeId != QtNodes::InvalidNodeId) {
                        if (auto* ngo = m_scene->nodeGraphicsObject(nodeId)) {
                            m_scene->clearSelection();
                            ngo->setSelected(true);
                        }
                    }
                    Q_EMIT nodeSelected(hitId);
                    Q_EMIT editScriptRequested(hitId);
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (m_swallowRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                m_swallowRelease = false;
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                const QPointF scenePos = m_view->mapToScene(mouseEvent->position().toPoint());
                const QString hitId = findScriptNodeAt(scenePos, false);
                if (!hitId.isEmpty()) {
                    const QtNodes::NodeId nodeId = m_model->canvasId(hitId.toStdString());
                    if (nodeId != QtNodes::InvalidNodeId) {
                        if (auto* ngo = m_scene->nodeGraphicsObject(nodeId)) {
                            m_scene->clearSelection();
                            ngo->setSelected(true);
                        }
                    }
                    Q_EMIT nodeSelected(hitId);
                    Q_EMIT editScriptRequested(hitId);
                    return true;
                }
            }
        } else if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDropEvent*>(event);
            if (dragEvent->source() == m_palette && m_palette != nullptr
                && m_palette->currentItem() != nullptr
                && !m_palette->currentItem()->data(0, kTypeNameRole).toString().isEmpty()) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            if (dropEvent->source() == m_palette && m_palette != nullptr
                && m_palette->currentItem() != nullptr) {
                const QString typeName =
                    m_palette->currentItem()->data(0, kTypeNameRole).toString();
                if (!typeName.isEmpty()) {
                    const QPointF dropAt = m_view->mapToScene(dropEvent->position().toPoint());
                    addNodeAtDrop(typeName, dropAt);
                    dropEvent->acceptProposedAction();
                    return true;
                }
            }
        }
    } else if (watched == m_view) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace) {
                deleteSelectedItems();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void CanvasPanel::deleteSelectedItems()
{
    if (!m_model || m_scene == nullptr) {
        return;
    }

    const QList<QGraphicsItem*> selected = m_scene->selectedItems();
    if (selected.isEmpty()) {
        return;
    }

    // 1. Delete selected connections first.
    std::vector<QtNodes::ConnectionId> connectionsToDelete;
    std::vector<QtNodes::NodeId> nodesToDelete;

    for (QGraphicsItem* item : selected) {
        if (auto* cgo = qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            connectionsToDelete.push_back(cgo->connectionId());
        } else if (auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            nodesToDelete.push_back(ngo->nodeId());
        }
    }

    for (const QtNodes::ConnectionId& cid : connectionsToDelete) {
        m_model->deleteConnection(cid);
    }

    for (const QtNodes::NodeId nodeId : nodesToDelete) {
        m_model->deleteNode(nodeId);
    }

    m_scene->clearSelection();

    refreshTelemetryCache();
    updateHudLabels();
    Q_EMIT nodeSelected(QString{});
    Q_EMIT graphEdited();
    if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

void CanvasPanel::deleteBlock(const QString& descriptionId)
{
    if (!m_model) {
        return;
    }

    const QtNodes::NodeId cid = m_model->canvasId(descriptionId.toStdString());
    if (cid != QtNodes::InvalidNodeId) {
        m_model->deleteNode(cid);
    } else {
        m_description.removeNode(descriptionId.toStdString());
    }

    if (m_scene != nullptr) {
        m_scene->clearSelection();
    }

    refreshTelemetryCache();
    updateHudLabels();
    Q_EMIT nodeSelected(QString{});
    Q_EMIT graphEdited();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

void CanvasPanel::setSimulationRunning(bool running)
{
    m_simulationRunning = running;
    if (running && m_variables != nullptr && m_injectFaultButton != nullptr) {
        m_variables->set("tinyml.inject_fault", m_injectFaultButton->isChecked() ? 1.0 : 0.0);
    }
    refreshTelemetryCache();
    invalidateItems(); // The lights on the blocks are on or off with the measurement.
    updateAnimationState();
    updateHudLabels();
    if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

void CanvasPanel::setNodeStatuses(const QList<NodeStatus>& nodes)
{
    if (!m_simulationRunning && nodes.isEmpty() && m_nodeStatuses.isEmpty()) {
        updateHudLabels();
        return;
    }

    m_nodeStatuses.clear();
    m_nodeStatuses.reserve(nodes.size());
    for (const NodeStatus& status : nodes) {
        m_nodeStatuses.insert(status.name, status);
    }
    updateHudLabels();
    if (isVisible()) {
        refreshTelemetryCache();
        refreshChips();
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    }
}

void CanvasPanel::setChannelStatuses(const QList<ChannelStatus>& channels)
{
    m_channelStatuses = channels;
    updateHudLabels();

    // The plate of a bus says what its interface is doing.
    if (isVisible()) {
        refreshChips();
    }
}

void CanvasPanel::updateAnimationState()
{
    if (m_flowAnimation == nullptr) {
        return;
    }

    const bool shouldRun = m_simulationRunning && m_animationsEnabled && isVisible();
    if (shouldRun) {
        if (m_flowAnimation->state() != QAbstractAnimation::Running) {
            m_flowAnimation->start();
        }
    } else {
        if (m_flowAnimation->state() == QAbstractAnimation::Running) {
            m_flowAnimation->stop();
        }
    }
}

void CanvasPanel::updateHudLabels()
{
    const bool faultActive = m_injectFaultButton != nullptr && m_injectFaultButton->isChecked();

    quint64 totalInferences = 0;
    quint64 maxAnomalyPct = 0;
    QString activeRegime;

    for (auto it = m_nodeStatuses.cbegin(); it != m_nodeStatuses.cend(); ++it) {
        if (it.value().typeName == QStringLiteral("tinyml.ecu")) {
            totalInferences += counterValue(&it.value(), QStringLiteral("Inferences run"));
            const quint64 score = counterValue(&it.value(), QStringLiteral("Anomaly score (%)"));
            if (score >= maxAnomalyPct) {
                maxAnomalyPct = score;
                activeRegime =
                    regimeName(counterValue(&it.value(), QStringLiteral("Regime class")));
            }
        }
    }

    QString summary = tr("Pipeline: %1 blocks • %2 wires")
                          .arg(m_description.nodes().size())
                          .arg(m_description.edges().size());

    QString stateToken = QStringLiteral("ready");

    if (m_simulationRunning) {
        stateToken = (faultActive || maxAnomalyPct >= 65) ? QStringLiteral("error")
                                                          : QStringLiteral("online");
        if (!activeRegime.isEmpty()) {
            const quint64 displayAnom =
                faultActive ? std::max<quint64>(maxAnomalyPct, 89) : maxAnomalyPct;
            const QString displayRegime = faultActive ? QStringLiteral("ANOMALY") : activeRegime;
            summary += tr("   |   TinyML: %1 (%2% anom, %3 inf)")
                           .arg(displayRegime)
                           .arg(displayAnom)
                           .arg(totalInferences);
        }
    }

    if (summary != m_statusSummaryText || stateToken != m_statusSummaryState) {
        m_statusSummaryText = summary;
        m_statusSummaryState = stateToken;
        Q_EMIT statusSummaryChanged(m_statusSummaryText, m_statusSummaryState);
    }
}

QByteArray CanvasPanel::splitterState() const
{
    return m_splitter != nullptr ? m_splitter->saveState() : QByteArray{};
}

void CanvasPanel::restoreSplitterState(const QByteArray& state)
{
    if (m_splitter == nullptr || state.isEmpty()) {
        return;
    }

    if (!m_splitter->restoreState(state)) {
        qWarning("TorqueBus: the saved Pipeline divider position could not be restored.");
    }
}

void CanvasPanel::releaseGraph()
{
    if (m_flowAnimation != nullptr) {
        m_flowAnimation->stop();
    }

    delete m_view;
    m_view = nullptr;

    // The scene clears its selection as it goes, and nothing here should look at it by then.
    if (m_scene != nullptr) {
        QObject::disconnect(m_scene, nullptr, this, nullptr);
    }
    delete m_scene;
    m_scene = nullptr;
    updateEditButtons();

    m_model.reset();
}

void CanvasPanel::buildPalette()
{
    m_palette->clear();

    QMap<QString, QTreeWidgetItem*> groups;

    for (const NodeTypeInfo& info : m_catalog.types()) {
        const QString category = QString::fromStdString(info.category);

        QTreeWidgetItem* group = groups.value(category, nullptr);
        if (group == nullptr) {
            group = new QTreeWidgetItem{m_palette, QStringList{category}};
            group->setFlags(Qt::ItemIsEnabled);
            group->setExpanded(true);
            groups.insert(category, group);
        }

        auto* entry =
            new QTreeWidgetItem{group, QStringList{QString::fromStdString(info.displayName)}};
        entry->setData(0, kTypeNameRole, QString::fromStdString(info.typeName));
        entry->setToolTip(0, QString::fromStdString(info.description));
    }

    applyPaletteIcons();
}

void CanvasPanel::applyPaletteIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr || m_palette == nullptr) {
        return;
    }

    for (int i = 0; i < m_palette->topLevelItemCount(); ++i) {
        QTreeWidgetItem* group = m_palette->topLevelItem(i);
        const QString category = group->text(0);
        group->setIcon(0, themes->icon(iconNameForCategory(category)));

        for (int j = 0; j < group->childCount(); ++j) {
            QTreeWidgetItem* entry = group->child(j);
            const QString typeName = entry->data(0, kTypeNameRole).toString();
            entry->setIcon(0, themes->icon(iconNameForNodeType(typeName)));
        }
    }
}

void CanvasPanel::buildNavigationToolBar()
{
    m_navToolBar = new QToolBar{this};
    m_navToolBar->setObjectName(QStringLiteral("torquebus.toolbar.canvas"));
    m_navToolBar->setOrientation(Qt::Vertical);
    m_navToolBar->setIconSize(QSize{18, 18});
    m_navToolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_actionPan = m_navToolBar->addAction(tr("Pan"));
    m_actionPan->setObjectName(QStringLiteral("torquebus.action.canvas.pan"));
    m_actionPan->setCheckable(true);
    m_actionPan->setToolTip(
        tr("Pan Tool: drag anywhere on the canvas to move around, without moving any block"));
    connect(m_actionPan, &QAction::toggled, this, &CanvasPanel::setPanToolActive);

    // The wheel zooms by the same step and inside the same limits, so the buttons and the wheel
    // can be mixed.
    m_actionZoomIn = m_navToolBar->addAction(tr("Zoom In"));
    m_actionZoomIn->setObjectName(QStringLiteral("torquebus.action.canvas.zoomIn"));
    m_actionZoomIn->setToolTip(tr("Zoom In (mouse wheel)"));
    connect(m_actionZoomIn, &QAction::triggered, m_view, &QtNodes::GraphicsView::scaleUp);

    m_actionZoomOut = m_navToolBar->addAction(tr("Zoom Out"));
    m_actionZoomOut->setObjectName(QStringLiteral("torquebus.action.canvas.zoomOut"));
    m_actionZoomOut->setToolTip(tr("Zoom Out (mouse wheel)"));
    connect(m_actionZoomOut, &QAction::triggered, m_view, &QtNodes::GraphicsView::scaleDown);

    m_actionFit = m_navToolBar->addAction(tr("Fit to Window"));
    m_actionFit->setObjectName(QStringLiteral("torquebus.action.canvas.fit"));
    m_actionFit->setToolTip(
        tr("Fit to Window: zoom and centre the canvas so every block is in view"));
    connect(m_actionFit, &QAction::triggered, this, &CanvasPanel::fitToView);

    applyNavigationIcons();
}

void CanvasPanel::applyNavigationIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr || m_navToolBar == nullptr) {
        return;
    }

    m_actionPan->setIcon(themes->icon(QStringLiteral("pan")));
    m_actionZoomIn->setIcon(themes->icon(QStringLiteral("zoom-in")));
    m_actionZoomOut->setIcon(themes->icon(QStringLiteral("zoom-out")));
    m_actionFit->setIcon(themes->icon(QStringLiteral("fit")));
}

void CanvasPanel::applyHudIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr || m_hudBar == nullptr) {
        return;
    }

    m_addBlockButton->setIcon(themes->icon(QStringLiteral("add")));
    m_deleteBlockButton->setIcon(themes->icon(QStringLiteral("clear")));
    m_arrangeButton->setIcon(themes->icon(QStringLiteral("network")));
    m_detailsButton->setIcon(themes->icon(QStringLiteral("subplots")));
    m_animateButton->setIcon(themes->icon(QStringLiteral("simulation")));
    m_demoPipelineButton->setIcon(themes->icon(QStringLiteral("neural")));
    m_injectFaultButton->setIcon(themes->icon(QStringLiteral("diagnostics")));
}

void CanvasPanel::setHudLabelsShown(bool shown)
{
    m_hudCompact = !shown;
    for (QPushButton* button : m_hudBar->findChildren<QPushButton*>()) {
        const QString label = button->property("hudLabel").toString();
        const QString tip = button->property("hudTip").toString();
        button->setText(shown ? label : QString{});
        button->setToolTip(shown ? tip : QStringLiteral("%1 - %2").arg(label, tip));
    }
}

void CanvasPanel::applyHudSizing()
{
    if (m_hudBar == nullptr) {
        return;
    }

    const auto buttons = m_hudBar->findChildren<QPushButton*>();
    m_hudLabelledWidth = 0;

    // Icons alone are only an option when every button has one.
    const bool allHaveIcons = std::ranges::none_of(
        buttons, [](const QPushButton* button) { return button->icon().isNull(); });
    if (!allHaveIcons) {
        setHudLabelsShown(true);
        return;
    }

    // What the labels need, measured with them on and no minimum in the way ...
    setHudLabelsShown(true);
    for (QPushButton* button : buttons) {
        button->setMinimumWidth(0);
        button->ensurePolished();
    }
    m_hudBar->layout()->invalidate();
    const int labelled = m_hudBar->layout()->sizeHint().width();

    // ... and the least the bar can be: the icons alone. A style sheet's min-width becomes the
    // widget's own minimum and 0 would let a button be squeezed until it is cut off, so the minimum
    // is put back, to what the button needs in whichever form it is shown.
    setHudLabelsShown(false);
    for (QPushButton* button : buttons) {
        button->ensurePolished();
        button->setMinimumWidth(button->sizeHint().width());
    }

    setHudLabelsShown(true);
    m_hudLabelledWidth = labelled;
    updateHudCompactness();
}

void CanvasPanel::updateHudCompactness()
{
    if (m_hudBar == nullptr || m_hudLabelledWidth <= 0 || !m_hudBar->isVisible()) {
        return;
    }

    const bool compact = m_hudBar->width() < m_hudLabelledWidth;
    if (compact != m_hudCompact) {
        setHudLabelsShown(!compact);
    }
}

void CanvasPanel::updateEditButtons()
{
    if (m_deleteBlockButton != nullptr) {
        m_deleteBlockButton->setEnabled(m_scene != nullptr && !m_scene->selectedItems().isEmpty());
    }
}

void CanvasPanel::setPanToolActive(bool active)
{
    m_panToolActive = active;

    if (m_view != nullptr) {
        // The view scrolls by hand-drag whether or not it is interactive; what interactive adds is
        // handing the press to the scene, which is how a block gets picked up, selected or wired.
        m_view->setInteractive(!active);
    }
}

QRectF CanvasPanel::contentBounds() const
{
    QRectF bounds;

    if (!m_model || m_scene == nullptr) {
        return bounds;
    }

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QSize nodeSize = m_scene->nodeGeometry().size(nodeId);
        const QRectF nodeRect = ngo->mapRectToScene(QRectF{QPointF{0.0, 0.0}, QSizeF{nodeSize}});

        bounds = bounds.united(nodeRect);
        if (node.typeName == "tinyml.ecu" && m_model->isExpanded(nodeId)) {
            bounds = bounds.united(tinyMlCardRect(nodeRect));
        }
    }

    return bounds;
}

void CanvasPanel::fitIfPending()
{
    if (m_fitPending) {
        fitToView();
    }
}

void CanvasPanel::fitToView()
{
    // A view that is not shown has not been given its size, and a fit to the size it has instead is
    // worse than none: it stays pending for the time it is shown.
    if (m_view == nullptr || m_view->viewport() == nullptr || !m_view->isVisible()) {
        return;
    }

    m_fitPending = false;

    const QRectF bounds = contentBounds();

    // Nothing to fit: go home, which is real size around the origin.
    if (bounds.isEmpty()) {
        m_view->setupScale(1.0);
        m_view->centerOn(QPointF{});
        return;
    }

    const double margins = 2.0 * kFitMarginPx;
    const double roomWidth = std::max(1.0, m_view->viewport()->width() - margins);
    const double roomHeight = std::max(1.0, m_view->viewport()->height() - margins);

    const double scale = std::clamp(
        std::min(roomWidth / bounds.width(), roomHeight / bounds.height()), kMinZoom, kFitMaxZoom);

    m_view->setupScale(scale);
    m_view->centerOn(bounds.center());
}

QPointF CanvasPanel::findNonOverlappingPosition(const QPointF& desired) const
{
    // What a block that is added is, and how much room to leave round it.
    constexpr qreal kClear = 14.0;
    const QSizeF added{layout::kChipWidth, layout::kChipHeight};

    const auto isFree = [&](const QPointF& at) {
        const QRectF wanted = QRectF{at, added}.adjusted(-kClear, -kClear, kClear, kClear);

        for (const NodeDescription& existing : m_description.nodes()) {
            QSize size{static_cast<int>(layout::kChipWidth), static_cast<int>(layout::kChipHeight)};
            if (m_model) {
                size = m_model->blockSize(m_model->canvasId(existing.id));
            }

            QRectF taken{existing.x,
                         existing.y,
                         static_cast<qreal>(size.width()),
                         static_cast<qreal>(size.height())};

            // What an opened TinyML block paints under itself is part of where it is.
            if (existing.typeName == "tinyml.ecu" && m_model
                && m_model->isExpanded(m_model->canvasId(existing.id))) {
                taken = taken.united(tinyMlCardRect(taken));
            }

            if (wanted.intersects(taken)) {
                return false;
            }
        }
        return true;
    };

    // Where it was asked to be, then along the row and down the rows.
    constexpr int kColumns = 8;
    constexpr int kRows = 12;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const QPointF candidate =
                desired
                + QPointF{column * layout::kChipPitch,
                          row * (layout::kChipHeight + layout::kRowGap * 0.5)};
            if (isFree(candidate)) {
                return candidate;
            }
        }
    }

    return desired;
}

QtNodes::NodeId CanvasPanel::selectedBus() const
{
    if (!m_model || m_scene == nullptr) {
        return QtNodes::InvalidNodeId;
    }

    // A bus that is selected, or failing that the bus a selected block is on - when it is on
    // exactly one: that is what makes adding several modules in a row put them all on the same bus,
    // where the selection has moved on to the one just added.
    QtNodes::NodeId hangsOn = QtNodes::InvalidNodeId;
    for (QGraphicsItem* item : m_scene->selectedItems()) {
        const auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
        if (ngo == nullptr) {
            continue;
        }
        if (m_model->isBus(ngo->nodeId())) {
            return ngo->nodeId();
        }
        if (const auto buses = m_model->busesOf(ngo->nodeId()); buses.size() == 1) {
            hangsOn = buses.front();
        }
    }

    return hangsOn;
}

QtNodes::NodeId CanvasPanel::busAt(const QPointF& scenePosition, qreal slack) const
{
    if (!m_model || m_scene == nullptr) {
        return QtNodes::InvalidNodeId;
    }

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId || !m_model->isBus(nodeId)) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QRectF rail = ngo->mapRectToScene(
            QRectF{QPointF{0.0, 0.0}, QSizeF{m_scene->nodeGeometry().size(nodeId)}});

        if (rail.adjusted(0.0, -slack, 0.0, slack).contains(scenePosition)) {
            return nodeId;
        }
    }

    return QtNodes::InvalidNodeId;
}

void CanvasPanel::addNodeFromPalette(QTreeWidgetItem* item)
{
    if (item == nullptr || !m_model || m_view == nullptr) {
        return;
    }

    const QString typeName = item->data(0, kTypeNameRole).toString();
    if (typeName.isEmpty()) {
        return;
    }

    const QPointF viewCenter = m_view->mapToScene(m_view->viewport()->rect().center())
                               - QPointF{layout::kChipWidth * 0.5, layout::kChipHeight * 0.5};

    // With a bus selected, a block added is added to it: that is the whole point of
    // selecting one, and the alternative is a block that has to be wired by hand to a
    // bus that is right there. Unless it has nothing to wire: a plot is not put on a bus.
    if (const QtNodes::NodeId bus = selectedBus();
        bus != QtNodes::InvalidNodeId && typeName != QLatin1String("can.bus")) {
        const NodeTypeInfo* info = m_catalog.find(typeName.toStdString());
        const bool framesPort =
            info != nullptr
            && (std::ranges::any_of(
                    info->inputs,
                    [](const PortDescriptor& port) { return port.type == PortType::Frames; })
                || std::ranges::any_of(info->outputs, [](const PortDescriptor& port) {
                       return port.type == PortType::Frames;
                   }));
        if (framesPort) {
            addModuleToBus(typeName, bus, viewCenter);
            return;
        }
    }

    addNodeAt(typeName, findNonOverlappingPosition(viewCenter));
}

void CanvasPanel::addSelectedPaletteNode()
{
    if (m_palette != nullptr && m_palette->currentItem() != nullptr) {
        const QString typeName = m_palette->currentItem()->data(0, kTypeNameRole).toString();
        if (!typeName.isEmpty()) {
            addNodeFromPalette(m_palette->currentItem());
            return;
        }
    }

    if (m_view == nullptr || m_addBlockButton == nullptr) {
        return;
    }

    const QPointF center = m_view->mapToScene(m_view->viewport()->rect().center())
                           - QPointF{layout::kChipWidth * 0.5, layout::kChipHeight * 0.5};
    if (QMenu* menu = buildSceneMenu(findNonOverlappingPosition(center))) {
        menu->popup(m_addBlockButton->mapToGlobal(QPoint{0, m_addBlockButton->height()}));
    }
}

void CanvasPanel::addNodeAt(const QString& typeName, const QPointF& scenePosition)
{
    const QtNodes::NodeId nodeId =
        addNodeAtReturning(typeName, findNonOverlappingPosition(scenePosition));
    if (nodeId == QtNodes::InvalidNodeId) {
        return;
    }

    // A second bus is another channel: the one the first is not.
    if (typeName == QLatin1String("can.bus")) {
        std::set<std::int64_t> taken;
        for (const NodeDescription& node : m_description.nodes()) {
            if (node.typeName == kCanBusType && node.id != m_model->descriptionId(nodeId)) {
                taken.insert(node.parameters.integer("channel", 0));
            }
        }
        std::int64_t channel = 0;
        while (taken.contains(channel)) {
            ++channel;
        }

        if (NodeDescription* added = editableNode(nodeId)) {
            added->parameters.set("channel", ParameterValue::fromInteger(channel));
        }
    }

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(nodeId);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(nodeId)));
}

void CanvasPanel::addNodeAtDrop(const QString& typeName, const QPointF& scenePosition)
{
    // Let go over a bus, the block is on it.
    const qreal reach = layout::kDropLength + layout::kChipHeight;
    if (const QtNodes::NodeId bus = busAt(scenePosition, reach);
        bus != QtNodes::InvalidNodeId && typeName != QLatin1String("can.bus")) {
        addModuleToBus(typeName, bus, scenePosition);
        return;
    }

    addNodeAt(typeName,
              scenePosition - QPointF{layout::kChipWidth * 0.5, layout::kChipHeight * 0.5});
}

bool CanvasPanel::placeOnBusNear(QtNodes::NodeId block, const QPointF& scenePosition)
{
    const QtNodes::NodeId bus = busAt(scenePosition, layout::kDropLength + layout::kChipHeight);
    if (bus == QtNodes::InvalidNodeId) {
        return false;
    }

    return m_model->attachToBus(block, bus);
}

void CanvasPanel::addModuleToBus(const QString& typeName,
                                 QtNodes::NodeId busId,
                                 const QPointF& near)
{
    if (!m_model || m_scene == nullptr || !m_model->isBus(busId)) {
        return;
    }

    QtNodes::NodeGraphicsObject* rail = m_scene->nodeGraphicsObject(busId);
    if (rail == nullptr) {
        return;
    }

    const QRectF railRect = rail->mapRectToScene(
        QRectF{QPointF{0.0, 0.0}, QSizeF{m_scene->nodeGeometry().size(busId)}});

    // Under the rail unless that is where it was asked not to be: a block let go above the middle
    // of the rail hangs above it.
    const bool above = near.y() + layout::kChipHeight * 0.5 < railRect.center().y();

    const qreal y = above ? railRect.top() - layout::kDropLength - layout::kChipHeight
                          : railRect.bottom() + layout::kDropLength;
    const qreal firstX = railRect.left() + layout::kRailPlateWidth + layout::kRailInset;
    const qreal wantedX = std::max(firstX, near.x());

    const QtNodes::NodeId block = addNodeAtReturning(typeName, QPointF{wantedX, y});
    if (block == QtNodes::InvalidNodeId) {
        return;
    }

    // Along the row to the first place that is free, so that a block added is never put on top of
    // one that is there.
    const QSizeF chip{layout::kChipWidth, layout::kChipHeight};
    QPointF at{wantedX, y};
    for (int step = 0; step < 24; ++step) {
        const QRectF wanted = QRectF{at, chip}.adjusted(-6.0, -6.0, 6.0, 6.0);
        bool free = true;
        for (const NodeDescription& other : m_description.nodes()) {
            const QtNodes::NodeId otherId = m_model->canvasId(other.id);
            if (otherId == block || m_model->isBus(otherId)) {
                continue;
            }
            const QSize size = m_model->blockSize(otherId);
            if (wanted.intersects(QRectF{other.x,
                                         other.y,
                                         static_cast<qreal>(size.width()),
                                         static_cast<qreal>(size.height())})) {
                free = false;
                break;
            }
        }
        if (free) {
            break;
        }
        at.rx() += layout::kChipPitch;
    }
    m_model->setNodeData(block, QtNodes::NodeRole::Position, at);

    // On the bus's channel, as the legacy "attach" did and for the same reason: a module that
    // says it is on CAN 1 while it is wired to CAN 2 is a bug nobody can see.
    if (NodeDescription* added = editableNode(block)) {
        if (const NodeTypeInfo* info = m_catalog.find(added->typeName)) {
            const bool hasChannel =
                std::ranges::any_of(info->parameters, [](const ParameterDescriptor& parameter) {
                    return parameter.name == "channel";
                });
            if (hasChannel) {
                added->parameters.set("channel",
                                      ParameterValue::fromInteger(m_model->busChannel(busId)));
            }
        }
    }

    m_model->attachToBus(block, busId);

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(block);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    refreshTelemetryCache();
    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(block)));
    Q_EMIT graphEdited();
}

bool CanvasPanel::attachToBus(const QString& blockId, const QString& busId)
{
    if (!m_model) {
        return false;
    }

    const bool attached = m_model->attachToBus(m_model->canvasId(blockId.toStdString()),
                                               m_model->canvasId(busId.toStdString()));
    if (attached) {
        Q_EMIT graphEdited();
    }
    return attached;
}

void CanvasPanel::detachFromBus(const QString& blockId, const QString& busId)
{
    if (!m_model) {
        return;
    }

    m_model->detachFromBus(m_model->canvasId(blockId.toStdString()),
                           m_model->canvasId(busId.toStdString()));
    Q_EMIT graphEdited();
}

void CanvasPanel::cycleFrameFormat(QtNodes::ConnectionId wire)
{
    if (!m_model) {
        return;
    }

    // All, then 11-bit only, then 29-bit only, and round again.
    const CanFormatMatch next = [&] {
        switch (m_model->frameFormat(wire)) {
        case CanFormatMatch::Any:
            return CanFormatMatch::StandardOnly;
        case CanFormatMatch::StandardOnly:
            return CanFormatMatch::ExtendedOnly;
        case CanFormatMatch::ExtendedOnly:
            break;
        }
        return CanFormatMatch::Any;
    }();

    m_model->setFrameFormat(wire, next);
    invalidateItems();
    Q_EMIT graphEdited();
}

void CanvasPanel::setBlockExpanded(QtNodes::NodeId nodeId, bool expanded)
{
    if (!m_model || m_scene == nullptr || m_model->isExpanded(nodeId) == expanded
        || m_model->isBus(nodeId)) {
        return;
    }

    const NodeDescription* node = m_model->node(nodeId);
    if (node == nullptr) {
        return;
    }

    const QPointF at{node->x, node->y};
    const QSize before = m_model->blockSize(nodeId);
    const std::string typeName = node->typeName;

    // Which side of its bus the block hangs on: a card is a good deal taller than a chip, and grows
    // away from the bus, not across it.
    bool aboveItsBus = false;
    for (const QtNodes::NodeId bus : m_model->busesOf(nodeId)) {
        if (const NodeDescription* rail = m_model->node(bus)) {
            aboveItsBus = at.y() + before.height() * 0.5 < rail->y + layout::kRailHeight * 0.5;
            break;
        }
    }

    m_model->setExpanded(nodeId, expanded);

    const QSize after = m_model->blockSize(nodeId);
    const qreal widthChange = after.width() - before.width();
    qreal heightChange = after.height() - before.height();

    // What an opened TinyML block paints under itself is part of how far it grows.
    if (typeName == "tinyml.ecu") {
        const qreal network = layout::kTinyMlCardGap + layout::kTinyMlCardHeight;
        heightChange += expanded ? network : -network;
    }

    // Room for it: what is beside the block in its row, to the right, makes way - and comes back
    // when the card is closed. Not an arrangement of everything; the person put the rest where it
    // is.
    for (const NodeDescription& other : m_description.nodes()) {
        const QtNodes::NodeId otherId = m_model->canvasId(other.id);
        if (otherId == nodeId || otherId == QtNodes::InvalidNodeId || m_model->isBus(otherId)) {
            continue;
        }

        const bool sameRow = std::abs(other.y - at.y()) < layout::kChipHeight;
        if (sameRow && other.x >= at.x() + before.width() - 1.0) {
            m_model->setNodeData(
                otherId, QtNodes::NodeRole::Position, QPointF{other.x + widthChange, other.y});
        }
    }

    if (aboveItsBus && heightChange != 0.0) {
        m_model->setNodeData(
            nodeId, QtNodes::NodeRole::Position, QPointF{at.x(), at.y() - heightChange});
    }

    refreshTelemetryCache();
    invalidateItems();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

std::vector<std::pair<std::string, QPointF>> CanvasPanel::blockPositions() const
{
    std::vector<std::pair<std::string, QPointF>> positions;
    positions.reserve(m_description.nodes().size());
    for (const NodeDescription& node : m_description.nodes()) {
        positions.emplace_back(node.id, QPointF{node.x, node.y});
    }
    return positions;
}

void CanvasPanel::placeBlocks(const std::vector<std::pair<std::string, QPointF>>& positions)
{
    if (!m_model) {
        return;
    }

    // Modules first and the buses last: a bus will not be put to the right of a module it carries,
    // and the modules are where they are going to be only once they have been moved.
    //
    // As one batch: each block moved on its own has the scene lay out its bus and redraw all of its
    // wires, which for a bus of eighty modules is eighty times over.
    {
        const PipelineGraphModel::MoveBatch batch{*m_model};

        for (const bool buses : {false, true}) {
            for (const auto& [id, position] : positions) {
                const QtNodes::NodeId nodeId = m_model->canvasId(id);
                if (nodeId == QtNodes::InvalidNodeId || m_model->isBus(nodeId) != buses) {
                    continue;
                }
                m_model->setNodeData(nodeId, QtNodes::NodeRole::Position, position);
            }
        }
    }

    refreshTelemetryCache();
    invalidateItems();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

std::vector<std::pair<std::string, QPointF>> CanvasPanel::arrangedPositions() const
{
    ArrangeOptions options;

    // As many blocks in a row as the window shows at a size that can be read: a row that has to be
    // zoomed out to fit is a row that is not.
    const qreal viewportWidth =
        m_view != nullptr && m_view->viewport() != nullptr ? m_view->viewport()->width() : 1200.0;
    const qreal readable = viewportWidth / 0.7;
    options.maxPerRow =
        std::clamp(static_cast<int>((readable - layout::kRailPlateWidth - 2.0 * layout::kRailInset)
                                    / layout::kChipPitch),
                   3,
                   12);

    for (const NodeDescription& node : m_description.nodes()) {
        if (m_model->isExpanded(m_model->canvasId(node.id))) {
            options.expanded.insert(node.id);
        }
    }

    std::vector<std::pair<std::string, QPointF>> positions;
    for (const Placement& placement : layOutBlocks(m_description, options)) {
        positions.emplace_back(placement.id, QPointF{placement.x, placement.y});
    }
    return positions;
}

void CanvasPanel::setDetailsShown(bool shown)
{
    if (!m_model || m_scene == nullptr) {
        return;
    }

    if (m_detailsButton != nullptr && m_detailsButton->isChecked() != shown) {
        const QSignalBlocker blocker{m_detailsButton};
        m_detailsButton->setChecked(shown);
    }

    // What is open now, to put back, and where everything is.
    std::vector<std::string> openBefore;
    for (const NodeDescription& node : m_description.nodes()) {
        if (m_model->isExpanded(m_model->canvasId(node.id))) {
            openBefore.push_back(node.id);
        }
    }
    const auto positionsBefore = blockPositions();

    m_model->setAllExpanded(shown);

    // Cards are wider and taller than chips, and a canvas laid out for chips has none of the room.
    const auto positionsAfter = arrangedPositions();
    placeBlocks(positionsAfter);

    m_fitPending = true;
    fitToView();

    const auto restore = [this](const std::vector<std::string>& open,
                                const std::vector<std::pair<std::string, QPointF>>& positions) {
        if (!m_model) {
            return;
        }
        m_model->setAllExpanded(false);
        for (const std::string& id : open) {
            m_model->setExpanded(m_model->canvasId(id), true);
        }
        placeBlocks(positions);

        if (m_detailsButton != nullptr) {
            const QSignalBlocker blocker{m_detailsButton};
            m_detailsButton->setChecked(!open.empty());
        }
        Q_EMIT graphEdited();
    };

    std::vector<std::string> openAfter;
    if (shown) {
        for (const NodeDescription& node : m_description.nodes()) {
            if (node.typeName != kCanBusType) {
                openAfter.push_back(node.id);
            }
        }
    }

    m_scene->undoStack().push(new CanvasActionCommand{
        shown ? tr("Show Frame Details") : tr("Hide Frame Details"),
        [restore, openBefore, positionsBefore] { restore(openBefore, positionsBefore); },
        [restore, openAfter, positionsAfter] { restore(openAfter, positionsAfter); }});

    Q_EMIT graphEdited();
}

void CanvasPanel::arrangeBlocks()
{
    if (!m_model || m_scene == nullptr) {
        return;
    }

    const auto before = blockPositions();
    const auto after = arrangedPositions();

    placeBlocks(after);

    m_fitPending = true;
    fitToView();

    // Rearranging is the one thing here that throws away what a person did by hand, so it can be
    // taken back.
    if (before != after) {
        const auto restore = [this](const std::vector<std::pair<std::string, QPointF>>& positions) {
            placeBlocks(positions);
            Q_EMIT graphEdited();
        };

        m_scene->undoStack().push(new CanvasActionCommand{tr("Arrange Blocks"),
                                                          [restore, before] { restore(before); },
                                                          [restore, after] { restore(after); }});
    }

    Q_EMIT graphEdited();
}

void CanvasPanel::restoreGraph(const GraphDescription& graph)
{
    if (!m_model) {
        return;
    }

    m_description = graph;

    // Blocks and wires were replaced under the model, which holds an id for each. Not reload():
    // that forgets the history, and this is being run by the history.
    m_model->reload();
    m_chipLines.clear();

    refreshTelemetryCache();
    updateHudLabels();
    updateEditButtons();

    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }

    Q_EMIT nodeSelected(QString{});
    Q_EMIT graphEdited();
}

void CanvasPanel::groupOnBus()
{
    if (!m_model) {
        return;
    }

    const GraphDescription before = m_description;

    const BusGrouping grouping = groupOnBuses(m_description);
    if (!grouping.changed()) {
        return;
    }

    // Where the blocks were put was where a CAN Channel in the middle and a column of ECUs wanted
    // them, and says nothing about where they go round a bus - least of all in what order. Left at
    // the origin, the order they are laid out in is the order the project lists them, which is the
    // one somebody wrote them down in.
    for (NodeDescription& node : m_description.nodes()) {
        node.x = 0.0;
        node.y = 0.0;
    }

    // Blocks and wires were replaced under the model, which holds an id for each.
    m_model->reload();
    m_chipLines.clear();

    refreshTelemetryCache();
    updateHudLabels();
    updateEditButtons();

    placeBlocks(arrangedPositions());
    m_fitPending = true;
    fitToView();

    // The history so far names blocks that are not there, so it goes - and what remains is this:
    // one step, to put the pipeline back as it was.
    if (m_scene != nullptr) {
        m_scene->undoStack().clear();
        m_scene->undoStack().push(
            new CanvasActionCommand{tr("Group on CAN Bus"),
                                    [this, before] { restoreGraph(before); },
                                    [this, after = m_description] { restoreGraph(after); }});
    }

    Q_EMIT nodeSelected(QString{});
    Q_EMIT graphEdited();
}

QMenu* CanvasPanel::buildSceneMenu(const QPointF& scenePosition)
{
    auto* menu = new QMenu{m_view};
    menu->setAttribute(Qt::WA_DeleteOnClose);

    QMap<QString, QMenu*> groups;

    for (const NodeTypeInfo& info : m_catalog.types()) {
        const QString category = QString::fromStdString(info.category);

        QMenu* group = groups.value(category, nullptr);
        if (group == nullptr) {
            group = menu->addMenu(category);
            groups.insert(category, group);
        }

        const QString typeName = QString::fromStdString(info.typeName);

        QAction* action = group->addAction(QString::fromStdString(info.displayName));
        action->setToolTip(QString::fromStdString(info.description));

        connect(action, &QAction::triggered, this, [this, typeName, scenePosition] {
            addNodeAtDrop(typeName, scenePosition);
        });
    }

    menu->addSeparator();

    QAction* arrange = menu->addAction(tr("Arrange Blocks"));
    connect(arrange, &QAction::triggered, this, &CanvasPanel::arrangeBlocks);

    QAction* group = menu->addAction(tr("Group the Wiring on a CAN Bus"));
    group->setToolTip(
        tr("Replace the CAN Channel and CAN Transmit block of each ECU with one CAN Bus "
           "per channel. What runs does not change."));
    group->setEnabled(canGroupOnBuses(m_description));
    connect(group, &QAction::triggered, this, &CanvasPanel::groupOnBus);

    menu->addSeparator();
    QAction* demoAction = menu->addAction(tr("Build TinyML Virtual Vehicle Demo"));
    connect(demoAction, &QAction::triggered, this, [this] { buildTinyMlDemoPipeline(); });

    if (m_scene != nullptr && !m_scene->selectedItems().isEmpty()) {
        menu->addSeparator();
        QAction* deleteSelected = menu->addAction(tr("Delete Selected (Del)"));
        connect(deleteSelected, &QAction::triggered, this, &CanvasPanel::deleteSelectedItems);
    }

    return menu;
}

void CanvasPanel::showBusMenu(QtNodes::NodeId busId, const QPointF& scenePosition)
{
    if (!m_model || m_view == nullptr) {
        return;
    }

    const std::string descriptionId = m_model->descriptionId(busId);

    QMenu menu{m_view};

    QAction* heading = menu.addAction(QString::fromStdString(descriptionId));
    heading->setEnabled(false);
    menu.addSeparator();

    // What can be put on a bus: anything with a frames port, by the group the palette has it in.
    QMap<QString, QMenu*> groups;
    QMenu* add = menu.addMenu(tr("Add to This Bus"));

    for (const NodeTypeInfo& info : m_catalog.types()) {
        if (info.typeName == kCanBusType) {
            continue;
        }

        const bool framesPort =
            std::ranges::any_of(
                info.inputs,
                [](const PortDescriptor& port) { return port.type == PortType::Frames; })
            || std::ranges::any_of(info.outputs, [](const PortDescriptor& port) {
                   return port.type == PortType::Frames;
               });
        if (!framesPort) {
            continue;
        }

        const QString category = QString::fromStdString(info.category);
        QMenu* group = groups.value(category, nullptr);
        if (group == nullptr) {
            group = add->addMenu(category);
            groups.insert(category, group);
        }

        const QString typeName = QString::fromStdString(info.typeName);
        QAction* action = group->addAction(QString::fromStdString(info.displayName));
        action->setToolTip(QString::fromStdString(info.description));

        connect(action, &QAction::triggered, this, [this, typeName, busId, scenePosition] {
            addModuleToBus(typeName, busId, scenePosition);
        });
    }

    // The one most people reach for, without a trip through two submenus.
    QAction* ecu = menu.addAction(tr("Add Simulated ECU"));
    connect(ecu, &QAction::triggered, this, [this, busId, scenePosition] {
        addModuleToBus(QStringLiteral("lua.ecu"), busId, scenePosition);

        // A script that runs, as the legacy "attach" gave: an ECU that is added to be looked at.
        if (m_scene == nullptr) {
            return;
        }
        for (QGraphicsItem* item : m_scene->selectedItems()) {
            if (const auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
                if (NodeDescription* node = editableNode(ngo->nodeId());
                    node != nullptr && node->typeName == "lua.ecu"
                    && node->parameters.text("script").empty()) {
                    node->parameters.set(
                        "script",
                        ParameterValue::fromText(defaultVehicleEcuScript().toStdString()));
                    Q_EMIT nodeSelected(QString::fromStdString(node->id));
                }
            }
        }
    });

    QAction* tinyml = menu.addAction(tr("Add TinyML Virtual ECU"));
    connect(tinyml, &QAction::triggered, this, [this, busId, scenePosition] {
        addModuleToBus(QStringLiteral("tinyml.ecu"), busId, scenePosition);
    });

    menu.addSeparator();

    QAction* arrange = menu.addAction(tr("Arrange Blocks"));
    connect(arrange, &QAction::triggered, this, &CanvasPanel::arrangeBlocks);

    QAction* properties = menu.addAction(tr("Settings"));
    connect(properties, &QAction::triggered, this, [this, descriptionId] {
        Q_EMIT nodeSelected(QString::fromStdString(descriptionId));
    });

    menu.addSeparator();

    QAction* remove = menu.addAction(tr("Delete Bus"));
    connect(remove, &QAction::triggered, this, [this, descriptionId] {
        deleteBlock(QString::fromStdString(descriptionId));
    });

    menu.exec(m_view->mapToGlobal(m_view->mapFromScene(scenePosition)));
}

void CanvasPanel::showWireMenu(QtNodes::ConnectionId wire, const QPoint& globalPosition)
{
    if (!m_model || !m_model->isBusTap(wire)) {
        return;
    }

    const bool towardBus = m_model->isBus(wire.inNodeId);
    const QtNodes::NodeId module = towardBus ? wire.outNodeId : wire.inNodeId;
    const QtNodes::NodeId bus = towardBus ? wire.inNodeId : wire.outNodeId;

    const QString moduleName = QString::fromStdString(m_model->descriptionId(module));
    const QString busName = QString::fromStdString(m_model->descriptionId(bus));
    const PipelineGraphModel::BusDirection direction = m_model->busDirection(module, bus);

    QMenu menu{m_view};
    menu.addAction(tr("%1 on %2").arg(moduleName, busName))->setEnabled(false);
    menu.addSeparator();

    // What the module hears: a property of the wire the bus delivers on.
    if (direction.listens) {
        QtNodes::ConnectionId listening{};
        for (const QtNodes::ConnectionId& tap : m_model->tapsBetween(module, bus)) {
            if (tap.outNodeId == bus) {
                listening = tap;
                break;
            }
        }

        const CanFormatMatch current = m_model->frameFormat(listening);
        QMenu* hears = menu.addMenu(tr("Hears"));

        const auto addFormat = [&](const QString& text, CanFormatMatch format) {
            QAction* action = hears->addAction(text);
            action->setCheckable(true);
            action->setChecked(current == format);
            connect(action, &QAction::triggered, this, [this, listening, format] {
                m_model->setFrameFormat(listening, format);
                invalidateItems();
                Q_EMIT graphEdited();
            });
        };
        addFormat(tr("All frames"), CanFormatMatch::Any);
        addFormat(tr("11-bit frames only"), CanFormatMatch::StandardOnly);
        addFormat(tr("29-bit frames only"), CanFormatMatch::ExtendedOnly);
    }

    // And whether it speaks: only a module with both a frames input and a frames output has a
    // choice to make.
    const NodeTypeInfo* type = m_model->info(module);
    const bool hasInput = std::ranges::any_of(
        type->inputs, [](const PortDescriptor& port) { return port.type == PortType::Frames; });
    const bool hasOutput = std::ranges::any_of(
        type->outputs, [](const PortDescriptor& port) { return port.type == PortType::Frames; });
    if (hasInput && hasOutput) {
        QMenu* does = menu.addMenu(tr("Direction"));

        const auto addDirection = [&](const QString& text, bool listens, bool talks) {
            QAction* action = does->addAction(text);
            action->setCheckable(true);
            action->setChecked(direction.listens == listens && direction.talks == talks);
            connect(action, &QAction::triggered, this, [this, module, bus, listens, talks] {
                m_model->setBusDirection(
                    module, bus, PipelineGraphModel::BusDirection{listens, talks});
                invalidateItems();
                Q_EMIT graphEdited();
            });
        };
        addDirection(tr("Receive and send"), true, true);
        addDirection(tr("Receive only"), true, false);
        addDirection(tr("Send only"), false, true);
    }

    menu.addSeparator();

    QAction* detach = menu.addAction(tr("Detach from the Bus"));
    connect(detach, &QAction::triggered, this, [this, module, bus] {
        m_model->detachFromBus(module, bus);
        invalidateItems();
        Q_EMIT graphEdited();
    });

    menu.exec(globalPosition);
}

void CanvasPanel::showNodeMenu(QtNodes::NodeId nodeId, const QPointF& scenePosition)
{
    if (!m_model || m_view == nullptr) {
        return;
    }

    const std::string descriptionId = m_model->descriptionId(nodeId);
    if (descriptionId.empty()) {
        return;
    }

    if (m_model->isBus(nodeId)) {
        showBusMenu(nodeId, scenePosition);
        return;
    }

    QMenu menu{m_view};

    QAction* heading =
        menu.addAction(m_model->nodeData(nodeId, QtNodes::NodeRole::Caption).toString());
    heading->setEnabled(false);
    menu.addSeparator();

    const std::string typeName = nodeTypeName(descriptionId);

    if (typeName == "lua.ecu" || typeName == "lua.test") {
        QAction* edit = menu.addAction(tr("Edit script"));
        if (ThemeManager* tm = ThemeManager::instance()) {
            edit->setIcon(tm->icon(QStringLiteral("edit")));
        }
        connect(edit, &QAction::triggered, this, [this, descriptionId] {
            Q_EMIT editScriptRequested(QString::fromStdString(descriptionId));
        });
        menu.addSeparator();
    }

    if (typeName == "can.source") {
        QAction* attach = menu.addAction(tr("Attach simulated ECU"));
        connect(attach, &QAction::triggered, this, [this, nodeId] { attachEcuTo(nodeId); });

        QAction* attachTinyMl = menu.addAction(tr("Attach TinyML Virtual ECU"));
        connect(
            attachTinyMl, &QAction::triggered, this, [this, nodeId] { attachTinyMlEcuTo(nodeId); });
        menu.addSeparator();
    }

    if (typeName == "tinyml.ecu" && m_injectFaultButton != nullptr) {
        QAction* toggleFault =
            menu.addAction(m_injectFaultButton->isChecked() ? tr("Clear Injected TinyML Anomaly")
                                                            : tr("Inject Thermal/Timing Anomaly"));
        connect(toggleFault, &QAction::triggered, this, [this] { m_injectFaultButton->toggle(); });
        menu.addSeparator();
    }

    // The bus: wire this block to one, or take it off. One bus is a flat entry, and several are a
    // menu of them.
    if (m_model->canAttachToBus(nodeId)) {
        std::vector<QtNodes::NodeId> buses;
        for (const NodeDescription& node : m_description.nodes()) {
            const QtNodes::NodeId candidate = m_model->canvasId(node.id);
            if (candidate != QtNodes::InvalidNodeId && m_model->isBus(candidate)) {
                buses.push_back(candidate);
            }
        }

        const bool several = buses.size() > 1;
        for (const QtNodes::NodeId bus : buses) {
            const QString busName = QString::fromStdString(m_model->descriptionId(bus));
            const bool attached = !m_model->tapsBetween(nodeId, bus).empty();

            if (attached) {
                QAction* detach = menu.addAction(several ? tr("Detach from %1").arg(busName)
                                                         : tr("Detach from the Bus"));
                connect(detach, &QAction::triggered, this, [this, nodeId, bus] {
                    m_model->detachFromBus(nodeId, bus);
                    invalidateItems();
                    Q_EMIT graphEdited();
                });
            } else {
                QAction* attach = menu.addAction(several ? tr("Attach to %1").arg(busName)
                                                         : tr("Attach to the Bus"));
                connect(attach, &QAction::triggered, this, [this, nodeId, bus] {
                    // On the bus's channel, as a drop onto the rail puts it.
                    if (NodeDescription* node = editableNode(nodeId)) {
                        if (const NodeTypeInfo* info = m_catalog.find(node->typeName)) {
                            const bool hasChannel = std::ranges::any_of(
                                info->parameters, [](const ParameterDescriptor& parameter) {
                                    return parameter.name == "channel";
                                });
                            if (hasChannel) {
                                node->parameters.set(
                                    "channel",
                                    ParameterValue::fromInteger(m_model->busChannel(bus)));
                            }
                        }
                    }
                    m_model->attachToBus(nodeId, bus);
                    invalidateItems();
                    Q_EMIT graphEdited();
                });
            }
        }

        if (!buses.empty()) {
            menu.addSeparator();
        }
    }

    // Open into a card, or close.
    {
        const bool open = m_model->isExpanded(nodeId);
        QAction* details = menu.addAction(open ? tr("Close the Card") : tr("Show Frame Details"));
        connect(details, &QAction::triggered, this, [this, nodeId, open] {
            setBlockExpanded(nodeId, !open);
        });
        menu.addSeparator();
    }

    QAction* properties = menu.addAction(tr("Settings"));
    connect(properties, &QAction::triggered, this, [this, descriptionId] {
        Q_EMIT nodeSelected(QString::fromStdString(descriptionId));
    });

    menu.addSeparator();

    QAction* remove = menu.addAction(tr("Delete Block"));
    connect(remove, &QAction::triggered, this, [this, descriptionId] {
        deleteBlock(QString::fromStdString(descriptionId));
    });

    menu.exec(m_view->mapToGlobal(m_view->mapFromScene(scenePosition)));
}

NodeDescription* CanvasPanel::editableNode(QtNodes::NodeId nodeId)
{
    if (!m_model) {
        return nullptr;
    }

    const std::string id = m_model->descriptionId(nodeId);
    for (NodeDescription& node : m_description.nodes()) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

std::string CanvasPanel::nodeTypeName(const std::string& descriptionId) const
{
    for (const NodeDescription& node : m_description.nodes()) {
        if (node.id == descriptionId) {
            return node.typeName;
        }
    }
    return {};
}

QtNodes::NodeId CanvasPanel::findNodeOnChannel(const std::string& typeName,
                                               std::int64_t channel) const
{
    if (!m_model) {
        return QtNodes::InvalidNodeId;
    }

    for (const NodeDescription& node : m_description.nodes()) {
        if (node.typeName != typeName) {
            continue;
        }
        if (node.parameters.integer("channel", -1) != channel) {
            continue;
        }
        return m_model->canvasId(node.id);
    }

    return QtNodes::InvalidNodeId;
}

void CanvasPanel::attachEcuTo(QtNodes::NodeId sourceNodeId)
{
    if (!m_model || m_view == nullptr) {
        return;
    }

    const std::string sourceId = m_model->descriptionId(sourceNodeId);
    if (sourceId.empty()) {
        return;
    }

    std::int64_t channel = 0;
    QPointF sourcePosition;

    for (const NodeDescription& node : m_description.nodes()) {
        if (node.id == sourceId) {
            channel = node.parameters.integer("channel", 0);
            sourcePosition = QPointF{node.x, node.y};
            break;
        }
    }

    constexpr qreal kStepX = layout::kChipPitch;

    const QtNodes::NodeId ecu =
        addNodeAtReturning(QStringLiteral("lua.ecu"),
                           findNonOverlappingPosition(sourcePosition + QPointF{kStepX, 0.0}));
    if (ecu == QtNodes::InvalidNodeId) {
        return;
    }

    const std::string ecuId = m_model->descriptionId(ecu);
    for (NodeDescription& node : m_description.nodes()) {
        if (node.id == ecuId) {
            node.parameters.set("channel", ParameterValue::fromInteger(channel));
            if (node.parameters.text("script").empty()) {
                node.parameters.set(
                    "script", ParameterValue::fromText(defaultVehicleEcuScript().toStdString()));
            }
            break;
        }
    }

    QtNodes::NodeId transmit = findNodeOnChannel("can.transmit", channel);
    if (transmit == QtNodes::InvalidNodeId) {
        transmit = addNodeAtReturning(
            QStringLiteral("can.transmit"),
            findNonOverlappingPosition(sourcePosition + QPointF{2.0 * kStepX, 0.0}));
        if (transmit == QtNodes::InvalidNodeId) {
            return;
        }

        const std::string transmitId = m_model->descriptionId(transmit);
        for (NodeDescription& node : m_description.nodes()) {
            if (node.id == transmitId) {
                node.parameters.set("channel", ParameterValue::fromInteger(channel));
                break;
            }
        }
    }

    connectPorts(sourceNodeId, ecu);
    connectPorts(ecu, transmit);

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(ecu); object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    Q_EMIT nodeSelected(QString::fromStdString(ecuId));
    Q_EMIT graphEdited();
}

void CanvasPanel::attachTinyMlEcuTo(QtNodes::NodeId sourceNodeId)
{
    if (!m_model || m_view == nullptr) {
        return;
    }

    const std::string sourceId = m_model->descriptionId(sourceNodeId);
    if (sourceId.empty()) {
        return;
    }

    std::int64_t channel = 0;
    QPointF sourcePosition;

    for (const NodeDescription& node : m_description.nodes()) {
        if (node.id == sourceId) {
            channel = node.parameters.integer("channel", 0);
            sourcePosition = QPointF{node.x, node.y};
            break;
        }
    }

    constexpr qreal kStepX = layout::kChipPitch;
    constexpr qreal kStepY = layout::kChipHeight + layout::kRowGap;

    QtNodes::NodeId tinyml = findNodeOnChannel("tinyml.ecu", channel);
    if (tinyml == QtNodes::InvalidNodeId) {
        tinyml = addNodeAtReturning(
            QStringLiteral("tinyml.ecu"),
            findNonOverlappingPosition(sourcePosition + QPointF{kStepX, kStepY}));
        if (tinyml == QtNodes::InvalidNodeId) {
            return;
        }

        const std::string tinymlId = m_model->descriptionId(tinyml);
        for (NodeDescription& node : m_description.nodes()) {
            if (node.id == tinymlId) {
                node.parameters.set("channel", ParameterValue::fromInteger(channel));
                break;
            }
        }
    }

    QtNodes::NodeId transmit = findNodeOnChannel("can.transmit", channel);
    if (transmit == QtNodes::InvalidNodeId) {
        transmit = addNodeAtReturning(
            QStringLiteral("can.transmit"),
            findNonOverlappingPosition(sourcePosition + QPointF{2.0 * kStepX, kStepY}));
        if (transmit != QtNodes::InvalidNodeId) {
            const std::string transmitId = m_model->descriptionId(transmit);
            for (NodeDescription& node : m_description.nodes()) {
                if (node.id == transmitId) {
                    node.parameters.set("channel", ParameterValue::fromInteger(channel));
                    break;
                }
            }
        }
    }

    if (transmit != QtNodes::InvalidNodeId) {
        connectPortIndex(tinyml, 0, transmit, 0);
    }

    QtNodes::NodeId plot = QtNodes::InvalidNodeId;
    for (const NodeDescription& node : m_description.nodes()) {
        if (node.typeName == "signal.plot") {
            plot = m_model->canvasId(node.id);
            break;
        }
    }
    if (plot == QtNodes::InvalidNodeId) {
        plot = addNodeAtReturning(
            QStringLiteral("signal.plot"),
            findNonOverlappingPosition(sourcePosition + QPointF{2.0 * kStepX, 2.0 * kStepY}));
    }

    connectPortIndex(sourceNodeId, 0, tinyml, 0);
    if (plot != QtNodes::InvalidNodeId) {
        connectPortIndex(tinyml, 1, plot, 0);
    }

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(tinyml);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(tinyml)));
    Q_EMIT graphEdited();
}

void CanvasPanel::buildTinyMlDemoPipeline()
{
    if (!m_model || m_scene == nullptr) {
        return;
    }

    const GraphDescription before = m_description;
    m_description = GraphDescription{};

    // One bus, and everything on it: the vehicle's ECU and the TinyML one listen to it and speak on
    // it, and what the TinyML block concludes goes to a plot.
    NodeDescription can1;
    can1.id = "can_1";
    can1.typeName = std::string{kCanBusType};
    can1.parameters.set("channel", ParameterValue::fromInteger(0));
    m_description.addNode(std::move(can1));

    NodeDescription vehicleEcu;
    vehicleEcu.id = "ecu_vehicle";
    vehicleEcu.typeName = "lua.ecu";
    vehicleEcu.parameters.set("channel", ParameterValue::fromInteger(0));
    vehicleEcu.parameters.set("script",
                              ParameterValue::fromText(defaultVehicleEcuScript().toStdString()));
    vehicleEcu.parameters.set("speed_id", ParameterValue::fromInteger(0x101));
    vehicleEcu.parameters.set("temp_id", ParameterValue::fromInteger(0x102));
    m_description.addNode(std::move(vehicleEcu));

    NodeDescription tinymlEcu;
    tinymlEcu.id = "tinyml_ecu";
    tinymlEcu.typeName = "tinyml.ecu";
    tinymlEcu.parameters.set("channel", ParameterValue::fromInteger(0));
    tinymlEcu.parameters.set("speedCanId", ParameterValue::fromInteger(0x101));
    tinymlEcu.parameters.set("tempCanId", ParameterValue::fromInteger(0x102));
    tinymlEcu.parameters.set("outputCanId", ParameterValue::fromInteger(0x105));
    tinymlEcu.parameters.set("anomalyThreshold", ParameterValue::fromReal(65.0));
    m_description.addNode(std::move(tinymlEcu));

    NodeDescription plotTinyMl;
    plotTinyMl.id = "tinyml_plot";
    plotTinyMl.typeName = "signal.plot";
    m_description.addNode(std::move(plotTinyMl));

    // What each ECU hears from the bus, and what it puts on it.
    m_description.addEdge(EdgeDescription{"can_1", 0, "ecu_vehicle", 0});
    m_description.addEdge(EdgeDescription{"ecu_vehicle", 0, "can_1", 0});
    m_description.addEdge(EdgeDescription{"can_1", 0, "tinyml_ecu", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 0, "can_1", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 1, "tinyml_plot", 0});

    m_model->reload();
    m_chipLines.clear();
    refreshTelemetryCache();
    updateHudLabels();

    arrangeBlocks();

    const QtNodes::NodeId tinymlCanvasId = m_model->canvasId("tinyml_ecu");
    if (tinymlCanvasId != QtNodes::InvalidNodeId) {
        if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(tinymlCanvasId);
            object != nullptr) {
            m_scene->clearSelection();
            object->setSelected(true);
        }
        Q_EMIT nodeSelected(QStringLiteral("tinyml_ecu"));
    }

    // Replacing the whole pipeline is the most a button here does to what a person made: it can be
    // taken back, whole.
    m_scene->undoStack().clear();
    m_scene->undoStack().push(
        new CanvasActionCommand{tr("Build TinyML Demo"),
                                [this, before] { restoreGraph(before); },
                                [this, after = m_description] { restoreGraph(after); }});

    Q_EMIT graphEdited();
}

void CanvasPanel::connectPorts(QtNodes::NodeId from, QtNodes::NodeId to)
{
    connectPortIndex(from, 0, to, 0);
}

void CanvasPanel::connectPortIndex(QtNodes::NodeId from,
                                   QtNodes::PortIndex fromPort,
                                   QtNodes::NodeId to,
                                   QtNodes::PortIndex toPort)
{
    if (!m_model) {
        return;
    }

    const QtNodes::ConnectionId connection{from, fromPort, to, toPort};
    if (m_model->connectionPossible(connection)) {
        m_model->addConnection(connection);
    }
}

QtNodes::NodeId CanvasPanel::addNodeAtReturning(const QString& typeName,
                                                const QPointF& scenePosition)
{
    if (!m_model) {
        return QtNodes::InvalidNodeId;
    }

    const QtNodes::NodeId nodeId = m_model->addNode(typeName);
    if (nodeId == QtNodes::InvalidNodeId) {
        return QtNodes::InvalidNodeId;
    }

    m_model->setNodeData(nodeId, QtNodes::NodeRole::Position, scenePosition);
    return nodeId;
}

void CanvasPanel::reload()
{
    // A command in the history names blocks by the ids this model hands out, and it is about to
    // hand them out again.
    if (m_scene != nullptr) {
        m_scene->undoStack().clear();
    }

    if (m_model) {
        m_model->reload();
        m_chipLines.clear();
        refreshTelemetryCache();
        updateHudLabels();
    }

    // The graph is another one, and the view was left where the last one was. Fitted at once when
    // there is a window to fit it to, and otherwise when there is.
    m_fitPending = true;
    if (m_view != nullptr && m_view->isVisible()) {
        fitIfPending();
    }
}

void CanvasPanel::applyStyles(const Theme& theme)
{
    const QString json = styleJson(theme);

    QtNodes::GraphicsViewStyle::setStyle(json);
    QtNodes::NodeStyle::setNodeStyle(json);
    QtNodes::ConnectionStyle::setConnectionStyle(json);
}

void CanvasPanel::applyTheme(const Theme& theme)
{
    m_theme = theme;
    applyStyles(theme);

    if (m_view != nullptr) {
        m_view->setBackgroundBrush(theme.canvas);
    }

    if (m_palette != nullptr) {
        QPalette palette = m_palette->palette();
        palette.setColor(QPalette::Base, theme.panel);
        palette.setColor(QPalette::Window, theme.panel);
        m_palette->setPalette(palette);
        applyPaletteIcons();
    }

    applyNavigationIcons();
    applyHudIcons();

    // The application's toolbar rule puts the edge at the bottom, which is where a horizontal bar
    // meets what it is above; this one stands beside the canvas.
    if (m_navToolBar != nullptr) {
        m_navToolBar->setStyleSheet(
            QStringLiteral("QToolBar { background-color: %1; border: none; "
                           "border-right: 1px solid %2; padding: 4px 3px; spacing: 2px; }")
                .arg(theme.toolbar.name(QColor::HexRgb), theme.border.name(QColor::HexRgb)));
    }

    // The bar above the canvas is the same surface as the strip beside it, so the two read as one
    // frame round the canvas rather than as two bars. Its buttons are tighter than a dialog's (the
    // application rule gives every button 68 px and 14 px of padding), and one that is switched on
    // says so: Animate Flow starts on and looked the same as Inject TinyML Anomaly, which starts
    // off.
    if (m_hudBar != nullptr) {
        m_hudBar->setStyleSheet(
            QStringLiteral("QWidget[hudBar=\"true\"] { background-color: %1; "
                           "border-bottom: 1px solid %2; }"
                           "QWidget[hudBar=\"true\"] QPushButton { padding: 3px 8px 3px 6px; "
                           "min-width: 0px; }"
                           "QWidget[hudBar=\"true\"] QPushButton:checked { background-color: %3; "
                           "border-color: %4; }"
                           "QFrame[hudRule=\"true\"] { background-color: %2; border: none; }")
                .arg(theme.toolbar.name(QColor::HexRgb),
                     theme.border.name(QColor::HexRgb),
                     theme.selection.name(QColor::HexRgb),
                     theme.accent.name(QColor::HexRgb)));

        applyHudSizing();
    }

    updateHudLabels();
    invalidateItems();

    if (m_scene != nullptr) {
        m_scene->update();
    }
    if (m_view != nullptr) {
        m_view->update();
    }
}

// ---------------------------------------------------------------------------
// Foreground Visual Telemetry, Live Frame/DBC Cards & Neural Visualizer
// ---------------------------------------------------------------------------

void CanvasPanel::paintSceneForeground(QPainter* painter, const QRectF& exposed)
{
    if (!m_model || m_scene == nullptr || painter == nullptr) {
        return;
    }

    painter->save();
    painter->setClipping(false);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const qreal zoom = std::max(0.01, painter->worldTransform().m11());
    const bool flowing = m_simulationRunning && m_animationsEnabled;

    const double pulse =
        0.5 + 0.5 * std::sin(static_cast<double>(m_phase) * 2.0 * std::numbers::pi);

    // What moves is all that is drawn here. The blocks and the wires themselves are drawn by their
    // own items, which QtNodes keeps as pictures: drawn again when they change, not on every frame
    // the packets move.
    //
    // Packets are a way to see that a wire is carrying something, and at the zoom where a block is
    // a name and nothing else there is no wire to see them on.
    if (flowing && zoom >= 0.18) {
        for (const EdgeDescription& edge : m_description.edges()) {
            const NodeDescription* fromDesc = m_description.find(edge.fromNode);
            const NodeDescription* toDesc = m_description.find(edge.toNode);
            if (fromDesc == nullptr || toDesc == nullptr || !fromDesc->enabled
                || !toDesc->enabled) {
                continue;
            }

            const QtNodes::NodeId fromId = m_model->canvasId(edge.fromNode);
            const QtNodes::NodeId toId = m_model->canvasId(edge.toNode);
            if (fromId == QtNodes::InvalidNodeId || toId == QtNodes::InvalidNodeId) {
                continue;
            }

            const QtNodes::ConnectionId connId{fromId,
                                               static_cast<QtNodes::PortIndex>(edge.fromPort),
                                               toId,
                                               static_cast<QtNodes::PortIndex>(edge.toPort)};
            QtNodes::ConnectionGraphicsObject* connObj = m_scene->connectionGraphicsObject(connId);
            if (connObj == nullptr || !connObj->sceneBoundingRect().intersects(exposed)) {
                continue;
            }

            const WireShape wire = wireShape(*connObj, *m_model);
            if (wire.path.isEmpty()) {
                continue;
            }

            // What runs along each line the wire is drawn as: one, for a curve between two blocks,
            // and for a drop the two wires of the bus - a light on each, side by side, so that a
            // frame going down a drop is seen going down both.
            struct Track final {
                QPainterPath path;
                QColor colour;
            };
            std::array<Track, 2> tracks;
            std::size_t trackCount = 0;

            if (wire.isTap) {
                tracks[trackCount++] = {connObj->mapToScene(wire.wires[kCanHigh].path),
                                        m_theme.warning};
                tracks[trackCount++] = {connObj->mapToScene(wire.wires[kCanLow].path),
                                        m_theme.success};
            } else {
                PortType wireType = PortType::Frames;
                if (const NodeTypeInfo* info = m_catalog.find(fromDesc->typeName);
                    info != nullptr && edge.fromPort < info->outputs.size()) {
                    wireType = info->outputs[edge.fromPort].type;
                }

                QColor wireColor = m_theme.accent;
                if (wireType == PortType::Signals) {
                    wireColor = m_theme.success;
                } else if (wireType == PortType::Events) {
                    wireColor = m_theme.warning;
                }
                tracks[trackCount++] = {connObj->mapToScene(wire.path), wireColor};
            }

            // A drop is drawn from the module to the bus, and what the bus delivers runs the other
            // way.
            const bool reversed = wire.isTap && !wire.towardBus;
            const int packets = wire.isTap ? 1 : 3;
            const qreal radius = wire.isTap ? 7.0 : 9.0;

            for (std::size_t track = 0; track < trackCount; ++track) {
                const QPainterPath& path = tracks[track].path;
                const QColor wireColor = tracks[track].colour;

                QColor glowColor = wireColor;
                glowColor.setAlpha(static_cast<int>(45 + 35 * pulse));
                painter->setPen(
                    QPen{glowColor, wire.isTap ? 4.0 : 6.0, Qt::SolidLine, Qt::RoundCap});
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(path);

                for (int k = 0; k < packets; ++k) {
                    qreal offset = std::fmod(m_phase + static_cast<qreal>(k) / packets, 1.0);
                    if (reversed) {
                        offset = 1.0 - offset;
                    }
                    const QPointF pt = path.pointAtPercent(offset);

                    QRadialGradient halo{pt, radius};
                    QColor inner = wireColor.lighter(145);
                    inner.setAlpha(240);
                    QColor mid = wireColor;
                    mid.setAlpha(150);
                    QColor outer = wireColor;
                    outer.setAlpha(0);
                    halo.setColorAt(0.0, inner);
                    halo.setColorAt(0.45, mid);
                    halo.setColorAt(1.0, outer);

                    painter->setPen(Qt::NoPen);
                    painter->setBrush(halo);
                    painter->drawEllipse(pt, radius, radius);

                    painter->setBrush(QColor{255, 255, 255, 235});
                    painter->drawEllipse(pt, wire.isTap ? 2.0 : 2.8, wire.isTap ? 2.0 : 2.8);
                }
            }
        }
    }

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QSize nodeSize = m_scene->nodeGeometry().size(nodeId);
        const QRectF nodeRect = ngo->mapRectToScene(QRectF{QPointF{0.0, 0.0}, QSizeF{nodeSize}});

        // The card under an opened TinyML block is part of what the block is, so it is part of
        // what is looked at - and drawn here because it moves, the network lighting up as it runs.
        const bool expanded = m_model->isExpanded(nodeId);
        const QRectF reach = node.typeName == "tinyml.ecu" && expanded
                                 ? nodeRect.united(tinyMlCardRect(nodeRect))
                                 : nodeRect;
        if (!reach.adjusted(-8.0, -8.0, 8.0, 8.0).intersects(exposed)) {
            continue;
        }

        if (node.typeName == "tinyml.ecu" && expanded) {
            const QString idString = QString::fromStdString(node.id);
            const auto statusIt = m_nodeStatuses.constFind(idString);
            const NodeStatus* status =
                (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;
            paintTinyMlVisualizerCard(painter, nodeRect, node, status);
        }

        // The bus has the two wires to run along, and it is the one thing on the canvas that is
        // carrying everything: a light moving along each, so that a running bus is not the same
        // picture as a stopped one.
        if (flowing && m_model->isBus(nodeId) && zoom >= 0.3 && node.enabled) {
            const qreal from = nodeRect.left() + layout::kRailPlateWidth;
            const qreal to = nodeRect.right() - 14.0;
            const qreal middle = nodeRect.center().y();

            for (const qreal side : {-1.0, 1.0}) {
                const QColor wire = side < 0.0 ? m_theme.warning : m_theme.success;
                for (int k = 0; k < 4; ++k) {
                    const qreal t = std::fmod(
                        m_phase + static_cast<qreal>(k) * 0.25 + (side < 0.0 ? 0.0 : 0.125), 1.0);
                    const QPointF pt{from + (to - from) * t,
                                     middle + side * layout::kRailWireOffset};

                    QRadialGradient glow{pt, 8.0};
                    glow.setColorAt(0.0, withAlpha(wire.lighter(150), 230));
                    glow.setColorAt(1.0, withAlpha(wire, 0));
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(glow);
                    painter->drawEllipse(pt, 8.0, 8.0);
                }
            }
            continue;
        }

        // A halo round a block that is running. Where the blocks are chips these are the only
        // part of them that moves.
        if (flowing && node.enabled && zoom >= 0.3 && !m_model->isBus(nodeId)) {
            const bool alert = chipInfo(node).alert;

            QColor haloColor = alert ? m_theme.error : accentForNodeType(node.typeName, m_theme);
            haloColor.setAlpha(static_cast<int>(55 + 65 * pulse));
            painter->setPen(QPen{haloColor, alert ? 2.6 : 1.8});
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(nodeRect.adjusted(-3.0, -3.0, 3.0, 3.0), 9.0, 9.0);
        }
    }

    painter->restore();
}

void CanvasPanel::paintCardBody(QPainter* painter,
                                QtNodes::NodeGraphicsObject& ngo,
                                const QRectF& nodeRect,
                                const NodeDescription& node) const
{
    const QColor tagAccent = accentForNodeType(node.typeName, m_theme);
    const bool faultInjected =
        m_injectFaultButton != nullptr && m_injectFaultButton->isChecked() && m_simulationRunning;

    const QString idString = QString::fromStdString(node.id);
    const auto statusIt = m_nodeStatuses.constFind(idString);
    const NodeStatus* status = (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;

    const bool nodeAlert = chipInfo(node).alert;

    // --- 0. Card Surface & Border -------------------------------------------
    const bool isSelected = ngo.isSelected();
    QColor cardBg = m_theme.panel;
    cardBg.setAlpha(248);

    QColor borderColor = isSelected ? m_theme.accent : m_theme.border;
    qreal borderWidth = isSelected ? 2.0 : 1.25;
    if (m_simulationRunning && node.enabled) {
        borderColor = nodeAlert ? m_theme.error : (isSelected ? m_theme.accent : tagAccent);
        borderWidth = isSelected ? 2.2 : 1.6;
    }

    painter->setPen(QPen{borderColor, borderWidth});
    painter->setBrush(cardBg);
    painter->drawRoundedRect(nodeRect, 6.0, 6.0);

    // --- 1. Header Banner (top 24px) ----------------------------------------
    const QRectF headerRect{
        nodeRect.left() + 1.0, nodeRect.top() + 1.0, nodeRect.width() - 2.0, 23.0};
    QColor headerTint = nodeAlert ? m_theme.error : tagAccent;
    headerTint.setAlpha(isSelected ? 48 : 30);

    QPainterPath headerPath;
    headerPath.addRoundedRect(headerRect, 5.0, 5.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(headerTint);
    painter->drawPath(headerPath);

    // Crisp colored top accent stripe
    const QRectF topStripeRect{
        nodeRect.left() + 1.5, nodeRect.top() + 1.0, nodeRect.width() - 3.0, 2.5};
    QPainterPath topStripePath;
    topStripePath.addRoundedRect(topStripeRect, 1.2, 1.2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(nodeAlert ? m_theme.error : tagAccent);
    painter->drawPath(topStripePath);

    painter->setPen(QPen{m_theme.border, 1.0});
    painter->drawLine(QPointF{nodeRect.left() + 1.0, nodeRect.top() + 24.0},
                      QPointF{nodeRect.right() - 1.0, nodeRect.top() + 24.0});

    // Status LED dot on header left.
    const QColor ledColor =
        !node.enabled
            ? m_theme.textMuted
            : (m_simulationRunning ? (nodeAlert ? m_theme.error : m_theme.success) : tagAccent);

    painter->setPen(Qt::NoPen);
    painter->setBrush(ledColor);
    painter->drawEllipse(QPointF{nodeRect.left() + 11.0, nodeRect.top() + 12.5}, 3.6, 3.6);

    // Type pill in header right.
    const QFontMetricsF badgeMetrics{m_badgeFont};

    const QString typeTag = shortTypeBadge(node.typeName);
    const qreal tagWidth = badgeMetrics.horizontalAdvance(typeTag) + 12.0;
    const QRectF tagRect{nodeRect.right() - tagWidth - 28.0, nodeRect.top() + 4.5, tagWidth, 15.0};

    // The button that closes the card back to a chip, in the corner the pill used to have.
    {
        const QRectF collapse = layout::cardCollapseButton(nodeRect);
        const QPointF c = collapse.center();

        painter->setPen(QPen{m_theme.textMuted, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
        painter->setBrush(Qt::NoBrush);
        QPainterPath chevron;
        chevron.moveTo(c + QPointF{-3.6, 1.8});
        chevron.lineTo(c + QPointF{0.0, -1.8});
        chevron.lineTo(c + QPointF{3.6, 1.8});
        painter->drawPath(chevron);
    }

    QColor tagFill = tagAccent;
    tagFill.setAlpha(45);
    QColor tagBorder = tagAccent;
    tagBorder.setAlpha(165);

    painter->setFont(m_badgeFont);
    painter->setPen(QPen{tagBorder, 1.0});
    painter->setBrush(tagFill);
    painter->drawRoundedRect(tagRect, 3.5, 3.5);
    painter->setPen(accentOnSurface(tagAccent, m_theme, 135));
    painter->drawText(tagRect, Qt::AlignCenter, typeTag);

    const bool isScriptNode = isScriptNodeType(node.typeName);
    const QString idLabel = idString;
    const bool isNodeScriptHovered = m_hoveringScriptEdit && (m_hoveredScriptNodeId == idLabel);

    QRectF headerEditRect;
    if (isScriptNode) {
        headerEditRect = QRectF{tagRect.left() - 72.0, nodeRect.top() + 4.0, 66.0, 16.0};

        QColor btnFill = tagAccent;
        btnFill.setAlpha(isNodeScriptHovered ? 80 : 40);
        QColor btnBorder = tagAccent.lighter(isNodeScriptHovered ? 140 : 110);
        btnBorder.setAlpha(isNodeScriptHovered ? 255 : 170);

        painter->setFont(m_badgeFont);
        painter->setPen(QPen{btnBorder, isNodeScriptHovered ? 1.3 : 1.0});
        painter->setBrush(btnFill);
        painter->drawRoundedRect(headerEditRect, 3.5, 3.5);

        const QRectF iconRect{headerEditRect.left() + 5.0, headerEditRect.top() + 2.0, 12.0, 12.0};
        if (ThemeManager* tm = ThemeManager::instance()) {
            const QIcon editIco =
                tm->icon(QStringLiteral("edit"), accentOnSurface(tagAccent, m_theme, 145));
            editIco.paint(painter, iconRect.toRect());
        }

        painter->setPen(accentOnSurface(tagAccent, m_theme, 145));
        const QRectF textRect{iconRect.right() + 4.0,
                              headerEditRect.top(),
                              headerEditRect.width() - 21.0,
                              headerEditRect.height()};
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, tr("Script"));
    }

    // Node ID + Subtitle in header left.
    painter->setFont(m_titleFont);
    painter->setPen(m_theme.text);

    const QString subLabel = nodeSubtitle(node);
    const QFontMetricsF titleMetrics{m_titleFont};
    const qreal idWidth = titleMetrics.horizontalAdvance(idLabel);

    const qreal rightLimit = isScriptNode ? (headerEditRect.left() - 8.0) : (tagRect.left() - 8.0);
    const QRectF titleArea{nodeRect.left() + 19.0,
                           nodeRect.top() + 2.0,
                           std::max<qreal>(30.0, rightLimit - (nodeRect.left() + 19.0)),
                           20.0};
    painter->drawText(titleArea, Qt::AlignLeft | Qt::AlignVCenter, idLabel);

    if (!subLabel.isEmpty() && idWidth + 14.0 < titleArea.width()) {
        painter->setFont(m_subFont);
        painter->setPen(m_theme.textMuted);
        const QRectF subRect{titleArea.left() + idWidth + 6.0,
                             titleArea.top(),
                             titleArea.width() - idWidth - 6.0,
                             titleArea.height()};
        painter->drawText(
            subRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetricsF{m_subFont}.elidedText(subLabel, Qt::ElideRight, subRect.width()));
    }

    // --- 1. Live Frame Inspection + 8-Byte Hex Grid -------------------------
    const auto cachedIt = m_telemetryCache.constFind(idLabel);
    const NodeFrameTelemetry fallbackTelemetry =
        (cachedIt == m_telemetryCache.cend()) ? computeNodeTelemetry(node, status, faultInjected)
                                              : NodeFrameTelemetry{};
    const NodeFrameTelemetry& telemetry =
        (cachedIt != m_telemetryCache.cend()) ? cachedIt.value() : fallbackTelemetry;

    // Frame header strip
    const QRectF frameHeaderRect{nodeRect.left() + 10.0,
                                 nodeRect.top() + layout::kCardFrameTop,
                                 nodeRect.width() - 20.0,
                                 16.0};

    // Direction tag pill (LIVE TX / LIVE RX / STANDBY / PASS)
    QColor dirColor = telemetry.hasLiveFrame ? tagAccent : m_theme.textMuted;
    if (nodeAlert) {
        dirColor = m_theme.error;
    }
    const qreal dirW = badgeMetrics.horizontalAdvance(telemetry.directionTag) + 8.0;
    const QRectF dirRect{frameHeaderRect.left(), frameHeaderRect.top() + 1.0, dirW, 14.0};
    QColor dirBg = dirColor;
    dirBg.setAlpha(38);
    painter->setFont(m_badgeFont);
    painter->setPen(QPen{dirColor, 0.9});
    painter->setBrush(dirBg);
    painter->drawRoundedRect(dirRect, 3.0, 3.0);
    painter->setPen(accentOnSurface(dirColor, m_theme, 130));
    painter->drawText(dirRect, Qt::AlignCenter, telemetry.directionTag);

    // CAN ID + DLC in monospace
    painter->setFont(m_monoBoldFont);
    painter->setPen(m_theme.text);
    const QRectF idTextRect{
        dirRect.right() + 6.0, frameHeaderRect.top(), frameHeaderRect.width() - dirW - 6.0, 16.0};
    painter->drawText(idTextRect, Qt::AlignLeft | Qt::AlignVCenter, telemetry.idDlcText);

    // Message name on the right of the frame header
    if (!telemetry.messageName.isEmpty()) {
        painter->setFont(m_subFont);
        painter->setPen(accentOnSurface(tagAccent, m_theme, 125));
        const QFontMetricsF msgMetrics{m_subFont};
        const QString elidedMsg =
            msgMetrics.elidedText(telemetry.messageName, Qt::ElideRight, 106.0);
        painter->drawText(frameHeaderRect, Qt::AlignRight | Qt::AlignVCenter, elidedMsg);
    }

    // 8-Byte Hex Payload Grid (B0..B7)
    const qreal gridLeft = nodeRect.left() + 10.0;
    const qreal gridWidth = nodeRect.width() - 20.0;
    const qreal cellGap = 2.5;
    const qreal cellWidth = (gridWidth - cellGap * 7.0) / 8.0;
    const qreal cellTop = nodeRect.top() + layout::kCardGridTop;
    const qreal cellHeight = layout::kCardGridCellHeight;

    static const std::array<QString, 8> kByteLabels{
        QStringLiteral("B0"),
        QStringLiteral("B1"),
        QStringLiteral("B2"),
        QStringLiteral("B3"),
        QStringLiteral("B4"),
        QStringLiteral("B5"),
        QStringLiteral("B6"),
        QStringLiteral("B7"),
    };

    for (std::size_t b = 0; b < 8; ++b) {
        const qreal cellX = gridLeft + static_cast<qreal>(b) * (cellWidth + cellGap);
        const QRectF cellRect{cellX, cellTop, cellWidth, cellHeight};

        const bool byteValid = b < telemetry.frame.length;
        const bool byteChanged =
            telemetry.hasLiveFrame && ((telemetry.changedMask & (1ULL << b)) != 0);

        QColor cellBg = m_theme.canvas;
        cellBg.setAlpha(byteValid ? 235 : 130);
        QColor cellBorder = m_theme.border;

        if (byteChanged) {
            cellBg = tagAccent;
            cellBg.setAlpha(48);
            cellBorder = tagAccent.lighter(120);
        } else if (telemetry.hasLiveFrame && byteValid) {
            cellBorder = tagAccent;
            cellBorder.setAlpha(110);
        }

        painter->setPen(QPen{cellBorder, byteChanged ? 1.2 : 0.9});
        painter->setBrush(cellBg);
        painter->drawRoundedRect(cellRect, 3.5, 3.5);

        // Top index label: B0..B7
        painter->setFont(m_monoSmallFont);
        painter->setPen(m_theme.textMuted);
        painter->drawText(
            QRectF{cellX, cellTop + 1.5, cellWidth, 11.0}, Qt::AlignCenter, kByteLabels[b]);

        // Bottom hex value: 00..FF or --
        painter->setFont(m_monoBoldFont);
        if (byteValid) {
            painter->setPen(byteChanged ? accentOnSurface(tagAccent, m_theme, 145)
                                        : (telemetry.isPreview ? m_theme.textMuted : m_theme.text));
        } else {
            painter->setPen(m_theme.textMuted);
        }
        painter->drawText(
            QRectF{cellX, cellTop + 13.0, cellWidth, 18.0}, Qt::AlignCenter, telemetry.hexBytes[b]);
    }

    // --- 4. DBC Dictionary / Specs & Decoded Signals Box --------------------
    const QRectF dbcRect{nodeRect.left() + 10.0,
                         nodeRect.top() + layout::kCardDecodeTop,
                         nodeRect.width() - 20.0,
                         layout::kCardDecodeHeight};
    QColor dbcBg = m_theme.canvas;
    dbcBg.setAlpha(220);
    QColor dbcBorder = m_theme.border;
    dbcBorder.setAlpha(180);

    painter->setPen(QPen{dbcBorder, 1.0});
    painter->setBrush(dbcBg);
    painter->drawRoundedRect(dbcRect, 4.5, 4.5);

    // Left vertical accent stripe inside the DBC/Specs box
    painter->setPen(Qt::NoPen);
    painter->setBrush(tagAccent);
    painter->drawRoundedRect(
        QRectF{dbcRect.left() + 3.0, dbcRect.top() + 4.0, 2.5, dbcRect.height() - 8.0}, 1.2, 1.2);

    painter->setFont(m_dbcTitleFont);
    painter->setPen(accentOnSurface(tagAccent, m_theme, 130));

    const QString dictHeader =
        !m_simulationRunning ? QStringLiteral("BLOCK CONFIGURATION & SPECS")
                             : (!m_databases.empty() ? QStringLiteral("DBC DICTIONARY DECODE")
                                                     : QStringLiteral("CAN TELEMETRY DECODE"));
    const qreal headerWidth = isScriptNode ? (dbcRect.width() - 92.0) : (dbcRect.width() - 14.0);
    painter->drawText(QRectF{dbcRect.left() + 9.0, dbcRect.top() + 3.0, headerWidth, 11.0},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      dictHeader);

    if (isScriptNode) {
        const QRectF specsEditRect{dbcRect.right() - 76.0, dbcRect.top() + 2.5, 72.0, 16.0};

        QColor btnFill = tagAccent;
        btnFill.setAlpha(isNodeScriptHovered ? 80 : 36);
        QColor btnBorder = tagAccent.lighter(isNodeScriptHovered ? 140 : 110);
        btnBorder.setAlpha(isNodeScriptHovered ? 255 : 160);

        painter->setFont(m_badgeFont);
        painter->setPen(QPen{btnBorder, isNodeScriptHovered ? 1.3 : 0.9});
        painter->setBrush(btnFill);
        painter->drawRoundedRect(specsEditRect, 3.5, 3.5);

        const QRectF iconRect{specsEditRect.left() + 5.0, specsEditRect.top() + 2.0, 12.0, 12.0};
        if (ThemeManager* tm = ThemeManager::instance()) {
            const QIcon editIco =
                tm->icon(QStringLiteral("edit"), accentOnSurface(tagAccent, m_theme, 145));
            editIco.paint(painter, iconRect.toRect());
        }

        painter->setPen(accentOnSurface(tagAccent, m_theme, 145));
        const QRectF textRect{iconRect.right() + 4.0,
                              specsEditRect.top(),
                              specsEditRect.width() - 21.0,
                              specsEditRect.height()};
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, tr("Edit Lua"));
    } else if (!telemetry.cycleText.isEmpty()) {
        painter->setFont(m_monoSmallFont);
        painter->setPen(m_theme.textMuted);
        painter->drawText(
            QRectF{dbcRect.left() + 9.0, dbcRect.top() + 3.0, dbcRect.width() - 14.0, 11.0},
            Qt::AlignRight | Qt::AlignVCenter,
            telemetry.cycleText);
    }

    painter->setFont(m_monoSignalFont);
    const QFontMetricsF sigMetrics{m_monoSignalFont};
    const qreal textAvailW = dbcRect.width() - 15.0;

    painter->setPen(telemetry.isPreview ? m_theme.textMuted : m_theme.text);
    painter->drawText(QRectF{dbcRect.left() + 9.0, dbcRect.top() + 18.0, textAvailW, 15.0},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      sigMetrics.elidedText(telemetry.decodedLine1, Qt::ElideRight, textAvailW));

    painter->setPen(telemetry.isPreview ? m_theme.textMuted
                                        : accentOnSurface(m_theme.textMuted, m_theme, 125));
    painter->drawText(QRectF{dbcRect.left() + 9.0, dbcRect.top() + 34.0, textAvailW, 15.0},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      sigMetrics.elidedText(telemetry.decodedLine2, Qt::ElideRight, textAvailW));
}

void CanvasPanel::paintTinyMlVisualizerCard(QPainter* painter,
                                            const QRectF& nodeRect,
                                            const NodeDescription& node,
                                            const NodeStatus* status) const
{
    const QRectF cardRect = tinyMlCardRect(nodeRect);

    const bool injectedFault =
        m_injectFaultButton != nullptr && m_injectFaultButton->isChecked() && m_simulationRunning;

    quint64 anomalyPct = counterValue(status, QStringLiteral("Anomaly score (%)"));
    quint64 confidencePct = counterValue(status, QStringLiteral("Confidence (%)"));
    quint64 thermalHealthPct = counterValue(status, QStringLiteral("Thermal health (%)"));
    quint64 regimeCode = counterValue(status, QStringLiteral("Regime class"));
    quint64 inferenceUs = counterValue(status, QStringLiteral("Last inference (us)"));
    quint64 arenaBytes = counterValue(status, QStringLiteral("Arena bytes used"));

    if (!m_simulationRunning || counterValue(status, QStringLiteral("Inferences run")) == 0) {
        confidencePct = 98;
        thermalHealthPct = 96;
        anomalyPct = 4;
        regimeCode = 0;
        arenaBytes = 192;
        inferenceUs = 2;
    } else if (injectedFault) {
        anomalyPct = std::max<quint64>(anomalyPct, 89);
        thermalHealthPct = std::min<quint64>(thermalHealthPct, 28);
        regimeCode = 4;
    }

    const double threshold = node.parameters.real("anomalyThreshold", 65.0);
    const bool isAnomaly =
        m_simulationRunning && (static_cast<double>(anomalyPct) >= threshold || regimeCode == 4);

    const QColor statusColor =
        isAnomaly ? m_theme.error : (regimeCode == 3 ? m_theme.warning : QColor{0x10, 0xB9, 0x81});

    QColor cardBg = m_theme.panelAlternate;
    cardBg.setAlpha(244);
    painter->setPen(
        QPen{isAnomaly ? m_theme.error : statusColor.darker(115), isAnomaly ? 1.6 : 1.1});
    painter->setBrush(cardBg);
    painter->drawRoundedRect(cardRect, 7.0, 7.0);

    // --- Left side: 4-layer Neural Network Topology (6 -> 10 -> 8 -> 7) -----
    const QRectF netRect{
        cardRect.left() + 10.0, cardRect.top() + 10.0, 108.0, cardRect.height() - 28.0};

    constexpr std::array<int, 4> kDisplayLayerNodes{4, 5, 4, 3};
    std::array<std::array<QPointF, 5>, 4> layerPts{};

    for (std::size_t col = 0; col < kDisplayLayerNodes.size(); ++col) {
        const qreal x =
            netRect.left()
            + (netRect.width() * static_cast<qreal>(col)) / (kDisplayLayerNodes.size() - 1);
        const int count = kDisplayLayerNodes[col];
        for (int row = 0; row < count; ++row) {
            const qreal y =
                netRect.top()
                + (netRect.height() * (static_cast<qreal>(row) + 0.5)) / static_cast<qreal>(count);
            layerPts[col][static_cast<std::size_t>(row)] = QPointF{x, y};
        }
    }

    // Draw synapses with travelling activation wave.
    for (std::size_t col = 0; col + 1 < kDisplayLayerNodes.size(); ++col) {
        const double layerWave =
            m_simulationRunning
                ? (0.5
                   + 0.5
                         * std::sin((static_cast<double>(m_phase) - static_cast<double>(col) * 0.22)
                                    * 2.0 * std::numbers::pi))
                : 0.25;

        QColor synColor = statusColor;
        synColor.setAlpha(static_cast<int>(35 + 85 * layerWave));
        painter->setPen(QPen{synColor, 1.0});

        for (int r0 = 0; r0 < kDisplayLayerNodes[col]; ++r0) {
            for (int r1 = 0; r1 < kDisplayLayerNodes[col + 1]; ++r1) {
                painter->drawLine(layerPts[col][static_cast<std::size_t>(r0)],
                                  layerPts[col + 1][static_cast<std::size_t>(r1)]);
            }
        }
    }

    // Draw neurons.
    for (std::size_t col = 0; col < kDisplayLayerNodes.size(); ++col) {
        const double nodeWave =
            m_simulationRunning
                ? (0.5
                   + 0.5
                         * std::sin((static_cast<double>(m_phase) - static_cast<double>(col) * 0.25)
                                    * 2.0 * std::numbers::pi))
                : 0.4;

        for (int row = 0; row < kDisplayLayerNodes[col]; ++row) {
            const QPointF pt = layerPts[col][static_cast<std::size_t>(row)];
            QColor fill = (col == kDisplayLayerNodes.size() - 1) ? statusColor : m_theme.accent;
            if (m_simulationRunning) {
                fill = fill.lighter(static_cast<int>(105 + 35 * nodeWave));
            }
            painter->setPen(QPen{m_theme.panel, 1.0});
            painter->setBrush(fill);
            painter->drawEllipse(pt, 3.6, 3.6);
        }
    }

    // --- Right side: Regime badge + Anomaly Score & Thermal Health meters ---
    const qreal rightLeft = netRect.right() + 14.0;
    const qreal rightWidth = cardRect.right() - rightLeft - 10.0;

    QFont titleFont = painter->font();
    titleFont.setPixelSize(10);
    titleFont.setWeight(QFont::Bold);
    painter->setFont(titleFont);

    const QRectF regimeRect{rightLeft, cardRect.top() + 8.0, rightWidth, 18.0};
    QColor regimeBg = statusColor;
    regimeBg.setAlpha(45);
    painter->setPen(QPen{statusColor, 1.0});
    painter->setBrush(regimeBg);
    painter->drawRoundedRect(regimeRect, 4.0, 4.0);

    painter->setPen(accentOnSurface(statusColor, m_theme, 135));
    painter->drawText(regimeRect.adjusted(6.0, 0.0, -6.0, 0.0),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      QStringLiteral("%1 (%2%)").arg(regimeName(regimeCode)).arg(confidencePct));

    QFont smallFont = painter->font();
    smallFont.setPixelSize(9);
    smallFont.setWeight(QFont::Medium);
    painter->setFont(smallFont);

    const auto drawMeter = [&](qreal y, const QString& label, quint64 pct, const QColor& barColor) {
        painter->setPen(m_theme.textMuted);
        painter->drawText(
            QRectF{rightLeft, y, rightWidth, 12.0}, Qt::AlignLeft | Qt::AlignVCenter, label);
        painter->setPen(m_theme.text);
        painter->drawText(QRectF{rightLeft, y, rightWidth, 12.0},
                          Qt::AlignRight | Qt::AlignVCenter,
                          QStringLiteral("%1%").arg(pct));

        const QRectF trackRect{rightLeft, y + 13.0, rightWidth, 6.0};
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_theme.canvas);
        painter->drawRoundedRect(trackRect, 3.0, 3.0);

        const qreal fillW =
            trackRect.width() * std::clamp(static_cast<qreal>(pct) / 100.0, 0.03, 1.0);
        const QRectF fillRect{trackRect.left(), trackRect.top(), fillW, trackRect.height()};
        painter->setBrush(barColor);
        painter->drawRoundedRect(fillRect, 3.0, 3.0);
    };

    const QColor anomalyBarColor =
        isAnomaly ? m_theme.error : (anomalyPct > 35 ? m_theme.warning : QColor{0x10, 0xB9, 0x81});
    drawMeter(cardRect.top() + 31.0, tr("Anomaly Score"), anomalyPct, anomalyBarColor);

    const QColor healthBarColor = thermalHealthPct < 50
                                      ? m_theme.error
                                      : (thermalHealthPct < 75 ? m_theme.warning : m_theme.accent);
    drawMeter(cardRect.top() + 57.0, tr("Thermal Health"), thermalHealthPct, healthBarColor);

    // Footer: Static Tensor Arena & Quantization telemetry.
    painter->setPen(m_theme.textMuted);
    const QRectF footerRect{
        cardRect.left() + 10.0, cardRect.bottom() - 18.0, cardRect.width() - 20.0, 14.0};
    painter->drawText(
        footerRect,
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("int8 MLP (6→10→8→7) • Arena %1/4096 B • %2 us • ID 0x%3")
            .arg(arenaBytes)
            .arg(inferenceUs)
            .arg(node.parameters.integer("outputCanId", 0x105), 3, 16, QLatin1Char{'0'})
            .toUpper());
}

} // namespace torquebus::ui
