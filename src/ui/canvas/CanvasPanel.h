// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The pipeline, drawn and animated in real time.
//
// A palette of node types on the left, the interactive QtNodes canvas on the
// right. Dropping a block and dragging a wire edits the GraphDescription
// directly - there is no "apply" step, because there is nothing to apply: the
// description *is* the project, and the engine builds from it on the next Start.
//
// During a measurement, the canvas renders live visual telemetry directly in
// the scene foreground:
//   - Animated data packets flowing along cubic Bezier wires, colour-coded by
//     port type (Frames, Signals, Events);
//   - Pulsing node activity halos and live counter badges above each block;
//   - A real-time Neural Network & Static Tensor Arena visualizer beneath every
//     `tinyml.ecu` Virtual ECU block, plus interactive fault injection.

#pragma once

#include "core/dashboard/SystemVariables.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/engine/CanEngineController.h"
#include "ui/theme/Theme.h"

#include <QtNodes/internal/Definitions.hpp>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>

class QLabel;
class QMenu;
class QPainter;
class QPushButton;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
} // namespace QtNodes

namespace torquebus::ui {

class PipelineGraphModel;

class CanvasPanel final : public QWidget {
    Q_OBJECT

public:
    /// Neither the description nor the catalog is owned; both outlive the panel.
    CanvasPanel(GraphDescription& description,
                const NodeCatalog& catalog,
                QWidget* parent = nullptr);
    ~CanvasPanel() override;

    /// Rebuilds the canvas from the description, after a project is opened.
    void reload();

    /// Tears down the scene and the model while the description is still alive.
    ///
    /// Must be called by whoever owns the GraphDescription, from its own
    /// destructor, before that description dies. See the definition for why the
    /// panel cannot simply do this in its own destructor.
    void releaseGraph();

    /// Binds the engine's SystemVariables store so the Canvas HUD can read
    /// live TinyML metrics and toggle interactive anomaly injection.
    void setSystemVariables(SystemVariables* variables);

    /// The position of the divider inside this panel, for the settings file.
    [[nodiscard]] QByteArray splitterState() const;

    /// Ignores an empty or unusable state, leaving the default proportions.
    void restoreSplitterState(const QByteArray& state);

Q_SIGNALS:
    /// The user selected a node. The Properties panel shows its settings.
    void nodeSelected(const QString& descriptionId);

    /// The user asked to edit a block's script, from the canvas.
    void editScriptRequested(const QString& descriptionId);

    /// The graph changed in a way worth telling the user about - a node added,
    /// a wire drawn. The status bar says whether it still compiles.
    void graphEdited();

public Q_SLOTS:
    /// Repaints the canvas in the current theme.
    void applyTheme(const Theme& theme);

    /// Starts or stops the 60 FPS canvas wire-flow and neural network animation.
    void setSimulationRunning(bool running);

    /// Updates the live per-node counters displayed on Canvas telemetry badges.
    void setNodeStatuses(const QList<NodeStatus>& nodes);

    /// Updates the live channel bitrate / frame rate summary in the Canvas HUD.
    void setChannelStatuses(const QList<ChannelStatus>& channels);

private:
    static void applyStyles(const Theme& theme);

    void buildPalette();
    void addNodeFromPalette(QTreeWidgetItem* item);

    void addNodeAt(const QString& typeName, const QPointF& scenePosition);

    [[nodiscard]] QMenu* buildSceneMenu(const QPointF& scenePosition);
    void showNodeMenu(QtNodes::NodeId nodeId, const QPointF& scenePosition);

    void attachEcuTo(QtNodes::NodeId sourceNodeId);
    void attachTinyMlEcuTo(QtNodes::NodeId sourceNodeId);
    void buildTinyMlDemoPipeline();

    [[nodiscard]] QtNodes::NodeId findNodeOnChannel(const std::string& typeName,
                                                    std::int64_t channel) const;

    [[nodiscard]] std::string nodeTypeName(const std::string& descriptionId) const;

    void connectPorts(QtNodes::NodeId from, QtNodes::NodeId to);
    void connectPortIndex(QtNodes::NodeId from,
                          QtNodes::PortIndex fromPort,
                          QtNodes::NodeId to,
                          QtNodes::PortIndex toPort);

    [[nodiscard]] QtNodes::NodeId addNodeAtReturning(const QString& typeName,
                                                     const QPointF& scenePosition);

    void updateAnimationState();
    void updateHudLabels();
    void paintSceneForeground(QPainter* painter, const QRectF& viewportRect);
    void paintTinyMlVisualizerCard(QPainter* painter,
                                   const QRectF& nodeRect,
                                   const NodeDescription& node,
                                   const NodeStatus* status) const;

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;
    SystemVariables* m_variables{nullptr};

    std::unique_ptr<PipelineGraphModel> m_model;
    QtNodes::BasicGraphicsScene* m_scene{nullptr};
    QtNodes::GraphicsView* m_view{nullptr};
    QSplitter* m_splitter{nullptr};
    QTreeWidget* m_palette{nullptr};

    QWidget* m_hudBar{nullptr};
    QLabel* m_hudStatusLabel{nullptr};
    QLabel* m_hudMetricsLabel{nullptr};
    QPushButton* m_animateButton{nullptr};
    QPushButton* m_injectFaultButton{nullptr};
    QPushButton* m_demoPipelineButton{nullptr};

    QVariantAnimation* m_flowAnimation{nullptr};
    qreal m_phase{0.0};
    bool m_simulationRunning{false};
    bool m_animationsEnabled{true};

    QHash<QString, NodeStatus> m_nodeStatuses;
    QList<ChannelStatus> m_channelStatuses;
    Theme m_theme;
};

} // namespace torquebus::ui
