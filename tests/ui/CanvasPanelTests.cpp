// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pipeline canvas, driven through the same widgets and QtNodes actions the
// user reaches: the palette, the view's keyboard shortcuts and the scene's undo
// stack. The model is checked against the description it fronts, because "the
// canvas and the project disagree" is the failure that matters here.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/canvas/CanvasPanel.h"
#include "ui/canvas/PipelineGraphModel.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>

#include <gtest/gtest.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUndoStack>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void settleCanvas()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

class CanvasPanelTest : public testing::Test {
protected:
    CanvasPanelTest()
        : catalog{NodeCatalog::withBuiltinTypes()}
        , panel{description, catalog}
    {
        panel.resize(1200, 700);
        panel.show();
        settleCanvas();
    }

    ~CanvasPanelTest() override { panel.releaseGraph(); }

    /// Adds a block the way the palette does.
    void addFromPalette(const char* typeName)
    {
        auto* palette = panel.findChild<QTreeWidget*>();
        ASSERT_NE(palette, nullptr);

        for (int g = 0; g < palette->topLevelItemCount(); ++g) {
            QTreeWidgetItem* group = palette->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c) {
                QTreeWidgetItem* entry = group->child(c);
                if (entry->data(0, Qt::UserRole + 1).toString() == QLatin1String(typeName)) {
                    Q_EMIT palette->itemDoubleClicked(entry, 0);
                    settleCanvas();
                    return;
                }
            }
        }
        FAIL() << "no palette entry for " << typeName;
    }

    [[nodiscard]] QtNodes::GraphicsView* view() const
    {
        return panel.findChild<QtNodes::GraphicsView*>();
    }

    [[nodiscard]] QtNodes::BasicGraphicsScene* scene() const
    {
        return view() != nullptr ? qobject_cast<QtNodes::BasicGraphicsScene*>(view()->scene())
                                 : nullptr;
    }

    void press(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QKeyEvent down{QEvent::KeyPress, key, modifiers};
        QApplication::sendEvent(view(), &down);
        QKeyEvent up{QEvent::KeyRelease, key, modifiers};
        QApplication::sendEvent(view(), &up);
        settleCanvas();

        // Shortcuts need a real window system to fire, so the ones QtNodes
        // registers as actions are triggered by name instead.
        if (modifiers == Qt::ControlModifier
            && (key == Qt::Key_Z || key == Qt::Key_Y || key == Qt::Key_D || key == Qt::Key_C
                || key == Qt::Key_V)) {
            if (key == Qt::Key_Z) {
                scene()->undoStack().undo();
            } else if (key == Qt::Key_Y) {
                scene()->undoStack().redo();
            } else {
                const char* name = key == Qt::Key_D   ? "Duplicate Selection"
                                   : key == Qt::Key_C ? "Copy Selection"
                                                      : "Paste Selection";
                for (QAction* a : view()->actions()) {
                    if (a->text() == QLatin1String(name)) {
                        a->trigger();
                    }
                }
            }
            settleCanvas();
        }
    }

    GraphDescription description;
    NodeCatalog catalog;
    CanvasPanel panel;
};

TEST_F(CanvasPanelTest, ExploreUndoAndClipboard)
{
    addFromPalette("can.source");
    addFromPalette("lua.ecu");
    ASSERT_EQ(description.nodes().size(), 2U);

    auto* model = &scene()->graphModel();
    ASSERT_NE(model, nullptr);

    // Select the first node and delete it with the keyboard.
    const auto allIds = model->allNodeIds();
    ASSERT_FALSE(allIds.empty());
    scene()->nodeGraphicsObject(*allIds.begin())->setSelected(true);
    view()->setFocus();
    press(Qt::Key_Delete);
    std::fprintf(stderr, "after delete: %zu nodes\n", description.nodes().size());

    press(Qt::Key_Z, Qt::ControlModifier);
    std::fprintf(stderr, "after ctrl+z: %zu nodes\n", description.nodes().size());

    press(Qt::Key_Y, Qt::ControlModifier);
    std::fprintf(stderr, "after ctrl+y: %zu nodes\n", description.nodes().size());

    press(Qt::Key_Z, Qt::ControlModifier);
    std::fprintf(stderr, "after ctrl+z again: %zu nodes\n", description.nodes().size());

    scene()->clearSelection();
    for (auto id : model->allNodeIds()) {
        scene()->nodeGraphicsObject(id)->setSelected(true);
    }
    press(Qt::Key_D, Qt::ControlModifier);
    std::fprintf(stderr, "after ctrl+d: %zu nodes\n", description.nodes().size());
    for (const auto& node : description.nodes()) {
        std::fprintf(stderr,
                     "  node %s (%s) at %.0f,%.0f\n",
                     node.id.c_str(),
                     node.typeName.c_str(),
                     node.x,
                     node.y);
    }
}

TEST_F(CanvasPanelTest, DeleteBlockClearsNodeAndWires)
{
    addFromPalette("can.source");
    addFromPalette("can.transmit");
    ASSERT_EQ(description.nodes().size(), 2U);

    const std::string sourceId = description.nodes()[0].id;
    const std::string transmitId = description.nodes()[1].id;

    auto* model = &scene()->graphModel();
    const auto sourceCanvas = qobject_cast<PipelineGraphModel*>(model)->canvasId(sourceId);
    const auto transmitCanvas = qobject_cast<PipelineGraphModel*>(model)->canvasId(transmitId);

    const QtNodes::ConnectionId conn{sourceCanvas, 0, transmitCanvas, 0};
    model->addConnection(conn);
    EXPECT_EQ(description.edges().size(), 1U);

    // Re-adding the exact same connection must be guarded against duplication
    model->addConnection(conn);
    EXPECT_EQ(description.edges().size(), 1U);

    // Delete source block via deleteBlock
    panel.deleteBlock(QString::fromStdString(sourceId));
    EXPECT_EQ(description.nodes().size(), 1U);
    EXPECT_EQ(description.edges().size(), 0U);

    // Delete remaining block via deleteSelectedItems
    scene()->clearSelection();
    scene()->nodeGraphicsObject(transmitCanvas)->setSelected(true);
    panel.deleteSelectedItems();
    EXPECT_EQ(description.nodes().size(), 0U);
}

TEST_F(CanvasPanelTest, ScriptEditRequestedEmittedOnLuaNodeDoubleClickAndEditClick)
{
    addFromPalette("lua.ecu");
    ASSERT_EQ(description.nodes().size(), 1U);
    const std::string ecuId = description.nodes()[0].id;

    auto* model = &scene()->graphModel();
    const auto ecuCanvas = qobject_cast<PipelineGraphModel*>(model)->canvasId(ecuId);
    ASSERT_NE(ecuCanvas, QtNodes::InvalidNodeId);

    auto* ngo = scene()->nodeGraphicsObject(ecuCanvas);
    ASSERT_NE(ngo, nullptr);

    QString requestedScriptId;
    QObject::connect(&panel, &CanvasPanel::editScriptRequested, [&](const QString& id) {
        requestedScriptId = id;
    });

    // 1. Double clicking the node emits editScriptRequested
    const QPointF sceneCenter = ngo->sceneBoundingRect().center();
    const QPoint viewPos = view()->mapFromScene(sceneCenter);

    QMouseEvent dblClick{QEvent::MouseButtonDblClick,
                         QPointF{viewPos},
                         QPointF{viewPos},
                         Qt::LeftButton,
                         Qt::LeftButton,
                         Qt::NoModifier};
    QApplication::sendEvent(view()->viewport(), &dblClick);
    settleCanvas();

    EXPECT_EQ(requestedScriptId, QString::fromStdString(ecuId));

    requestedScriptId.clear();

    // 2. Clicking the pencil edit button emits editScriptRequested
    const QRectF nodeRect = ngo->mapRectToScene(QRectF{0.0, 0.0, 310.0, 196.0});
    const QPointF btnScenePos{nodeRect.right() - 103.0, nodeRect.top() + 10.0};
    const QPoint btnViewPos = view()->mapFromScene(btnScenePos);

    QMouseEvent click{QEvent::MouseButtonPress,
                      QPointF{btnViewPos},
                      QPointF{btnViewPos},
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier};
    QApplication::sendEvent(view()->viewport(), &click);
    settleCanvas();

    EXPECT_EQ(requestedScriptId, QString::fromStdString(ecuId));
}

} // namespace
