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
#include "core/database/CanMessage.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/plot/SignalSeries.h"
#include "core/trace/TraceStore.h"
#include "ui/engine/CanEngineController.h"
#include "ui/theme/Theme.h"

#include <QtNodes/internal/Definitions.hpp>

#include <QByteArray>
#include <QFont>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QEvent;
class QHideEvent;
class QMenu;
class QPainter;
class QPushButton;
class QShowEvent;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
class NodeGraphicsObject;
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

    /// Binds the live frame trace store so node cards can inspect and display
    /// the current 8-byte CAN frame and ID passing through each block.
    void setTraceStore(const TraceStore* store);

    /// Binds the decoded signal series store for live signal readouts on plot
    /// and decoder blocks.
    void setPlotStore(const SignalSeriesStore* store);

    /// Supplies the loaded DBC databases so node cards can decode physical
    /// signals and message names directly inside each block.
    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

    /// The position of the divider inside this panel, for the settings file.
    [[nodiscard]] QByteArray splitterState() const;

    /// Ignores an empty or unusable state, leaving the default proportions.
    void restoreSplitterState(const QByteArray& state);

    /// Compact Pipeline & TinyML status summary for the main window's footer.
    [[nodiscard]] QString statusSummaryText() const noexcept { return m_statusSummaryText; }
    [[nodiscard]] QString statusSummaryState() const noexcept { return m_statusSummaryState; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

Q_SIGNALS:
    /// The user selected a node. The Properties panel shows its settings.
    void nodeSelected(const QString& descriptionId);

    /// The user asked to edit a block's script, from the canvas.
    void editScriptRequested(const QString& descriptionId);

    /// The graph changed in a way worth telling the user about - a node added,
    /// a wire drawn. The status bar says whether it still compiles.
    void graphEdited();

    /// Emitted when the compact Pipeline / TinyML summary changes, for display
    /// in the application status bar footer.
    void statusSummaryChanged(const QString& summaryText, const QString& stateToken);

public Q_SLOTS:
    /// Repaints the canvas in the current theme.
    void applyTheme(const Theme& theme);

    /// Starts or stops the canvas wire-flow and neural network animation.
    void setSimulationRunning(bool running);

    /// Updates the live per-node counters displayed on Canvas telemetry badges.
    void setNodeStatuses(const QList<NodeStatus>& nodes);

    /// Updates the live channel bitrate / frame rate summary.
    void setChannelStatuses(const QList<ChannelStatus>& channels);

    /// Deletes all currently selected nodes and wires on the Canvas.
    void deleteSelectedItems();

private:
    struct NodeFrameTelemetry final {
        bool hasLiveFrame{false};
        bool isPreview{false};
        CanFrame frame{};
        std::uint32_t cycleUs{0};
        std::uint64_t changedMask{0};
        QString directionTag;
        QString idDlcText;
        QString cycleText;
        std::array<QString, 8> hexBytes{};
        QString messageName;
        QString decodedLine1;
        QString decodedLine2;
    };

    static void applyStyles(const Theme& theme);

    void buildPalette();
    void addNodeFromPalette(QTreeWidgetItem* item);
    void addSelectedPaletteNode();

    void addNodeAt(const QString& typeName, const QPointF& scenePosition);
    [[nodiscard]] QPointF findNonOverlappingPosition(const QPointF& desired) const;

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
    void refreshTelemetryCache();
    [[nodiscard]] NodeFrameTelemetry computeNodeTelemetry(const NodeDescription& node,
                                                          const NodeStatus* status,
                                                          bool faultInjected) const;
    void decodeFromDatabasesOrBuiltin(const NodeDescription& node,
                                      NodeFrameTelemetry& telemetry) const;
    void paintSceneForeground(QPainter* painter, const QRectF& viewportRect);
    void paintNodeTelemetryCard(QPainter* painter,
                                QtNodes::NodeId nodeId,
                                QtNodes::NodeGraphicsObject* ngo,
                                const QRectF& nodeRect,
                                const NodeDescription& node,
                                const NodeStatus* status,
                                double pulse) const;
    void paintTinyMlVisualizerCard(QPainter* painter,
                                   const QRectF& nodeRect,
                                   const NodeDescription& node,
                                   const NodeStatus* status) const;

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;
    SystemVariables* m_variables{nullptr};
    const TraceStore* m_traceStore{nullptr};
    const SignalSeriesStore* m_plotStore{nullptr};
    std::vector<std::shared_ptr<const CanDatabase>> m_databases;

    std::unique_ptr<PipelineGraphModel> m_model;
    QtNodes::BasicGraphicsScene* m_scene{nullptr};
    QtNodes::GraphicsView* m_view{nullptr};
    QSplitter* m_splitter{nullptr};
    QTreeWidget* m_palette{nullptr};

    QWidget* m_hudBar{nullptr};
    QPushButton* m_addBlockButton{nullptr};
    QPushButton* m_deleteBlockButton{nullptr};
    QPushButton* m_animateButton{nullptr};
    QPushButton* m_injectFaultButton{nullptr};
    QPushButton* m_demoPipelineButton{nullptr};

    QVariantAnimation* m_flowAnimation{nullptr};
    qreal m_phase{0.0};
    bool m_simulationRunning{false};
    bool m_animationsEnabled{true};

    QHash<QString, NodeStatus> m_nodeStatuses;
    QHash<QString, NodeFrameTelemetry> m_telemetryCache;
    QList<ChannelStatus> m_channelStatuses;
    QString m_statusSummaryText;
    QString m_statusSummaryState{QStringLiteral("ready")};
    Theme m_theme;

    QFont m_monoBoldFont;
    QFont m_monoSmallFont;
    QFont m_monoSignalFont;
    QFont m_badgeFont;
    QFont m_titleFont;
    QFont m_subFont;
    QFont m_dbcTitleFont;
};

} // namespace torquebus::ui
