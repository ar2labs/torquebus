// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The pipeline, drawn.
//
// A palette of node types on the left, the canvas on the right. Dropping a
// block and dragging a wire edits the GraphDescription directly - there is no
// "apply" step, because there is nothing to apply: the description *is* the
// project, and the engine builds from it on the next Start.
//
// What this panel deliberately does not do:
//
//   * It does not own the graph. Close the canvas and the pipeline still runs;
//     the measurement never depended on a window being open (rule #3).
//   * It does not edit node settings. Those go in the Properties panel, which
//     has room for a form and does not have to be zoomed into.

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/theme/Theme.h"

#include <QtNodes/internal/Definitions.hpp>

#include <QByteArray>
#include <QPointF>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>

class QSplitter;

class QTreeWidget;
class QTreeWidgetItem;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
} // namespace QtNodes

class QMenu;

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

    /// The position of the divider inside this panel, for the settings file.
    ///
    /// The dock layout saver knows about docks and the boundaries between them.
    /// A splitter a panel put inside itself is invisible to it, so unless it is
    /// saved here, dragging this divider is the one arrangement in the window
    /// that does not survive a restart.
    [[nodiscard]] QByteArray splitterState() const;

    /// Ignores an empty or unusable state, leaving the default proportions.
    void restoreSplitterState(const QByteArray& state);

Q_SIGNALS:
    /// The user selected a node. The Properties panel shows its settings.
    void nodeSelected(const QString& descriptionId);

    /// The user asked to edit a block's script, from the canvas.
    ///
    /// Separate from nodeSelected, which the script editor already follows:
    /// selection decides *what* the editor shows, this decides that the editor
    /// is what the user wants to be looking at. CANoe puts a pencil on the node
    /// for the same reason - editing the script is what people do with an ECU,
    /// and making that a trip through a properties form charges for it twice.
    void editScriptRequested(const QString& descriptionId);

    /// The graph changed in a way worth telling the user about - a node added,
    /// a wire drawn. The status bar says whether it still compiles.
    void graphEdited();

public Q_SLOTS:
    /// Repaints the canvas in the current theme.
    ///
    /// QtNodes keeps its styles in three process-wide singletons rather than
    /// per-view, so this is a static reconfiguration followed by a repaint -
    /// which is also why a second canvas would share the same colours.
    void applyTheme(const Theme& theme);

private:
    /// Loads the theme into QtNodes' three style singletons.
    ///
    /// Static and separate from applyTheme because it has to run *before* the
    /// GraphicsView is constructed: the view copies BackgroundColor out of the
    /// singleton in its constructor and never looks at it again.
    static void applyStyles(const Theme& theme);

    void buildPalette();
    void addNodeFromPalette(QTreeWidgetItem* item);

    /// Adds a block of `typeName`, places it at `scenePosition`, and selects it.
    ///
    /// The one path both the palette and the context menu go through, so a node
    /// added either way lands selected with its settings in front of the user.
    void addNodeAt(const QString& typeName, const QPointF& scenePosition);

    /// The menu for a right click on empty canvas: every block, by category.
    [[nodiscard]] QMenu* buildSceneMenu(const QPointF& scenePosition);

    /// The menu for a right click on a node.
    void showNodeMenu(QtNodes::NodeId nodeId, const QPointF& scenePosition);

    /// Puts a simulated ECU on the same channel a CAN Channel block reads.
    ///
    /// The gesture CANoe has and this did not: in its Simulation Setup a node
    /// dropped on a bus *is* on that bus, in both directions, with no wire to
    /// draw. Here that was an ECU block, a transmit block, and two wires - six
    /// actions for the thing people do most, and twenty-four of them for a rest
    /// bus of eight.
    ///
    /// It is still two edges in the graph afterwards, and they are visible and
    /// deletable like any others. Rule #11 is about there being one data path,
    /// not about how many gestures it takes to draw one.
    void attachEcuTo(QtNodes::NodeId sourceNodeId);

    /// The node of `typeName` whose `channel` matches, or InvalidNodeId.
    ///
    /// An ECU attached to CAN 1 must transmit on CAN 1. Reusing the transmit
    /// block that is already there is what keeps a second ECU from adding a
    /// second one beside it - and a graph with two transmit blocks on the same
    /// channel is not wrong, only confusing.
    [[nodiscard]] QtNodes::NodeId findNodeOnChannel(const std::string& typeName,
                                                    std::int64_t channel) const;

    /// The catalog type of a node in the description, or empty.
    [[nodiscard]] std::string nodeTypeName(const std::string& descriptionId) const;

    /// Wires port 0 to port 0, if the graph allows it.
    void connectPorts(QtNodes::NodeId from, QtNodes::NodeId to);

    /// addNodeAt without the selecting and announcing, for callers building
    /// several nodes at once: the last one is the one worth putting in front of
    /// the user, not each in turn.
    [[nodiscard]] QtNodes::NodeId addNodeAtReturning(const QString& typeName,
                                                     const QPointF& scenePosition);

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;

    std::unique_ptr<PipelineGraphModel> m_model;
    QtNodes::BasicGraphicsScene* m_scene{nullptr};
    QtNodes::GraphicsView* m_view{nullptr};
    QSplitter* m_splitter{nullptr};
    QTreeWidget* m_palette{nullptr};
};

} // namespace torquebus::ui
