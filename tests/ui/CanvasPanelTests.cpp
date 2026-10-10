// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pipeline canvas, driven through the same widgets and QtNodes actions the
// user reaches: the palette, the view's keyboard shortcuts, the scene's undo
// stack and the buttons that pan, zoom and fit the view. The model is checked
// against the description it fronts, because "the canvas and the project
// disagree" is the failure that matters here.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/canvas/CanvasPanel.h"
#include "ui/canvas/PipelineGraphModel.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUndoStack>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <algorithm>
#include <string>

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

    /// One of the navigation toolbar's buttons: "pan", "zoomIn", "zoomOut" or "fit".
    [[nodiscard]] QAction* navAction(const char* name) const
    {
        return panel.findChild<QAction*>(QStringLiteral("torquebus.action.canvas.")
                                         + QLatin1String(name));
    }

    [[nodiscard]] QtNodes::NodeId canvasIdOf(const std::string& descriptionId) const
    {
        return qobject_cast<PipelineGraphModel*>(&scene()->graphModel())->canvasId(descriptionId);
    }

    /// Where the block is in the scene, as the foreground paints its card over it.
    [[nodiscard]] QRectF blockRect(const std::string& descriptionId) const
    {
        const QtNodes::NodeId id = canvasIdOf(descriptionId);
        return scene()->nodeGraphicsObject(id)->mapRectToScene(
            QRectF{QPointF{0.0, 0.0}, QSizeF{scene()->nodeGeometry().size(id)}});
    }

    void place(const std::string& descriptionId, const QPointF& position)
    {
        scene()->graphModel().setNodeData(
            canvasIdOf(descriptionId), QtNodes::NodeRole::Position, position);
        settleCanvas();
    }

    /// The part of the scene the canvas is showing.
    [[nodiscard]] QRectF visibleScene() const
    {
        return view()->mapToScene(view()->viewport()->rect()).boundingRect();
    }

    void mouse(QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons)
    {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event{type,
                          QPointF{at},
                          QPointF{view()->viewport()->mapToGlobal(at)},
                          button,
                          buttons,
                          Qt::NoModifier};
        QApplication::sendEvent(view()->viewport(), &event);
    }

    /// Presses the left button at `from`, drags by `by` and lets go.
    void drag(const QPoint& from, const QPoint& by)
    {
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
        mouse(QEvent::MouseMove, from + by / 2, Qt::LeftButton);
        mouse(QEvent::MouseMove, from + by, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, from + by, Qt::NoButton);
        settleCanvas();
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

// ---------------------------------------------------------------------------------------------
// The toolbar that moves the view: pan, zoom and fit.
// ---------------------------------------------------------------------------------------------

/// The panel as the application has it: with a theme, and so with the icons its buttons are drawn
/// from. Without one a button has no icon, and a QToolButton falls back to its text.
class CanvasPanelThemedTest : public testing::Test {
protected:
    CanvasPanelThemedTest()
        : catalog{NodeCatalog::withBuiltinTypes()}
        , panel{description, catalog}
    {
        panel.resize(1200, 700);
        panel.show();
        settleCanvas();
    }

    ~CanvasPanelThemedTest() override { panel.releaseGraph(); }

    [[nodiscard]] QAction* navAction(const char* name) const
    {
        return panel.findChild<QAction*>(QStringLiteral("torquebus.action.canvas.")
                                         + QLatin1String(name));
    }

    ThemeManager themes;
    GraphDescription description;
    NodeCatalog catalog;
    CanvasPanel panel;
};

TEST_F(CanvasPanelThemedTest, NavigationToolBarIsAColumnOfSmallIconButtonsBesideTheCanvas)
{
    auto* toolBar = panel.findChild<QToolBar*>(QStringLiteral("torquebus.toolbar.canvas"));
    ASSERT_NE(toolBar, nullptr);
    EXPECT_TRUE(toolBar->isVisible());

    // Icons only, at the size the Graph panel's toolbar uses.
    EXPECT_EQ(toolBar->toolButtonStyle(), Qt::ToolButtonIconOnly);
    EXPECT_EQ(toolBar->iconSize(), QSize(18, 18));
    EXPECT_EQ(toolBar->orientation(), Qt::Vertical);

    int lastTop = -1;
    for (const char* name : {"pan", "zoomIn", "zoomOut", "fit"}) {
        QAction* action = navAction(name);
        ASSERT_NE(action, nullptr) << name;
        EXPECT_FALSE(action->text().isEmpty()) << name;
        EXPECT_FALSE(action->toolTip().isEmpty()) << name;
        EXPECT_FALSE(action->icon().isNull()) << name;

        // Shown, small, and not folded away into the toolbar's overflow menu.
        auto* button = qobject_cast<QToolButton*>(toolBar->widgetForAction(action));
        ASSERT_NE(button, nullptr) << name;
        EXPECT_TRUE(button->isVisible()) << name;
        EXPECT_LT(button->width(), 50) << name;
        EXPECT_LT(button->height(), 40) << name;

        // Top to bottom in the order they are named in here.
        EXPECT_GT(button->y(), lastTop) << name;
        lastTop = button->y();
    }

    // Pan is a tool that stays on; the others act once.
    EXPECT_TRUE(navAction("pan")->isCheckable());
    EXPECT_FALSE(navAction("pan")->isChecked());
    EXPECT_FALSE(navAction("zoomIn")->isCheckable());
    EXPECT_FALSE(navAction("zoomOut")->isCheckable());
    EXPECT_FALSE(navAction("fit")->isCheckable());

    // Beside the canvas, left of it and level with its top: the canvas gives up a strip of its
    // width, and the bar above it - which needs most of what a panel has - gives up nothing.
    auto* canvas = panel.findChild<QtNodes::GraphicsView*>();
    ASSERT_NE(canvas, nullptr);
    EXPECT_LE(toolBar->mapTo(&panel, QPoint{toolBar->width(), 0}).x(),
              canvas->mapTo(&panel, QPoint{0, 0}).x());
    EXPECT_EQ(toolBar->mapTo(&panel, QPoint{0, 0}).y(), canvas->mapTo(&panel, QPoint{0, 0}).y());
    EXPECT_LT(toolBar->width(), 60);
}

TEST_F(CanvasPanelThemedTest, NavigationIconsAreAskedForAgainWhenTheThemeChanges)
{
    for (const char* name : {"pan", "zoomIn", "zoomOut", "fit"}) {
        QAction* action = navAction(name);
        ASSERT_NE(action, nullptr) << name;

        // A theme change reaches the panel as applyTheme(), and an icon is tinted for the theme it
        // was asked for in: it has to be asked for again, not kept.
        action->setIcon(QIcon{});
        panel.applyTheme(themes.theme());
        EXPECT_FALSE(action->icon().isNull()) << name << " after the theme changed";
    }
}

TEST_F(CanvasPanelTest, ZoomButtonsStepTheViewAndStopAtItsLimits)
{
    addFromPalette("can.source");
    ASSERT_NEAR(view()->getScale(), 1.0, 1e-9);

    navAction("zoomIn")->trigger();
    EXPECT_GT(view()->getScale(), 1.0);

    navAction("zoomOut")->trigger();
    EXPECT_NEAR(view()->getScale(), 1.0, 1e-9);

    // Out as far as it goes: past the 0.3 that QtNodes stops at, so that a big pipeline can be seen
    // whole, and no further however many times it is pressed.
    for (int i = 0; i < 60; ++i) {
        navAction("zoomOut")->trigger();
    }
    const double floor = view()->getScale();
    EXPECT_LT(floor, 0.3);
    EXPECT_GT(floor, 0.0);
    navAction("zoomOut")->trigger();
    EXPECT_DOUBLE_EQ(view()->getScale(), floor);

    for (int i = 0; i < 60; ++i) {
        navAction("zoomIn")->trigger();
    }
    const double ceiling = view()->getScale();
    EXPECT_GT(ceiling, 1.0);
    navAction("zoomIn")->trigger();
    EXPECT_DOUBLE_EQ(view()->getScale(), ceiling);
}

TEST_F(CanvasPanelTest, FitToWindowBringsEveryBlockIntoViewAndCentresThem)
{
    addFromPalette("can.source");
    addFromPalette("lua.ecu");
    addFromPalette("can.transmit");
    ASSERT_EQ(description.nodes().size(), 3U);

    // Far enough apart that real size cannot show all three, and that fitting them has to look at
    // the extremes of both axes.
    place(description.nodes()[0].id, QPointF{-2500.0, -1500.0});
    place(description.nodes()[1].id, QPointF{2500.0, 200.0});
    place(description.nodes()[2].id, QPointF{-300.0, 1800.0});

    QRectF all;
    for (const auto& node : description.nodes()) {
        all = all.united(blockRect(node.id));
    }
    ASSERT_FALSE(visibleScene().contains(all)) << "the layout is meant to need a zoom-out";

    navAction("fit")->trigger();
    settleCanvas();

    const double scale = view()->getScale();
    EXPECT_LT(scale, 1.0);
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visibleScene().contains(blockRect(node.id))) << node.id << " is out of view";
    }

    // Centred...
    EXPECT_NEAR(visibleScene().center().x(), all.center().x(), 2.0 / scale);
    EXPECT_NEAR(visibleScene().center().y(), all.center().y(), 2.0 / scale);

    // ...and as large as the window lets it be, not merely zoomed out far enough: along the side
    // that limits, the blocks take nearly all of the canvas.
    const QSize viewport = view()->viewport()->size();
    const double used =
        std::max(all.width() * scale / viewport.width(), all.height() * scale / viewport.height());
    EXPECT_GT(used, 0.85);
    EXPECT_LE(used, 1.0);

    // And again from somewhere else: zoomed in and scrolled away.
    for (int i = 0; i < 5; ++i) {
        navAction("zoomIn")->trigger();
    }
    view()->centerOn(QPointF{12000.0, -9000.0});
    navAction("fit")->trigger();
    settleCanvas();

    EXPECT_NEAR(view()->getScale(), scale, 1e-9);
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visibleScene().contains(blockRect(node.id))) << node.id << " is out of view";
    }
}

TEST_F(CanvasPanelTest, FitToWindowDoesNotBlowUpAShortPipeline)
{
    addFromPalette("can.source");
    ASSERT_EQ(description.nodes().size(), 1U);

    navAction("zoomIn")->trigger();
    navAction("zoomIn")->trigger();
    ASSERT_GT(view()->getScale(), 1.0);

    // One block in a wide window: it is shown at real size, not enlarged to fill the window.
    navAction("fit")->trigger();
    settleCanvas();

    EXPECT_NEAR(view()->getScale(), 1.0, 1e-9);

    const QRectF block = blockRect(description.nodes()[0].id);
    EXPECT_TRUE(visibleScene().contains(block));
    EXPECT_NEAR(visibleScene().center().x(), block.center().x(), 2.0);
    EXPECT_NEAR(visibleScene().center().y(), block.center().y(), 2.0);
}

TEST_F(CanvasPanelTest, FitToWindowCountsTheCardUnderATinyMlBlock)
{
    addFromPalette("tinyml.ecu");
    ASSERT_EQ(description.nodes().size(), 1U);

    navAction("fit")->trigger();
    settleCanvas();

    // The neural network card the foreground paints beneath the block is part of what the block
    // looks like: 8 units below it, and 114 high.
    const QRectF block = blockRect(description.nodes()[0].id);
    const QRectF card{block.left(), block.bottom() + 8.0, block.width(), 114.0};

    EXPECT_TRUE(visibleScene().contains(block.united(card)));
    EXPECT_NEAR(visibleScene().center().y(), block.united(card).center().y(), 2.0);
}

TEST_F(CanvasPanelTest, FitToWindowOnAnEmptyCanvasGoesBackToRealSizeAtTheOrigin)
{
    ASSERT_TRUE(description.nodes().empty());

    navAction("zoomIn")->trigger();
    navAction("zoomIn")->trigger();
    view()->centerOn(QPointF{4000.0, 3000.0});

    navAction("fit")->trigger();
    settleCanvas();

    EXPECT_NEAR(view()->getScale(), 1.0, 1e-9);
    EXPECT_NEAR(visibleScene().center().x(), 0.0, 2.0);
    EXPECT_NEAR(visibleScene().center().y(), 0.0, 2.0);
}

TEST_F(CanvasPanelTest, PanToolMovesTheCanvasAndLeavesTheBlocksAlone)
{
    addFromPalette("lua.ecu");
    ASSERT_EQ(description.nodes().size(), 1U);

    const std::string id = description.nodes()[0].id;
    auto* object = scene()->nodeGraphicsObject(canvasIdOf(id));
    ASSERT_NE(object, nullptr);

    // The middle of the block, wherever the canvas is showing it.
    const auto grab = [&] { return view()->mapFromScene(blockRect(id).center()); };
    const auto position = [&] {
        return QPointF{description.nodes()[0].x, description.nodes()[0].y};
    };

    // The canvas as it is: the same drag picks the block up.
    scene()->clearSelection();
    const QPointF home = position();
    drag(grab(), QPoint{80, 50});
    EXPECT_NEAR(position().x(), home.x() + 80.0, 2.0);
    EXPECT_NEAR(position().y(), home.y() + 50.0, 2.0);
    EXPECT_TRUE(object->isSelected());

    // With Pan on it moves the canvas underneath, and the block neither moves nor is selected.
    scene()->clearSelection();
    const QPointF placed = position();
    navAction("pan")->setChecked(true);
    EXPECT_FALSE(view()->isInteractive());

    const QPoint before = grab();
    drag(before, QPoint{90, 60});
    const QPoint after = grab();

    EXPECT_EQ(position(), placed);
    EXPECT_FALSE(object->isSelected());
    EXPECT_NEAR(after.x() - before.x(), 90, 1);
    EXPECT_NEAR(after.y() - before.y(), 60, 1);

    // Off again: back to moving blocks.
    navAction("pan")->setChecked(false);
    EXPECT_TRUE(view()->isInteractive());
    drag(grab(), QPoint{-40, -30});
    EXPECT_NEAR(position().x(), placed.x() - 40.0, 2.0);
    EXPECT_NEAR(position().y(), placed.y() - 30.0, 2.0);
}

TEST_F(CanvasPanelTest, PanToolSetsTheEditScriptButtonAside)
{
    addFromPalette("lua.ecu");
    ASSERT_EQ(description.nodes().size(), 1U);

    int requests = 0;
    QObject::connect(
        &panel, &CanvasPanel::editScriptRequested, [&requests](const QString&) { ++requests; });

    const QRectF block = blockRect(description.nodes()[0].id);
    const QPoint pencil = view()->mapFromScene(QPointF{block.right() - 103.0, block.top() + 10.0});

    // A click there while panning is the start of a drag, not a request to open the editor.
    navAction("pan")->setChecked(true);
    mouse(QEvent::MouseButtonPress, pencil, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, pencil, Qt::NoButton);
    EXPECT_EQ(requests, 0);

    navAction("pan")->setChecked(false);
    mouse(QEvent::MouseButtonPress, pencil, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, pencil, Qt::NoButton);
    EXPECT_EQ(requests, 1);
}

TEST_F(CanvasPanelTest, TheViewKeepsItsZoomAndPlaceWhenThePanelIsHiddenAndShownAgain)
{
    addFromPalette("can.source");
    addFromPalette("lua.ecu");
    place(description.nodes()[0].id, QPointF{-2500.0, -1500.0});
    place(description.nodes()[1].id, QPointF{2500.0, 1500.0});

    // Where the user has taken the view: zoomed in on one block, off to one side.
    navAction("zoomIn")->trigger();
    navAction("zoomIn")->trigger();
    view()->centerOn(QPointF{-2400.0, -1400.0});
    settleCanvas();

    const double scale = view()->getScale();
    const QPointF centre = visibleScene().center();

    // Another tab, and back. QtNodes' own view fits the whole scene each time it is shown, which
    // would throw away exactly what the buttons are for.
    panel.hide();
    settleCanvas();
    panel.show();
    settleCanvas();

    EXPECT_NEAR(view()->getScale(), scale, 1e-9);
    EXPECT_NEAR(visibleScene().center().x(), centre.x(), 2.0);
    EXPECT_NEAR(visibleScene().center().y(), centre.y(), 2.0);
}

TEST_F(CanvasPanelTest, ReplacingTheGraphFitsTheNewOneAtOnceWhileTheCanvasIsShown)
{
    // The view is somewhere else when the project is opened.
    view()->centerOn(QPointF{9000.0, 9000.0});

    NodeDescription first;
    first.id = "a";
    first.typeName = "can.source";
    first.x = -2500.0;
    first.y = -1500.0;
    description.addNode(std::move(first));

    NodeDescription second;
    second.id = "b";
    second.typeName = "can.transmit";
    second.x = 2500.0;
    second.y = 1500.0;
    description.addNode(std::move(second));

    panel.reload();
    settleCanvas();

    EXPECT_LT(view()->getScale(), 1.0);
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visibleScene().contains(blockRect(node.id))) << node.id << " is out of view";
    }
}

TEST_F(CanvasPanelTest, AGraphLoadedWhileTheCanvasIsHiddenIsFittedWhenItIsShown)
{
    panel.hide();
    settleCanvas();

    NodeDescription first;
    first.id = "a";
    first.typeName = "can.source";
    first.x = -2500.0;
    first.y = -1500.0;
    description.addNode(std::move(first));

    NodeDescription second;
    second.id = "b";
    second.typeName = "can.transmit";
    second.x = 2500.0;
    second.y = 1500.0;
    description.addNode(std::move(second));

    panel.reload();
    panel.show();
    settleCanvas();

    EXPECT_LT(view()->getScale(), 1.0);
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visibleScene().contains(blockRect(node.id))) << node.id << " is out of view";
    }
}

TEST_F(CanvasPanelTest, TinyMlDemoSetupShowsWhatItBuilds)
{
    QPushButton* demo = nullptr;
    for (auto* button : panel.findChildren<QPushButton*>()) {
        if (button->text() == QLatin1String("TinyML Demo Setup")) {
            demo = button;
        }
    }
    ASSERT_NE(demo, nullptr);

    // Somewhere that does not show it.
    view()->centerOn(QPointF{9000.0, 9000.0});

    demo->click();
    settleCanvas();

    ASSERT_FALSE(description.nodes().empty());
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visibleScene().contains(blockRect(node.id))) << node.id << " is out of view";
    }
}

} // namespace
