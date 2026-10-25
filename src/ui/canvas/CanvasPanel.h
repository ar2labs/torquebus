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
// It is drawn the way a network is drawn. The CAN Bus is a line - the two wires
// of the bus - and every module is a chip hanging off it by one drop, the way a
// simulation setup in CANoe draws one; a wire that is not a drop to a bus (a
// decoder to a plot) is a curve between two chips. A chip is small on purpose:
// its name, what it is and one line of what it is doing, so that a pipeline of
// twenty blocks is one screen. Open one into a card and it shows the frame it
// last carried, byte by byte.
//
// During a measurement the canvas draws live telemetry over what the blocks and
// wires are:
//   - Data packets running along the drops and the curves, colour-coded by port
//     type (Frames, Signals, Events);
//   - A halo on each running block, and on a chip the frame it last carried;
//   - A real-time Neural Network & Static Tensor Arena visualizer beneath every
//     opened `tinyml.ecu` Virtual ECU block, plus interactive fault injection.
//
// What is static is drawn by the blocks themselves, which QtNodes caches; what
// moves is drawn in the scene foreground, where it costs only what moves.

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
#include <utility>
#include <vector>

class QAction;
class QEvent;
class QGraphicsItem;
class QHideEvent;
class QLineEdit;
class QMenu;
class QPainter;
class QPushButton;
class QShowEvent;
class QSplitter;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;

namespace QtNodes {
class BasicGraphicsScene;
class ConnectionGraphicsObject;
class GraphicsView;
class NodeGraphicsObject;
} // namespace QtNodes

namespace torquebus::ui {

class PipelineConnectionPainter;
class PipelineGraphModel;
class PipelineNodePainter;

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

    /// Deletes a specific block and all its connections by description ID.
    void deleteBlock(const QString& descriptionId);

    /// Puts every block where a tidy diagram has it: each bus a line across the
    /// top of what is wired to it, the modules that speak on it above, the ones
    /// that only listen below, and whatever hangs off those below them.
    void arrangeBlocks();

    /// Rewrites a pipeline that reaches a channel through a CAN Channel and a CAN
    /// Transmit per ECU as one CAN Bus per channel. Nothing that runs changes.
    void groupOnBus();

    /// Opens every block into a card, or closes them all to chips.
    void setDetailsShown(bool shown);

    /// Opens one block into a card, or closes it, and makes room: what is beside it in its row
    /// moves aside, and a block that hangs over its bus grows upwards, away from the bus.
    void setBlockExpanded(QtNodes::NodeId nodeId, bool expanded);

    /// Wires a block to a bus, as dropping it on the rail does. Returns whether
    /// anything was wired.
    bool attachToBus(const QString& blockId, const QString& busId);

    /// Takes a block off a bus.
    void detachFromBus(const QString& blockId, const QString& busId);

private:
    friend class PipelineNodePainter;
    friend class PipelineConnectionPainter;

    /// What a chip says about a block right now: the line under its name, whether
    /// that line is live, and whether the block is in trouble.
    struct ChipInfo final {
        QString line;
        bool live{false};
        bool alert{false};
    };

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
    void applyPaletteIcons();

    /// The small icon buttons that move the view: pan, zoom and fit.
    void buildNavigationToolBar();
    void applyNavigationIcons();

    /// The icons of the bar above the canvas, asked for again whenever the theme changes.
    void applyHudIcons();

    /// Delete is offered only while something is selected to delete.
    void updateEditButtons();

    /// The bar above the canvas shows its labels when there is room for them and its icons alone
    /// when there is not, so the Pipeline panel is never made wider by it. applyHudSizing()
    /// measures both (after a theme or font change), updateHudCompactness() picks one for the
    /// width the bar has, and the tooltip carries the name while the label is away.
    void applyHudSizing();
    void updateHudCompactness();
    void setHudLabelsShown(bool shown);

    /// Pan on: a drag anywhere moves the canvas and no block is touched. Off: the canvas as it was.
    void setPanToolActive(bool active);

    /// Zooms and centres so that every block, and what is drawn around one, is in view.
    void fitToView();

    /// Fits the canvas if it has not been since it was filled: the first time it is shown, and
    /// after the project was replaced. Otherwise the view stays as the user left it.
    void fitIfPending();

    /// Every block on the canvas, with the card a TinyML block draws below itself. Null when empty.
    [[nodiscard]] QRectF contentBounds() const;

    void addNodeFromPalette(QTreeWidgetItem* item);
    void addSelectedPaletteNode();

    void addNodeAt(const QString& typeName, const QPointF& scenePosition);

    /// A block dropped from the palette: where the pointer is, and on the bus if that is where
    /// it was let go.
    void addNodeAtDrop(const QString& typeName, const QPointF& scenePosition);
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

    /// The body of a block that has been opened into a card, in the block's own
    /// coordinates: its header, the frame it last carried and what the database
    /// says that frame means.
    void paintCardBody(QPainter* painter,
                       QtNodes::NodeGraphicsObject& ngo,
                       const QRectF& nodeRect,
                       const NodeDescription& node) const;
    void paintTinyMlVisualizerCard(QPainter* painter,
                                   const QRectF& nodeRect,
                                   const NodeDescription& node,
                                   const NodeStatus* status) const;

    [[nodiscard]] ChipInfo chipInfo(const NodeDescription& node) const;

    /// Where every block is, and where a tidy diagram would put it. Moving the blocks to a list of
    /// positions is placeBlocks(), which is what arranging and undoing it both do.
    [[nodiscard]] std::vector<std::pair<std::string, QPointF>> blockPositions() const;
    [[nodiscard]] std::vector<std::pair<std::string, QPointF>> arrangedPositions() const;
    void placeBlocks(const std::vector<std::pair<std::string, QPointF>>& positions);

    /// Puts the whole pipeline back to a description - what undoing, and redoing, a grouping do.
    void restoreGraph(const GraphDescription& graph);

    /// Asks every block, bus and wire to be drawn again. QtNodes keeps each as a
    /// picture, so what changes them from outside - the theme, a measurement
    /// starting - has to say so.
    void invalidateItems();

    /// Asks the blocks whose chip says something different from last time to be
    /// drawn again, and no others.
    void refreshChips();

    [[nodiscard]] QString findScriptNodeAt(const QPointF& scenePosition,
                                           bool onlyEditButtons) const;

    /// The block whose chevron is at the point, or empty.
    [[nodiscard]] QString findExpandButtonAt(const QPointF& scenePosition) const;

    /// The bus-drop wire whose pill is at the point, if it shows one.
    [[nodiscard]] QtNodes::ConnectionId findFrameFormatPillAt(const QPointF& scenePosition,
                                                              bool& found) const;

    /// The wire to a bus under the point, if there is one.
    [[nodiscard]] QtNodes::ConnectionId findBusWireAt(const QPointF& scenePosition,
                                                      bool& found) const;

    void showBusMenu(QtNodes::NodeId busId, const QPointF& scenePosition);
    void showWireMenu(QtNodes::ConnectionId wire, const QPoint& globalPosition);
    void cycleFrameFormat(QtNodes::ConnectionId wire);

    /// Adds a block of a type to a bus, below it where there is room and wired
    /// the way dropping it there would.
    void addModuleToBus(const QString& typeName, QtNodes::NodeId busId, const QPointF& near);

    /// Wires a block just added to the bus the pointer is over, or the one that is
    /// selected, and puts it where a module on that bus goes. False when there is
    /// no bus to put it on.
    bool placeOnBusNear(QtNodes::NodeId block, const QPointF& scenePosition);

    /// The description's own entry for a canvas block, to be edited. Null when there is none.
    [[nodiscard]] NodeDescription* editableNode(QtNodes::NodeId nodeId);

    [[nodiscard]] QtNodes::NodeId selectedBus() const;
    [[nodiscard]] QtNodes::NodeId busAt(const QPointF& scenePosition, qreal slack) const;

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
    QLineEdit* m_paletteFilter{nullptr};

    QWidget* m_hudBar{nullptr};
    QToolBar* m_navToolBar{nullptr};
    QAction* m_actionPan{nullptr};
    QAction* m_actionZoomIn{nullptr};
    QAction* m_actionZoomOut{nullptr};
    QAction* m_actionFit{nullptr};
    bool m_panToolActive{false};
    bool m_fitPending{true};
    QPushButton* m_addBlockButton{nullptr};
    QPushButton* m_deleteBlockButton{nullptr};
    QPushButton* m_animateButton{nullptr};
    QPushButton* m_injectFaultButton{nullptr};
    QPushButton* m_demoPipelineButton{nullptr};
    QPushButton* m_arrangeButton{nullptr};
    QPushButton* m_detailsButton{nullptr};
    bool m_hudCompact{false};
    int m_hudLabelledWidth{0};

    QVariantAnimation* m_flowAnimation{nullptr};
    qreal m_phase{0.0};
    bool m_simulationRunning{false};
    bool m_animationsEnabled{true};

    QHash<QString, NodeStatus> m_nodeStatuses;
    QHash<QString, NodeFrameTelemetry> m_telemetryCache;

    /// The line each chip last drew, to tell which ones have to be drawn again.
    QHash<QString, QString> m_chipLines;
    QList<ChannelStatus> m_channelStatuses;
    QString m_statusSummaryText;
    QString m_statusSummaryState{QStringLiteral("ready")};
    Theme m_theme;

    /// The panel handled the press of a click itself, and the release that ends it is not the
    /// scene's to be shown.
    bool m_swallowRelease{false};

    bool m_hoveringScriptEdit{false};
    QString m_hoveredScriptNodeId;

    QFont m_monoBoldFont;
    QFont m_monoSmallFont;
    QFont m_monoSignalFont;
    QFont m_badgeFont;
    QFont m_titleFont;
    QFont m_subFont;
    QFont m_dbcTitleFont;
};

} // namespace torquebus::ui
