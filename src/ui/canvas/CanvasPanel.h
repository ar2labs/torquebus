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

#include <QByteArray>
#include <QWidget>

#include <memory>

class QSplitter;

class QTreeWidget;
class QTreeWidgetItem;

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

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;

    std::unique_ptr<PipelineGraphModel> m_model;
    QtNodes::BasicGraphicsScene* m_scene{nullptr};
    QtNodes::GraphicsView* m_view{nullptr};
    QSplitter* m_splitter{nullptr};
    QTreeWidget* m_palette{nullptr};
};

} // namespace torquebus::ui
