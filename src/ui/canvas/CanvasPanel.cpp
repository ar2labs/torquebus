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
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLinearGradient>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QRadialGradient>
#include <QSplitter>
#include <QString>
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

/// Duration of one full cycle of packet motion along a wire on the Canvas.
constexpr int kFlowCycleDurationMs = 1350;

/// Scene subclass that supplies the right-click context menu and delegates
/// foreground painting (animated wire packets, live node badges, and the
/// TinyML neural network card) to `CanvasPanel`.
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
    node["GradientColor0"] = rgb(theme.panel);
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
    node["PenWidth"] = 1.25;
    node["HoveredPenWidth"] = 1.75;
    node["ConnectionPointDiameter"] = 8.0;
    node["Opacity"] = 1.0;

    QJsonObject connection;
    connection["ConstructionColor"] = rgb(theme.textMuted);
    connection["NormalColor"] = rgb(theme.accent);
    connection["SelectedColor"] = rgb(theme.accentHover);
    connection["SelectedHaloColor"] = rgb(theme.accent);
    connection["HoveredColor"] = rgb(theme.accentHover);
    connection["LineWidth"] = 2.2;
    connection["ConstructionLineWidth"] = 2.0;
    connection["PointDiameter"] = 8.0;
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

} // namespace

CanvasPanel::CanvasPanel(GraphDescription& description, const NodeCatalog& catalog, QWidget* parent)
    : QWidget{parent}
    , m_description{description}
    , m_catalog{catalog}
    , m_theme{Theme::dark()}
{
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
    m_view = new QtNodes::GraphicsView{m_scene};
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                           | QPainter::SmoothPixmapTransform);
    m_view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

    connect(m_scene,
            &QtNodes::BasicGraphicsScene::nodeContextMenu,
            this,
            [this](QtNodes::NodeId nodeId, QPointF position) { showNodeMenu(nodeId, position); });

    m_palette = new QTreeWidget;
    m_palette->setHeaderHidden(true);
    m_palette->setRootIsDecorated(true);
    m_palette->setSelectionMode(QAbstractItemView::SingleSelection);
    m_palette->setMinimumWidth(150);
    m_palette->setMaximumWidth(260);

    buildPalette();

    // --- Simulation & TinyML Control Bar above the Canvas view -------------
    m_hudBar = new QWidget{this};
    m_hudBar->setObjectName(QStringLiteral("torquebus.canvas.hud"));
    auto* hudLayout = new QHBoxLayout{m_hudBar};
    hudLayout->setContentsMargins(10, 5, 10, 5);
    hudLayout->setSpacing(8);

    m_hudStatusLabel = new QLabel{m_hudBar};
    m_hudMetricsLabel = new QLabel{m_hudBar};

    m_animateButton = new QPushButton{tr("Animate Flow"), m_hudBar};
    m_animateButton->setCheckable(true);
    m_animateButton->setChecked(true);
    m_animateButton->setToolTip(
        tr("Toggle live 60 FPS wire packet flow, node telemetry halos, and TinyML neural "
           "network animations on the Canvas."));

    m_injectFaultButton = new QPushButton{tr("Inject TinyML Anomaly"), m_hudBar};
    m_injectFaultButton->setCheckable(true);
    m_injectFaultButton->setToolTip(
        tr("Inject simulated powertrain thermal runaway and CAN timing jitter into the "
           "TinyML Virtual ECU (`tinyml.inject_fault`) to observe real-time anomaly detection."));

    m_demoPipelineButton = new QPushButton{tr("TinyML Demo Setup"), m_hudBar};
    m_demoPipelineButton->setToolTip(
        tr("Populate the Canvas with a complete Virtual Vehicle ECU + TinyML Virtual ECU + "
           "Signal Plot + CAN Trace simulation pipeline."));

    hudLayout->addWidget(m_hudStatusLabel);
    hudLayout->addWidget(m_hudMetricsLabel, 1);
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
        m_phase = value.toReal();
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(m_animateButton, &QPushButton::toggled, this, [this](bool checked) {
        m_animationsEnabled = checked;
        updateAnimationState();
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(m_injectFaultButton, &QPushButton::toggled, this, [this](bool checked) {
        if (m_variables != nullptr) {
            m_variables->set("tinyml.inject_fault", checked ? 1.0 : 0.0);
        }
        updateHudLabels();
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    });

    connect(
        m_demoPipelineButton, &QPushButton::clicked, this, [this] { buildTinyMlDemoPipeline(); });

    connect(m_palette, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        addNodeFromPalette(item);
    });

    connect(m_model.get(), &PipelineGraphModel::nodeCreated, this, [this](QtNodes::NodeId) {
        updateHudLabels();
        Q_EMIT graphEdited();
    });
    connect(m_model.get(), &PipelineGraphModel::nodeDeleted, this, [this](QtNodes::NodeId) {
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

void CanvasPanel::setSystemVariables(SystemVariables* variables)
{
    m_variables = variables;
    if (m_variables != nullptr && m_injectFaultButton != nullptr
        && m_injectFaultButton->isChecked()) {
        m_variables->set("tinyml.inject_fault", 1.0);
    }
}

void CanvasPanel::setSimulationRunning(bool running)
{
    m_simulationRunning = running;
    if (running && m_variables != nullptr && m_injectFaultButton != nullptr) {
        m_variables->set("tinyml.inject_fault", m_injectFaultButton->isChecked() ? 1.0 : 0.0);
    }
    updateAnimationState();
    updateHudLabels();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
    }
}

void CanvasPanel::setNodeStatuses(const QList<NodeStatus>& nodes)
{
    m_nodeStatuses.clear();
    m_nodeStatuses.reserve(nodes.size());
    for (const NodeStatus& status : nodes) {
        m_nodeStatuses.insert(status.name, status);
    }
    updateHudLabels();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->update();
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

    const bool shouldRun = m_simulationRunning && m_animationsEnabled;
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
    if (m_hudStatusLabel == nullptr || m_hudMetricsLabel == nullptr) {
        return;
    }

    const bool faultActive = m_injectFaultButton != nullptr && m_injectFaultButton->isChecked();
    const QColor badgeColor =
        !m_simulationRunning ? m_theme.textMuted : (faultActive ? m_theme.error : m_theme.success);

    const QString stateWord = !m_simulationRunning ? tr("SIL Ready (Press F5)")
                                                   : (faultActive ? tr("SIL Live • Fault Injected")
                                                                  : tr("SIL Live • Streaming"));

    m_hudStatusLabel->setText(
        QStringLiteral("<span style='color:%1; font-weight:600;'>&#9679; %2</span>")
            .arg(badgeColor.name(QColor::HexRgb), stateWord));

    double totalFps = 0.0;
    double peakBusLoad = 0.0;
    for (const ChannelStatus& channel : m_channelStatuses) {
        totalFps += channel.framesPerSecond;
        peakBusLoad = std::max(peakBusLoad, channel.busLoadPercent);
    }

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

    QString metrics = tr("%1 blocks • %2 wires")
                          .arg(m_description.nodes().size())
                          .arg(m_description.edges().size());

    if (m_simulationRunning) {
        metrics += tr("   |   %1 frames/s • %2% bus load")
                       .arg(totalFps, 0, 'f', 0)
                       .arg(peakBusLoad, 0, 'f', 1);
        if (!activeRegime.isEmpty()) {
            metrics += tr("   |   TinyML: %1 (%2% anomaly, %3 inferences)")
                           .arg(activeRegime)
                           .arg(maxAnomalyPct)
                           .arg(totalInferences);
        }
    }

    m_hudMetricsLabel->setText(metrics);
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
}

void CanvasPanel::addNodeFromPalette(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }

    if (!m_model || m_view == nullptr) {
        return;
    }

    const QString typeName = item->data(0, kTypeNameRole).toString();
    if (typeName.isEmpty()) {
        return;
    }

    addNodeAt(typeName, m_view->mapToScene(m_view->viewport()->rect().center()));
}

void CanvasPanel::addNodeAt(const QString& typeName, const QPointF& scenePosition)
{
    const QtNodes::NodeId nodeId = addNodeAtReturning(typeName, scenePosition);
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

    QAction* remove = menu.addAction(tr("Delete"));
    connect(remove, &QAction::triggered, this, [this, nodeId] {
        if (m_model) {
            m_model->deleteNode(nodeId);
        }
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

    constexpr qreal kStep = 220.0;

    const QtNodes::NodeId ecu =
        addNodeAtReturning(QStringLiteral("lua.ecu"), sourcePosition + QPointF{kStep, 0.0});
    if (ecu == QtNodes::InvalidNodeId) {
        return;
    }

    QtNodes::NodeId transmit = findNodeOnChannel("can.transmit", channel);
    const bool created = transmit == QtNodes::InvalidNodeId;

    if (created) {
        transmit = addNodeAtReturning(QStringLiteral("can.transmit"),
                                      sourcePosition + QPointF{2.0 * kStep, 0.0});
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

    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(ecu)));
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

    constexpr qreal kStepX = 240.0;
    constexpr qreal kStepY = 170.0;

    const QtNodes::NodeId tinyml =
        addNodeAtReturning(QStringLiteral("tinyml.ecu"), sourcePosition + QPointF{kStepX, kStepY});
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

    const QtNodes::NodeId transmit = addNodeAtReturning(
        QStringLiteral("can.transmit"), sourcePosition + QPointF{2.0 * kStepX, kStepY - 40.0});
    if (transmit != QtNodes::InvalidNodeId) {
        const std::string transmitId = m_model->descriptionId(transmit);
        for (NodeDescription& node : m_description.nodes()) {
            if (node.id == transmitId) {
                node.parameters.set("channel", ParameterValue::fromInteger(channel));
                break;
            }
        }
        connectPortIndex(tinyml, 0, transmit, 0);
    }

    const QtNodes::NodeId plot = addNodeAtReturning(
        QStringLiteral("signal.plot"), sourcePosition + QPointF{2.0 * kStepX, kStepY + 70.0});

    connectPortIndex(sourceNodeId, 0, tinyml, 0);
    if (plot != QtNodes::InvalidNodeId) {
        connectPortIndex(tinyml, 1, plot, 0);
    }

    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(tinyml);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    Q_EMIT nodeSelected(QString::fromStdString(tinymlId));
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
    can1.x = -320.0;
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
    vehicleEcu.y = -130.0;
    m_description.addNode(std::move(vehicleEcu));

    NodeDescription tx1;
    tx1.id = "tx_bus";
    tx1.typeName = "can.transmit";
    tx1.parameters.set("channel", ParameterValue::fromInteger(0));
    tx1.x = 270.0;
    tx1.y = -130.0;
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
    txTinyMl.x = 270.0;
    txTinyMl.y = 20.0;
    m_description.addNode(std::move(txTinyMl));

    NodeDescription plotTinyMl;
    plotTinyMl.id = "tinyml_plot";
    plotTinyMl.typeName = "signal.plot";
    plotTinyMl.x = 270.0;
    plotTinyMl.y = 140.0;
    m_description.addNode(std::move(plotTinyMl));

    m_description.addEdge(EdgeDescription{"can_1", 0, "ecu_vehicle", 0});
    m_description.addEdge(EdgeDescription{"ecu_vehicle", 0, "tx_bus", 0});
    m_description.addEdge(EdgeDescription{"can_1", 0, "tinyml_ecu", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 0, "tx_tinyml", 0});
    m_description.addEdge(EdgeDescription{"tinyml_ecu", 1, "tinyml_plot", 0});

    m_model->reload();
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
// Foreground Visual Telemetry, Animated Wire Packets & Neural Visualizer
// ---------------------------------------------------------------------------

void CanvasPanel::paintSceneForeground(QPainter* painter, const QRectF& /*viewportRect*/)
{
    if (!m_model || m_scene == nullptr || painter == nullptr) {
        return;
    }

    painter->save();
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
            tint.setAlpha(185);
            painter->setPen(QPen{tint, 2.2, Qt::SolidLine, Qt::RoundCap});
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

    // 2. Paint node type pills, live telemetry badges, and TinyML neural cards.
    QFont badgeFont = painter->font();
    badgeFont.setPixelSize(10);
    badgeFont.setWeight(QFont::DemiBold);
    const QFontMetricsF badgeMetrics{badgeFont};

    for (const NodeDescription& node : m_description.nodes()) {
        const QtNodes::NodeId nodeId = m_model->canvasId(node.id);
        if (nodeId == QtNodes::InvalidNodeId) {
            continue;
        }

        QtNodes::NodeGraphicsObject* ngo = m_scene->nodeGraphicsObject(nodeId);
        if (ngo == nullptr) {
            continue;
        }

        const QRectF nodeRect = ngo->sceneBoundingRect().adjusted(2.0, 2.0, -2.0, -2.0);
        const QString idString = QString::fromStdString(node.id);
        const auto statusIt = m_nodeStatuses.constFind(idString);
        const NodeStatus* status =
            (statusIt != m_nodeStatuses.cend()) ? &statusIt.value() : nullptr;

        // Category/Type pill in the upper-right corner of the block.
        const QString typeTag = shortTypeBadge(node.typeName);
        const qreal tagWidth = badgeMetrics.horizontalAdvance(typeTag) + 12.0;
        const QRectF tagRect{
            nodeRect.right() - tagWidth - 6.0, nodeRect.top() + 5.0, tagWidth, 16.0};

        QColor tagAccent = m_theme.accent;
        if (node.typeName == "tinyml.ecu") {
            tagAccent = QColor{0x10, 0xB9, 0x81}; // Emerald AI accent
        } else if (node.typeName == "lua.ecu" || node.typeName == "sim.restbus"
                   || node.typeName == "lua.test") {
            tagAccent = QColor{0x8B, 0x5C, 0xF6}; // Violet simulation accent
        } else if (node.typeName == "dbc.decoder" || node.typeName == "j1939.decoder"
                   || node.typeName == "signal.plot") {
            tagAccent = m_theme.success;
        }

        QColor tagFill = tagAccent;
        tagFill.setAlpha(42);
        QColor tagBorder = tagAccent;
        tagBorder.setAlpha(150);

        painter->setFont(badgeFont);
        painter->setPen(QPen{tagBorder, 1.0});
        painter->setBrush(tagFill);
        painter->drawRoundedRect(tagRect, 4.0, 4.0);

        painter->setPen(tagAccent.lighter(130));
        painter->drawText(tagRect, Qt::AlignCenter, typeTag);

        // Live activity halo and counter badge above active nodes during measurement.
        if (m_simulationRunning && node.enabled) {
            bool nodeAlert = false;
            QString summaryText;

            if (node.typeName == "tinyml.ecu") {
                const quint64 inferences = counterValue(status, QStringLiteral("Inferences run"));
                const quint64 anomalyPct =
                    counterValue(status, QStringLiteral("Anomaly score (%)"));
                const quint64 regimeCode = counterValue(status, QStringLiteral("Regime class"));
                const bool injected =
                    m_injectFaultButton != nullptr && m_injectFaultButton->isChecked();
                nodeAlert = (anomalyPct >= 65) || (regimeCode == 4) || injected;
                summaryText = QStringLiteral("%1 • %2 inf • %3% anom")
                                  .arg(regimeName(injected ? 4 : regimeCode))
                                  .arg(inferences)
                                  .arg(injected ? std::max<quint64>(anomalyPct, 88) : anomalyPct);
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

            if (m_animationsEnabled) {
                QColor haloColor = nodeAlert ? m_theme.error : tagAccent;
                haloColor.setAlpha(static_cast<int>(55 + 65 * pulse));
                painter->setPen(QPen{haloColor, nodeAlert ? 2.6 : 1.8});
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(nodeRect.adjusted(-3.0, -3.0, 3.0, 3.0), 6.0, 6.0);
            }

            if (!summaryText.isEmpty()) {
                const qreal pillW =
                    std::max(110.0, badgeMetrics.horizontalAdvance(summaryText) + 24.0);
                const QRectF pillRect{
                    nodeRect.center().x() - pillW * 0.5, nodeRect.top() - 24.0, pillW, 19.0};

                QColor pillBg = m_theme.panelAlternate;
                pillBg.setAlpha(235);
                const QColor pillBorder = nodeAlert ? m_theme.error : tagAccent;

                painter->setPen(QPen{pillBorder, 1.1});
                painter->setBrush(pillBg);
                painter->drawRoundedRect(pillRect, 9.0, 9.0);

                // Pulsing status dot inside the pill.
                QColor dotColor = nodeAlert ? m_theme.error : m_theme.success;
                dotColor.setAlpha(static_cast<int>(160 + 95 * pulse));
                painter->setPen(Qt::NoPen);
                painter->setBrush(dotColor);
                painter->drawEllipse(
                    QPointF{pillRect.left() + 9.0, pillRect.center().y()}, 3.5, 3.5);

                painter->setPen(m_theme.text);
                painter->drawText(pillRect.adjusted(16.0, 0.0, -6.0, 0.0),
                                  Qt::AlignVCenter | Qt::AlignLeft,
                                  summaryText);
            }
        }

        // Rich Neural Network & Static Tensor Arena card beneath every `tinyml.ecu` node.
        if (node.typeName == "tinyml.ecu") {
            paintTinyMlVisualizerCard(painter, nodeRect, node, status);
        }
    }

    painter->restore();
}

void CanvasPanel::paintTinyMlVisualizerCard(QPainter* painter,
                                            const QRectF& nodeRect,
                                            const NodeDescription& node,
                                            const NodeStatus* status) const
{
    const qreal cardWidth = std::max(nodeRect.width() + 70.0, 276.0);
    const qreal cardHeight = 116.0;
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
    cardBg.setAlpha(242);
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
