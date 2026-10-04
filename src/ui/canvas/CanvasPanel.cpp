// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/CanvasPanel.h"

#include "ui/canvas/PipelineGraphModel.h"
#include "ui/common/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/DefaultNodePainter.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMap>
#include <QMenu>
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
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <utility>

namespace torquebus::ui {
namespace {

/// The role holding a palette entry's catalog type name.
constexpr int kTypeNameRole = Qt::UserRole + 1;

[[nodiscard]] QString iconNameForCategory(const QString& category)
{
    if (category.compare(QLatin1String("Sources"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("source");
    }
    if (category.compare(QLatin1String("Transforms"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("transform");
    }
    if (category.compare(QLatin1String("Simulation"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("simulation");
    }
    if (category.compare(QLatin1String("Diagnostics"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("diagnostics");
    }
    if (category.compare(QLatin1String("Sinks"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("sink");
    }
    return QStringLiteral("project");
}

[[nodiscard]] QString iconNameForNodeType(const QString& typeName)
{
    if (typeName == QLatin1String("can.source")) {
        return QStringLiteral("hardware");
    }
    if (typeName == QLatin1String("transmit.list")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("log.source")) {
        return QStringLiteral("replay");
    }
    if (typeName == QLatin1String("can.filter")) {
        return QStringLiteral("filter");
    }
    if (typeName == QLatin1String("dbc.decoder")) {
        return QStringLiteral("database");
    }
    if (typeName == QLatin1String("j1939.decoder")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("lua.ecu")) {
        return QStringLiteral("script");
    }
    if (typeName == QLatin1String("sim.restbus")) {
        return QStringLiteral("restbus");
    }
    if (typeName == QLatin1String("lua.test")) {
        return QStringLiteral("test");
    }
    if (typeName == QLatin1String("tinyml.ecu")) {
        return QStringLiteral("neural");
    }
    if (typeName == QLatin1String("uds.client")) {
        return QStringLiteral("diagnostics");
    }
    if (typeName == QLatin1String("isotp.transport")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("can.transmit")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("trace.sink")) {
        return QStringLiteral("trace");
    }
    if (typeName == QLatin1String("can.log")) {
        return QStringLiteral("save");
    }
    if (typeName == QLatin1String("signal.plot")) {
        return QStringLiteral("graph");
    }
    return QStringLiteral("hardware");
}

/// Duration of one full cycle of packet motion along a wire on the Canvas.
constexpr int kFlowCycleDurationMs = 1350;

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

protected:
    void drawForeground(QPainter* painter, const QRectF& rect) override
    {
        QtNodes::BasicGraphicsScene::drawForeground(painter, rect);
        if (m_foregroundPainter) {
            m_foregroundPainter(painter, rect);
        }
    }

private:
    MenuFactory m_factory;
    ForegroundPainter m_foregroundPainter;
};

/// Custom QtNodes painter that draws the node body rectangle and port
/// connection points while delegating all interior typography (header bar,
/// 8-byte CAN frame grid, and DBC dictionary decoded signals) to
/// `CanvasPanel::paintNodeTelemetryCard`.
class PipelineNodePainter final : public QtNodes::DefaultNodePainter {
public:
    void paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const override
    {
        drawNodeRect(painter, ngo);
        drawConnectionPoints(painter, ngo);
        drawFilledConnectionPoints(painter, ngo);
        drawResizeRect(painter, ngo);
        drawValidationIcon(painter, ngo);
    }
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
    connection["PointDiameter"] = 9.0;
    connection["UseDataDefinedColors"] = false;

    QJsonObject root;
    root["GraphicsViewStyle"] = view;
    root["NodeStyle"] = node;
    root["ConnectionStyle"] = connection;

    return QString::fromUtf8(QJsonDocument{root}.toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString shortTypeBadge(std::string_view typeName)
{
    if (typeName == "can.source") {
        return QStringLiteral("CAN RX");
    }
    if (typeName == "can.transmit") {
        return QStringLiteral("CAN TX");
    }
    if (typeName == "lua.ecu") {
        return QStringLiteral("LUA ECU");
    }
    if (typeName == "tinyml.ecu") {
        return QStringLiteral("TINYML AI");
    }
    if (typeName == "dbc.decoder") {
        return QStringLiteral("DBC");
    }
    if (typeName == "signal.plot") {
        return QStringLiteral("PLOT");
    }
    if (typeName == "trace.sink") {
        return QStringLiteral("TRACE");
    }
    if (typeName == "can.filter") {
        return QStringLiteral("FILTER");
    }
    if (typeName == "uds.client") {
        return QStringLiteral("UDS");
    }
    if (typeName == "isotp.transport") {
        return QStringLiteral("ISO-TP");
    }
    if (typeName == "j1939.decoder") {
        return QStringLiteral("J1939");
    }
    if (typeName == "sim.restbus") {
        return QStringLiteral("RESTBUS");
    }
    if (typeName == "lua.test") {
        return QStringLiteral("TEST SEQ");
    }
    if (typeName == "transmit.list") {
        return QStringLiteral("TX LIST");
    }
    if (typeName == "log.source") {
        return QStringLiteral("LOG PLAY");
    }
    if (typeName == "can.log") {
        return QStringLiteral("LOG REC");
    }
    return QStringLiteral("NODE");
}

[[nodiscard]] QColor accentForNodeType(std::string_view typeName, const Theme& theme)
{
    if (typeName == "tinyml.ecu") {
        return QColor{0x10, 0xB9, 0x81}; // Emerald AI accent
    }
    if (typeName == "lua.ecu" || typeName == "sim.restbus" || typeName == "lua.test") {
        return QColor{0x8B, 0x5C, 0xF6}; // Violet simulation accent
    }
    if (typeName == "dbc.decoder" || typeName == "j1939.decoder" || typeName == "signal.plot") {
        return theme.success;
    }
    if (typeName == "can.filter" || typeName == "uds.client" || typeName == "isotp.transport") {
        return theme.warning;
    }
    return theme.accent;
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

[[nodiscard]] QString nodeSubtitle(const NodeDescription& node)
{
    if (node.typeName == "can.source" || node.typeName == "can.transmit") {
        const std::int64_t ch = node.parameters.integer("channel", 0);
        return QStringLiteral("CAN %1").arg(ch + 1);
    }
    if (node.typeName == "lua.ecu") {
        const std::int64_t ch = node.parameters.integer("channel", 0);
        const std::int64_t speedId = node.parameters.integer("speed_id", 0x101);
        return QStringLiteral("CAN %1 • 0x%2")
            .arg(ch + 1)
            .arg(speedId, 3, 16, QLatin1Char{'0'})
            .toUpper();
    }
    if (node.typeName == "tinyml.ecu") {
        const std::int64_t outId = node.parameters.integer("outputCanId", 0x105);
        return QStringLiteral("TX 0x%1").arg(outId, 3, 16, QLatin1Char{'0'}).toUpper();
    }
    if (node.typeName == "can.filter") {
        const std::int64_t fromId = node.parameters.integer("from", 0x000);
        const std::int64_t toId = node.parameters.integer("to", 0x7FF);
        return QStringLiteral("0x%1..0x%2")
            .arg(fromId, 3, 16, QLatin1Char{'0'})
            .arg(toId, 3, 16, QLatin1Char{'0'})
            .toUpper();
    }
    if (node.typeName == "dbc.decoder") {
        const std::string path = node.parameters.text("database");
        if (!path.empty()) {
            const QString qpath = QString::fromStdString(path);
            const qsizetype slash =
                std::max(qpath.lastIndexOf(QLatin1Char('/')), qpath.lastIndexOf(QLatin1Char('\\')));
            return slash >= 0 ? qpath.mid(slash + 1) : qpath;
        }
        return QStringLiteral("DBC Dictionary");
    }
    if (node.typeName == "signal.plot") {
        return QStringLiteral("Time-Series Sink");
    }
    return {};
}

[[nodiscard]] bool frameMatchesNode(const CanFrame& frame, const NodeDescription& node)
{
    if (frame.rtr || frame.error) {
        return false;
    }

    const std::int64_t nodeChannel = node.parameters.integer("channel", -1);
    if (nodeChannel >= 0 && static_cast<std::int64_t>(frame.channel) != nodeChannel) {
        return false;
    }

    if (node.typeName == "can.filter") {
        const auto fromId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("from", 0)));
        const auto toId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("to", 0x1FFFFFFF)));
        return frame.identifier >= fromId && frame.identifier <= toId;
    }

    if (node.typeName == "tinyml.ecu") {
        const auto outId = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, node.parameters.integer("outputCanId", 0x105)));
        return frame.identifier == outId;
    }

    if (node.typeName == "lua.ecu") {
        const std::int64_t speedId = node.parameters.integer("speed_id", -1);
        const std::int64_t tempId = node.parameters.integer("temp_id", -1);
        if (speedId >= 0 || tempId >= 0) {
            return static_cast<std::int64_t>(frame.identifier) == speedId
                   || static_cast<std::int64_t>(frame.identifier) == tempId;
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
            const std::int64_t outId = node.parameters.integer("outputCanId", 0x105);
            const double threshold = node.parameters.real("anomalyThreshold", 65.0);
            out.messageName = QStringLiteral("TinyML_Virtual_ECU");
            out.decodedLine1 = QStringLiteral("Model: 1D-CNN + TFLM Int8  •  TX ID: 0x%1")
                                   .arg(outId, 3, 16, QLatin1Char{'0'})
                                   .toUpper();
            out.decodedLine2 = QStringLiteral("Anomaly Threshold: %1%  •  Arena: 16 KB Static")
                                   .arg(threshold, 0, 'f', 0);
            return;
        }
        if (node.typeName == "can.filter") {
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
        out.messageName =
            QStringLiteral("J1939 PGN %1 (SA 0x%2)").arg(pgn).arg(sa, 2, 16, QLatin1Char{'0'});
        if (pgn == 61444 && out.frame.length >= 5) {
            const std::uint16_t rawRpm =
                static_cast<std::uint16_t>(d[3]) | (static_cast<std::uint16_t>(d[4]) << 8U);
            out.decodedLine1 =
                QStringLiteral("EEC1 EngineSpeed: %1 rpm").arg(rawRpm * 0.125, 0, 'f', 0);
            out.decodedLine2 =
                QStringLiteral("DriverDemandTorque: %1%").arg(static_cast<int>(d[1]) - 125);
        } else {
            out.decodedLine1 =
                QStringLiteral("PGN: 0x%1 (%2)").arg(pgn, 4, 16, QLatin1Char{'0'}).arg(pgn);
            out.decodedLine2 = QStringLiteral("Source Addr: 0x%1  •  Pri: %2")
                                   .arg(sa, 2, 16, QLatin1Char{'0'})
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
            if (stats.count == 0 || !frameMatchesNode(stats.lastFrame, node)) {
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

            result.hasLiveFrame = true;
            result.frame.channel = static_cast<std::uint8_t>(
                std::max<std::int64_t>(0, node.parameters.integer("channel", 0)));
            result.frame.identifier = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, node.parameters.integer("outputCanId", 0x105)));
            result.frame.format = CanFrameFormat::Standard;
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
            result.frame.identifier = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, node.parameters.integer("outputCanId", 0x105)));
            result.frame.data = {1, 98, 4, 96, 0, 2, 0, 0};
        } else if (node.typeName == "lua.ecu") {
            result.frame.identifier = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, node.parameters.integer("speed_id", 0x101)));
            result.frame.data = {0xE8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 74.4 km/h
        } else if (node.typeName == "can.filter") {
            result.frame.identifier = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, node.parameters.integer("from", 0x101)));
            result.frame.data = {0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // 40.0 km/h
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
        applyStyles(m_theme);
    }

    auto* scene = new CanvasScene{
        *m_model,
        [this](QPointF scenePosition) { return buildSceneMenu(scenePosition); },
        [this](QPainter* painter, const QRectF& rect) { paintSceneForeground(painter, rect); },
        this};

    m_scene = scene;
    m_scene->setNodePainter(std::make_unique<PipelineNodePainter>());

    m_view = new QtNodes::GraphicsView{m_scene};
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                           | QPainter::SmoothPixmapTransform);
    m_view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
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
           "'+ Add Block'."));

    buildPalette();

    // --- Block Editing & TinyML Control Bar above Canvas (right-aligned) ---
    m_hudBar = new QWidget{this};
    m_hudBar->setObjectName(QStringLiteral("torquebus.canvas.hud"));
    auto* hudLayout = new QHBoxLayout{m_hudBar};
    hudLayout->setContentsMargins(10, 5, 10, 5);
    hudLayout->setSpacing(8);

    m_addBlockButton = new QPushButton{tr("+ Add Block"), m_hudBar};
    m_addBlockButton->setToolTip(
        tr("Add the block selected in the left palette (or choose from a menu) at a free "
           "position on the Canvas."));

    m_deleteBlockButton = new QPushButton{tr("Delete Selected"), m_hudBar};
    m_deleteBlockButton->setToolTip(
        tr("Delete the selected block(s) or connection wire(s) from the Pipeline (Del / "
           "Backspace)."));

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

    m_demoPipelineButton = new QPushButton{tr("TinyML Demo Setup"), m_hudBar};
    m_demoPipelineButton->setToolTip(
        tr("Populate the Canvas with a complete Virtual Vehicle ECU + TinyML Virtual ECU + "
           "Signal Plot + CAN Trace simulation pipeline."));

    hudLayout->addStretch(1);
    hudLayout->addWidget(m_addBlockButton);
    hudLayout->addWidget(m_deleteBlockButton);
    hudLayout->addWidget(m_animateButton);
    hudLayout->addWidget(m_injectFaultButton);
    hudLayout->addWidget(m_demoPipelineButton);

    auto* rightPane = new QWidget{this};
    auto* rightLayout = new QVBoxLayout{rightPane};
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_hudBar);
    rightLayout->addWidget(m_view, 1);

    m_splitter = new QSplitter{Qt::Horizontal, this};
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.canvas"));
    m_splitter->addWidget(m_palette);
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
    if (isVisible() && m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

bool CanvasPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event == nullptr || m_view == nullptr) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_view->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
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
                    const QPointF scenePos =
                        m_view->mapToScene(dropEvent->position().toPoint()) - QPointF{155.0, 98.0};
                    addNodeAt(typeName, findNonOverlappingPosition(scenePos));
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
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    }
}

void CanvasPanel::setChannelStatuses(const QList<ChannelStatus>& channels)
{
    m_channelStatuses = channels;
    updateHudLabels();
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

    delete m_scene;
    m_scene = nullptr;

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

QPointF CanvasPanel::findNonOverlappingPosition(const QPointF& desired) const
{
    constexpr qreal kMinDx = 340.0;
    constexpr qreal kMinDy = 220.0;

    QPointF candidate = desired;
    for (int attempt = 0; attempt < 36; ++attempt) {
        bool overlaps = false;
        for (const NodeDescription& existing : m_description.nodes()) {
            const qreal requiredDy =
                (existing.typeName == "tinyml.ecu") ? (kMinDy + 125.0) : kMinDy;
            if (std::abs(existing.x - candidate.x()) < kMinDx
                && std::abs(existing.y - candidate.y()) < requiredDy) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            return candidate;
        }

        const int col = (attempt + 1) % 3;
        const int row = (attempt + 1) / 3;
        candidate = desired + QPointF{col * 350.0, row * 230.0};
    }

    return candidate;
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

    const QPointF viewCenter =
        m_view->mapToScene(m_view->viewport()->rect().center()) - QPointF{140.0, 65.0};
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

    const QPointF center =
        m_view->mapToScene(m_view->viewport()->rect().center()) - QPointF{140.0, 65.0};
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

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(nodeId);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(nodeId)));
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
            addNodeAt(typeName, scenePosition);
        });
    }

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

void CanvasPanel::showNodeMenu(QtNodes::NodeId nodeId, const QPointF& scenePosition)
{
    if (!m_model || m_view == nullptr) {
        return;
    }

    const std::string descriptionId = m_model->descriptionId(nodeId);
    if (descriptionId.empty()) {
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

    constexpr qreal kStepX = 360.0;

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

    constexpr qreal kStepX = 360.0;
    constexpr qreal kStepY = 220.0;

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
            findNonOverlappingPosition(sourcePosition + QPointF{2.0 * kStepX, kStepY - 60.0}));
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
            findNonOverlappingPosition(sourcePosition + QPointF{2.0 * kStepX, kStepY + 140.0}));
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

    m_description = GraphDescription{};

    NodeDescription can1;
    can1.id = "can_1";
    can1.typeName = "can.source";
    can1.parameters.set("channel", ParameterValue::fromInteger(0));
    can1.x = -440.0;
    can1.y = -60.0;
    m_description.addNode(std::move(can1));

    NodeDescription vehicleEcu;
    vehicleEcu.id = "ecu_vehicle";
    vehicleEcu.typeName = "lua.ecu";
    vehicleEcu.parameters.set("channel", ParameterValue::fromInteger(0));
    vehicleEcu.parameters.set("script",
                              ParameterValue::fromText(defaultVehicleEcuScript().toStdString()));
    vehicleEcu.parameters.set("speed_id", ParameterValue::fromInteger(0x101));
    vehicleEcu.parameters.set("temp_id", ParameterValue::fromInteger(0x102));
    vehicleEcu.x = -40.0;
    vehicleEcu.y = -190.0;
    m_description.addNode(std::move(vehicleEcu));

    NodeDescription tx1;
    tx1.id = "tx_bus";
    tx1.typeName = "can.transmit";
    tx1.parameters.set("channel", ParameterValue::fromInteger(0));
    tx1.x = 360.0;
    tx1.y = -190.0;
    m_description.addNode(std::move(tx1));

    NodeDescription tinymlEcu;
    tinymlEcu.id = "tinyml_ecu";
    tinymlEcu.typeName = "tinyml.ecu";
    tinymlEcu.parameters.set("channel", ParameterValue::fromInteger(0));
    tinymlEcu.parameters.set("speedCanId", ParameterValue::fromInteger(0x101));
    tinymlEcu.parameters.set("tempCanId", ParameterValue::fromInteger(0x102));
    tinymlEcu.parameters.set("outputCanId", ParameterValue::fromInteger(0x105));
    tinymlEcu.parameters.set("anomalyThreshold", ParameterValue::fromReal(65.0));
    tinymlEcu.x = -40.0;
    tinymlEcu.y = 60.0;
    m_description.addNode(std::move(tinymlEcu));

    NodeDescription txTinyMl;
    txTinyMl.id = "tx_tinyml";
    txTinyMl.typeName = "can.transmit";
    txTinyMl.parameters.set("channel", ParameterValue::fromInteger(0));
    txTinyMl.x = 360.0;
    txTinyMl.y = 40.0;
    m_description.addNode(std::move(txTinyMl));

    NodeDescription plotTinyMl;
    plotTinyMl.id = "tinyml_plot";
    plotTinyMl.typeName = "signal.plot";
    plotTinyMl.x = 360.0;
    plotTinyMl.y = 260.0;
    m_description.addNode(std::move(plotTinyMl));

    m_description.addEdge(EdgeDescription{"can_1", 0, "ecu_vehicle", 0});
    m_description.addEdge(EdgeDescription{"ecu_vehicle", 0, "tx_bus", 0});
    m_description.addEdge(EdgeDescription{"can_1", 0, "tinyml_ecu", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 0, "tx_tinyml", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 1, "tinyml_plot", 0});

    m_model->reload();
    refreshTelemetryCache();
    updateHudLabels();

    const QtNodes::NodeId tinymlCanvasId = m_model->canvasId("tinyml_ecu");
    if (tinymlCanvasId != QtNodes::InvalidNodeId) {
        if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(tinymlCanvasId);
            object != nullptr) {
            m_scene->clearSelection();
            object->setSelected(true);
        }
        Q_EMIT nodeSelected(QStringLiteral("tinyml_ecu"));
    }

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
    if (m_model) {
        m_model->reload();
        refreshTelemetryCache();
        updateHudLabels();
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

    if (m_hudBar != nullptr) {
        m_hudBar->setStyleSheet(
            QStringLiteral("QWidget#torquebus.canvas.hud { background-color: %1; "
                           "border-bottom: 1px solid %2; }")
                .arg(theme.panelAlternate.name(QColor::HexRgb), theme.border.name(QColor::HexRgb)));
    }

    updateHudLabels();

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

void CanvasPanel::paintSceneForeground(QPainter* painter, const QRectF& /*viewportRect*/)
{
    if (!m_model || m_scene == nullptr || painter == nullptr) {
        return;
    }

    painter->save();
    painter->setClipping(false);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const double pulse =
        0.5 + 0.5 * std::sin(static_cast<double>(m_phase) * 2.0 * std::numbers::pi);

    // 1. Paint port-type tinted overlays and animated data packets along wires.
    for (const EdgeDescription& edge : m_description.edges()) {
        const NodeDescription* fromDesc = m_description.find(edge.fromNode);
        const NodeDescription* toDesc = m_description.find(edge.toNode);
        if (fromDesc == nullptr || toDesc == nullptr || !fromDesc->enabled || !toDesc->enabled) {
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
        if (connObj == nullptr) {
            continue;
        }

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

        const QPointF p0 = connObj->mapToScene(connObj->out());
        const QPointF p3 = connObj->mapToScene(connObj->in());
        const auto [c1Local, c2Local] = connObj->pointsC1C2();
        const QPointF c1 = connObj->mapToScene(c1Local);
        const QPointF c2 = connObj->mapToScene(c2Local);

        QPainterPath path{p0};
        path.cubicTo(c1, c2, p3);

        // Distinguish Signals and Events wires by tinting the curve.
        if (wireType != PortType::Frames) {
            QColor tint = wireColor;
            tint.setAlpha(210);
            painter->setPen(QPen{tint, 2.4, Qt::SolidLine, Qt::RoundCap});
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(path);
        }

        if (m_simulationRunning && m_animationsEnabled) {
            QColor glowColor = wireColor;
            glowColor.setAlpha(static_cast<int>(45 + 35 * pulse));
            painter->setPen(QPen{glowColor, 6.0, Qt::SolidLine, Qt::RoundCap});
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(path);

            constexpr int kPacketsPerWire = 3;
            for (int k = 0; k < kPacketsPerWire; ++k) {
                const qreal offset =
                    std::fmod(m_phase + static_cast<qreal>(k) / kPacketsPerWire, 1.0);
                const QPointF pt = path.pointAtPercent(offset);

                QRadialGradient halo{pt, 9.0};
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
                painter->drawEllipse(pt, 9.0, 9.0);

                painter->setBrush(QColor{255, 255, 255, 235});
                painter->drawEllipse(pt, 2.8, 2.8);
            }
        }
    }

    // 2. Paint rich telemetry node cards (header, ports, 8-byte CAN frame grid,
    //    DBC dictionary decoded signals) and TinyML neural cards.
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

        const QString idString = QString::fromStdString(node.id);
        const auto statusIt = m_nodeStatuses.constFind(idString);
        const NodeStatus* status =
            (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;

        paintNodeTelemetryCard(painter, nodeId, ngo, nodeRect, node, status, pulse);

        if (node.typeName == "tinyml.ecu") {
            paintTinyMlVisualizerCard(painter, nodeRect, node, status);
        }
    }

    painter->restore();
}

void CanvasPanel::paintNodeTelemetryCard(QPainter* painter,
                                         QtNodes::NodeId nodeId,
                                         QtNodes::NodeGraphicsObject* ngo,
                                         const QRectF& nodeRect,
                                         const NodeDescription& node,
                                         const NodeStatus* status,
                                         double pulse) const
{
    const QColor tagAccent = accentForNodeType(node.typeName, m_theme);
    const bool faultInjected =
        m_injectFaultButton != nullptr && m_injectFaultButton->isChecked() && m_simulationRunning;

    bool nodeAlert = false;
    QString summaryText;

    if (m_simulationRunning && node.enabled) {
        if (node.typeName == "tinyml.ecu") {
            const quint64 inferences = counterValue(status, QStringLiteral("Inferences run"));
            const quint64 anomalyPct = counterValue(status, QStringLiteral("Anomaly score (%)"));
            const quint64 regimeCode = counterValue(status, QStringLiteral("Regime class"));
            nodeAlert = (anomalyPct >= 65) || (regimeCode == 4) || faultInjected;
            summaryText = QStringLiteral("%1 • %2 inf • %3% anom")
                              .arg(regimeName(faultInjected ? 4 : regimeCode))
                              .arg(inferences)
                              .arg(faultInjected ? std::max<quint64>(anomalyPct, 89) : anomalyPct);
        } else if (status != nullptr && !status->counters.isEmpty()) {
            const NodeCounter& primary = status->counters.front();
            summaryText = QStringLiteral("%1: %2").arg(primary.label).arg(primary.value);
            if (counterValue(status, QStringLiteral("Stopped after repeated errors")) > 0) {
                nodeAlert = true;
            }
        } else if (node.typeName == "can.source") {
            const std::int64_t ch = node.parameters.integer("channel", 0);
            if (ch >= 0 && ch < m_channelStatuses.size()) {
                const ChannelStatus& cs = m_channelStatuses.at(static_cast<qsizetype>(ch));
                summaryText = QStringLiteral("RX %1 (%2/s)")
                                  .arg(cs.rxFrames)
                                  .arg(cs.framesPerSecond, 0, 'f', 0);
            }
        }
    }

    // --- 0. Card Surface & Border -------------------------------------------
    const bool isSelected = ngo->isSelected();
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
    const QRectF tagRect{nodeRect.right() - tagWidth - 6.0, nodeRect.top() + 4.5, tagWidth, 15.0};

    QColor tagFill = tagAccent;
    tagFill.setAlpha(45);
    QColor tagBorder = tagAccent;
    tagBorder.setAlpha(165);

    painter->setFont(m_badgeFont);
    painter->setPen(QPen{tagBorder, 1.0});
    painter->setBrush(tagFill);
    painter->drawRoundedRect(tagRect, 3.5, 3.5);
    painter->setPen(tagAccent.lighter(135));
    painter->drawText(tagRect, Qt::AlignCenter, typeTag);

    // Node ID + Subtitle in header left.
    painter->setFont(m_titleFont);
    painter->setPen(m_theme.text);

    const QString idLabel = QString::fromStdString(node.id);
    const QString subLabel = nodeSubtitle(node);
    const QFontMetricsF titleMetrics{m_titleFont};
    const qreal idWidth = titleMetrics.horizontalAdvance(idLabel);

    const QRectF titleArea{nodeRect.left() + 19.0,
                           nodeRect.top() + 2.0,
                           tagRect.left() - nodeRect.left() - 24.0,
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

    // --- 2. Live Frame Inspection + 8-Byte Hex Grid -------------------------
    // --- 2. Port Badges & Connection Rings on Left and Right Edges ---------
    const auto drawPortBadges = [&](QtNodes::PortType portType,
                                    const std::vector<PortDescriptor>& ports) {
        for (std::size_t idx = 0; idx < ports.size(); ++idx) {
            const QPointF ptScene = m_scene->nodeGeometry().portScenePosition(
                nodeId, portType, static_cast<QtNodes::PortIndex>(idx), ngo->sceneTransform());

            QColor portColor = m_theme.accent;
            QString typeCode = QStringLiteral("Frames");
            if (ports[idx].type == PortType::Signals) {
                portColor = m_theme.success;
                typeCode = QStringLiteral("Signals");
            } else if (ports[idx].type == PortType::Events) {
                portColor = m_theme.warning;
                typeCode = QStringLiteral("Events");
            }

            const QString portName = QString::fromUtf8(
                ports[idx].name.data(), static_cast<qsizetype>(ports[idx].name.size()));
            const QString badgeText =
                (portType == QtNodes::PortType::In)
                    ? QStringLiteral("● %1 [%2]")
                          .arg(portName,
                               (ports[idx].type == PortType::Signals ? QStringLiteral("Sig")
                                                                     : QStringLiteral("Rx")))
                    : QStringLiteral("[%1] %2 ●")
                          .arg((ports[idx].type == PortType::Signals ? QStringLiteral("Sig")
                                                                     : QStringLiteral("Tx")),
                               portName);

            painter->setFont(m_monoSmallFont);
            const QFontMetricsF smMetrics{m_monoSmallFont};
            const qreal badgeW = smMetrics.horizontalAdvance(badgeText) + 12.0;
            const qreal badgeH = 15.0;
            const qreal badgeY = ptScene.y() - badgeH * 0.5;

            QRectF badgeRect;
            if (portType == QtNodes::PortType::In) {
                badgeRect = QRectF{nodeRect.left() + 9.0, badgeY, badgeW, badgeH};
            } else {
                badgeRect = QRectF{nodeRect.right() - badgeW - 9.0, badgeY, badgeW, badgeH};
            }

            QColor badgeBg = portColor;
            badgeBg.setAlpha(32);
            QColor badgeBorder = portColor;
            badgeBorder.setAlpha(150);

            painter->setPen(QPen{badgeBorder, 0.9});
            painter->setBrush(badgeBg);
            painter->drawRoundedRect(badgeRect, 3.5, 3.5);

            painter->setPen(portColor.lighter(135));
            painter->drawText(badgeRect, Qt::AlignCenter, badgeText);

            // Crisp exterior port ring at the border
            painter->setPen(QPen{m_theme.panel, 1.8});
            painter->setBrush(portColor);
            painter->drawEllipse(ptScene, 4.8, 4.8);
        }
    };

    if (const NodeTypeInfo* typeInfo = m_catalog.find(node.typeName); typeInfo != nullptr) {
        drawPortBadges(QtNodes::PortType::In, typeInfo->inputs);
        drawPortBadges(QtNodes::PortType::Out, typeInfo->outputs);
    }

    // --- 3. Live Frame Inspection + 8-Byte Hex Grid -------------------------
    const auto cachedIt = m_telemetryCache.constFind(idLabel);
    const NodeFrameTelemetry fallbackTelemetry =
        (cachedIt == m_telemetryCache.cend()) ? computeNodeTelemetry(node, status, faultInjected)
                                              : NodeFrameTelemetry{};
    const NodeFrameTelemetry& telemetry =
        (cachedIt != m_telemetryCache.cend()) ? cachedIt.value() : fallbackTelemetry;

    // Frame header strip (y = +74 .. +90, completely below all ports)
    const QRectF frameHeaderRect{
        nodeRect.left() + 10.0, nodeRect.top() + 74.0, nodeRect.width() - 20.0, 16.0};

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
    painter->setPen(dirColor.lighter(130));
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
        painter->setPen(tagAccent.lighter(125));
        const QFontMetricsF msgMetrics{m_subFont};
        const QString elidedMsg =
            msgMetrics.elidedText(telemetry.messageName, Qt::ElideRight, 106.0);
        painter->drawText(frameHeaderRect, Qt::AlignRight | Qt::AlignVCenter, elidedMsg);
    }

    // 8-Byte Hex Payload Grid (B0..B7) at y = +94 .. +127
    const qreal gridLeft = nodeRect.left() + 10.0;
    const qreal gridWidth = nodeRect.width() - 20.0;
    const qreal cellGap = 2.5;
    const qreal cellWidth = (gridWidth - cellGap * 7.0) / 8.0;
    const qreal cellTop = nodeRect.top() + 94.0;
    const qreal cellHeight = 33.0;

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
            painter->setPen(byteChanged ? tagAccent.lighter(145)
                                        : (telemetry.isPreview ? m_theme.textMuted : m_theme.text));
        } else {
            painter->setPen(m_theme.textMuted);
        }
        painter->drawText(
            QRectF{cellX, cellTop + 13.0, cellWidth, 18.0}, Qt::AlignCenter, telemetry.hexBytes[b]);
    }

    // --- 4. DBC Dictionary / Specs & Decoded Signals Box (y = +132 .. +186) -
    const QRectF dbcRect{
        nodeRect.left() + 10.0, nodeRect.top() + 132.0, nodeRect.width() - 20.0, 54.0};
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
    painter->setPen(tagAccent.lighter(130));

    const QString dictHeader =
        !m_simulationRunning ? QStringLiteral("BLOCK CONFIGURATION & SPECS")
                             : (!m_databases.empty() ? QStringLiteral("DBC DICTIONARY DECODE")
                                                     : QStringLiteral("CAN TELEMETRY DECODE"));
    painter->drawText(
        QRectF{dbcRect.left() + 9.0, dbcRect.top() + 3.0, dbcRect.width() - 14.0, 11.0},
        Qt::AlignLeft | Qt::AlignVCenter,
        dictHeader);

    if (!telemetry.cycleText.isEmpty()) {
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

    painter->setPen(m_theme.textMuted);
    painter->drawText(QRectF{dbcRect.left() + 9.0, dbcRect.top() + 34.0, textAvailW, 15.0},
                      Qt::AlignLeft | Qt::AlignVCenter,
                      sigMetrics.elidedText(telemetry.decodedLine2, Qt::ElideRight, textAvailW));

    // --- 5. Live Activity Halo & Floating Counter Pill Above Node -----------
    if (m_simulationRunning && node.enabled) {
        if (m_animationsEnabled) {
            QColor haloColor = nodeAlert ? m_theme.error : tagAccent;
            haloColor.setAlpha(static_cast<int>(55 + 65 * pulse));
            painter->setPen(QPen{haloColor, nodeAlert ? 2.6 : 1.8});
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(nodeRect.adjusted(-3.0, -3.0, 3.0, 3.0), 7.0, 7.0);
        }

        if (!summaryText.isEmpty()) {
            const qreal pillW = std::max(110.0, badgeMetrics.horizontalAdvance(summaryText) + 24.0);
            const QRectF pillRect{
                nodeRect.center().x() - pillW * 0.5, nodeRect.top() - 23.0, pillW, 18.0};

            QColor pillBg = m_theme.panelAlternate;
            pillBg.setAlpha(240);
            const QColor pillBorder = nodeAlert ? m_theme.error : tagAccent;

            painter->setFont(m_badgeFont);
            painter->setPen(QPen{pillBorder, 1.1});
            painter->setBrush(pillBg);
            painter->drawRoundedRect(pillRect, 9.0, 9.0);

            QColor dotColor = nodeAlert ? m_theme.error : m_theme.success;
            dotColor.setAlpha(static_cast<int>(160 + 95 * pulse));
            painter->setPen(Qt::NoPen);
            painter->setBrush(dotColor);
            painter->drawEllipse(QPointF{pillRect.left() + 9.0, pillRect.center().y()}, 3.5, 3.5);

            painter->setPen(m_theme.text);
            painter->drawText(pillRect.adjusted(16.0, 0.0, -6.0, 0.0),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              summaryText);
        }
    }
}

void CanvasPanel::paintTinyMlVisualizerCard(QPainter* painter,
                                            const QRectF& nodeRect,
                                            const NodeDescription& node,
                                            const NodeStatus* status) const
{
    const qreal cardWidth = std::max(nodeRect.width(), 310.0);
    const qreal cardHeight = 114.0;
    const QRectF cardRect{
        nodeRect.center().x() - cardWidth * 0.5, nodeRect.bottom() + 8.0, cardWidth, cardHeight};

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

    painter->setPen(statusColor.lighter(135));
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
