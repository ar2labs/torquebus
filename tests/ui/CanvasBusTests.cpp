// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The CAN Bus on the Pipeline canvas: a line, with every module one drop off it.
//
// Two layers are tested, and the line between them matters. The model is where "one drop is one
// thing" is kept - the graph holds a drop as two wires, and the model is what makes wiring or
// deleting one of them do the same to the other - so it is tested on its own, with no window.
// What is drawn, and what a hand does to it, is tested through the widgets the person has: the
// palette, the rail and the ports, with real mouse events.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/canvas/CanvasLayout.h"
#include "ui/canvas/CanvasPainters.h"
#include "ui/canvas/CanvasPanel.h"
#include "ui/canvas/PipelineGraphModel.h"
#include "ui/theme/Theme.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

[[nodiscard]] bool hasEdge(const GraphDescription& graph,
                           const char* from,
                           std::size_t fromPort,
                           const char* to,
                           std::size_t toPort)
{
    return std::ranges::any_of(graph.edges(), [&](const EdgeDescription& edge) {
        return edge.fromNode == from && edge.fromPort == fromPort && edge.toNode == to
               && edge.toPort == toPort;
    });
}

// ---------------------------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------------------------

class BusModelTest : public testing::Test {
protected:
    BusModelTest()
        : catalog{NodeCatalog::withBuiltinTypes()}
        , model{description, catalog}
    { }

    QtNodes::NodeId add(const char* type, double x = 0.0, double y = 0.0)
    {
        const QtNodes::NodeId id = model.addNode(QString::fromLatin1(type));
        model.setNodeData(id, QtNodes::NodeRole::Position, QPointF{x, y});
        return id;
    }

    [[nodiscard]] const NodeDescription& block(QtNodes::NodeId id) const { return *model.node(id); }

    GraphDescription description;
    NodeCatalog catalog;
    PipelineGraphModel model;
};

} // namespace

TEST_F(BusModelTest, AChipIsAChipAndACardIsACardAndABusIsAsLongAsItsModulesAreSpreadOut)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu", 400.0, 140.0);

    EXPECT_EQ(model.blockSize(ecu),
              QSize(static_cast<int>(layout::kChipWidth), static_cast<int>(layout::kChipHeight)));
    EXPECT_EQ(model.blockSize(bus).height(), static_cast<int>(layout::kRailHeight));
    EXPECT_EQ(model.blockSize(bus).width(), static_cast<int>(layout::kRailMinWidth));

    // On the bus, the bus reaches the module and a little past it.
    ASSERT_TRUE(model.attachToBus(ecu, bus));
    const double reach = 400.0 + layout::kChipWidth + layout::kRailInset - block(bus).x;
    EXPECT_EQ(model.blockSize(bus).width(), static_cast<int>(std::ceil(reach)));

    // Opened into a card, the module is wider and the bus has to be as well.
    model.setExpanded(ecu, true);
    EXPECT_EQ(model.blockSize(ecu),
              QSize(static_cast<int>(layout::kCardWidth), static_cast<int>(layout::kCardHeight)));
    EXPECT_EQ(model.blockSize(bus).width(),
              static_cast<int>(
                  std::ceil(400.0 + layout::kCardWidth + layout::kRailInset - block(bus).x)));

    model.setExpanded(ecu, false);
    EXPECT_EQ(model.blockSize(ecu),
              QSize(static_cast<int>(layout::kChipWidth), static_cast<int>(layout::kChipHeight)));
}

TEST_F(BusModelTest, TheBusFollowsAModuleThatIsMovedToItsLeft)
{
    const auto bus = add("can.bus", 0.0, 0.0);
    const auto ecu = add("lua.ecu", 600.0, 140.0);
    ASSERT_TRUE(model.attachToBus(ecu, bus));
    EXPECT_EQ(block(bus).x, 0.0);

    // Past where its plate would end: the bus has to start further left to reach it.
    model.setNodeData(ecu, QtNodes::NodeRole::Position, QPointF{100.0, 140.0});

    EXPECT_NEAR(block(bus).x, 100.0 - layout::kRailPlateWidth - layout::kRailInset, 0.001);
}

TEST_F(BusModelTest, TheBusCannotBeMovedToTheRightOfAModuleItCarries)
{
    const auto bus = add("can.bus", 0.0, 0.0);
    const auto ecu = add("lua.ecu", 600.0, 140.0);
    ASSERT_TRUE(model.attachToBus(ecu, bus));

    model.setNodeData(bus, QtNodes::NodeRole::Position, QPointF{900.0, 50.0});

    // As far right as it can go and still reach the module; free to move down.
    EXPECT_NEAR(block(bus).x, 600.0 - layout::kRailPlateWidth - layout::kRailInset, 0.001);
    EXPECT_EQ(block(bus).y, 50.0);
}

TEST_F(BusModelTest, ABusWithNothingOnItCanGoAnywhere)
{
    const auto bus = add("can.bus");

    model.setNodeData(bus, QtNodes::NodeRole::Position, QPointF{-700.0, 300.0});

    EXPECT_EQ(block(bus).x, -700.0);
    EXPECT_EQ(block(bus).y, 300.0);
}

TEST_F(BusModelTest, DroppingAnEcuOnTheBusWiresItToListenAndToSpeak)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");

    ASSERT_TRUE(model.attachToBus(ecu, bus));

    EXPECT_TRUE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
    EXPECT_TRUE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));
    EXPECT_EQ(description.edges().size(), 2U);

    const auto direction = model.busDirection(ecu, bus);
    EXPECT_TRUE(direction.listens);
    EXPECT_TRUE(direction.talks);
}

TEST_F(BusModelTest, AFilterOnTheBusOnlyListensForItsOutputFedBackOntoItWouldBeALoop)
{
    const auto bus = add("can.bus");
    const auto filter = add("can.filter");

    ASSERT_TRUE(model.attachToBus(filter, bus));

    EXPECT_TRUE(hasEdge(description, "can_bus", 0, "can_filter", 0));
    EXPECT_FALSE(hasEdge(description, "can_filter", 0, "can_bus", 0));
}

TEST_F(BusModelTest, ABlockThatOnlySpeaksIsWiredToSpeak)
{
    const auto bus = add("can.bus");
    const auto restbus = add("sim.restbus");

    ASSERT_TRUE(model.attachToBus(restbus, bus));

    EXPECT_TRUE(hasEdge(description, "sim_restbus", 0, "can_bus", 0));
    EXPECT_EQ(description.edges().size(), 1U);
}

TEST_F(BusModelTest, ABlockWithNoFramesPortHasNothingToWireToABus)
{
    const auto bus = add("can.bus");
    const auto plot = add("signal.plot");

    EXPECT_FALSE(model.canAttachToBus(plot));
    EXPECT_FALSE(model.attachToBus(plot, bus));
    EXPECT_TRUE(description.edges().empty());
}

TEST_F(BusModelTest, WiringAnEcusOutputToTheBusByHandCompletesTheDrop)
{
    // The gesture: from the module's output port to the rail. One wire is drawn, and the drop is
    // two.
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");

    model.addConnection(QtNodes::ConnectionId{ecu, 0, bus, 0});

    EXPECT_TRUE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));
    EXPECT_TRUE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
}

TEST_F(BusModelTest, WiringTheBusToAModulesInputCompletesTheDropToo)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");

    model.addConnection(QtNodes::ConnectionId{bus, 0, ecu, 0});

    EXPECT_TRUE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
    EXPECT_TRUE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));
}

TEST_F(BusModelTest, DeletingOneHalfOfADropDeletesTheOther)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");
    ASSERT_TRUE(model.attachToBus(ecu, bus));

    ASSERT_TRUE(model.deleteConnection(QtNodes::ConnectionId{bus, 0, ecu, 0}));

    // The module is off the bus, and not left talking to it with nobody listening.
    EXPECT_TRUE(description.edges().empty());
}

TEST_F(BusModelTest, DetachingTakesEveryWireBetweenTheModuleAndTheBusAway)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");
    ASSERT_TRUE(model.attachToBus(ecu, bus));

    model.detachFromBus(ecu, bus);

    EXPECT_TRUE(description.edges().empty());
    EXPECT_TRUE(model.tapsBetween(ecu, bus).empty());
}

TEST_F(BusModelTest, ABusTakesAWireFromEveryModuleWhereAnOrdinaryInputTakesOne)
{
    const auto bus = add("can.bus");
    const auto a = add("lua.ecu");
    const auto b = add("lua.ecu");
    const auto c = add("lua.ecu");

    EXPECT_TRUE(model.connectionPossible(QtNodes::ConnectionId{a, 0, bus, 0}));
    model.addConnection(QtNodes::ConnectionId{a, 0, bus, 0});

    // Another module, same input: allowed. The same wire again: not.
    EXPECT_TRUE(model.connectionPossible(QtNodes::ConnectionId{b, 0, bus, 0}));
    EXPECT_FALSE(model.connectionPossible(QtNodes::ConnectionId{a, 0, bus, 0}));

    model.addConnection(QtNodes::ConnectionId{b, 0, bus, 0});
    model.addConnection(QtNodes::ConnectionId{c, 0, bus, 0});

    EXPECT_EQ(model.modulesOn(bus).size(), 3U);

    // A CAN Transmit still takes one.
    const auto transmit = add("can.transmit");
    model.addConnection(QtNodes::ConnectionId{a, 0, transmit, 0});
    EXPECT_FALSE(model.connectionPossible(QtNodes::ConnectionId{b, 0, transmit, 0}));
}

TEST_F(BusModelTest, ABusToABusIsRefused)
{
    const auto first = add("can.bus");
    const auto second = add("can.bus");

    // The graph could build a gateway out of that; nothing on the canvas could draw one.
    EXPECT_FALSE(model.connectionPossible(QtNodes::ConnectionId{first, 0, second, 0}));
}

TEST_F(BusModelTest, TheInputOfAModuleThatHearsSomethingElseKeepsItsOneWire)
{
    const auto bus = add("can.bus");
    const auto source = add("can.source");
    const auto ecu = add("lua.ecu");
    model.addConnection(QtNodes::ConnectionId{source, 0, ecu, 0});

    // The ECU is listening to a CAN Channel block: attaching it to the bus leaves that alone, and
    // wires what it can - the module can still be heard.
    ASSERT_TRUE(model.attachToBus(ecu, bus));
    EXPECT_TRUE(hasEdge(description, "can_source", 0, "lua_ecu", 0));
    EXPECT_FALSE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
    EXPECT_TRUE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));
}

TEST_F(BusModelTest, AModuleCanListenOnlyOrSpeakOnlyAndNeverNeither)
{
    const auto bus = add("can.bus");
    const auto ecu = add("lua.ecu");
    ASSERT_TRUE(model.attachToBus(ecu, bus));

    model.setBusDirection(ecu, bus, PipelineGraphModel::BusDirection{true, false});
    EXPECT_TRUE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
    EXPECT_FALSE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));

    model.setBusDirection(ecu, bus, PipelineGraphModel::BusDirection{false, true});
    EXPECT_FALSE(hasEdge(description, "can_bus", 0, "lua_ecu", 0));
    EXPECT_TRUE(hasEdge(description, "lua_ecu", 0, "can_bus", 0));

    model.setBusDirection(ecu, bus, PipelineGraphModel::BusDirection{true, true});
    EXPECT_EQ(description.edges().size(), 2U);

    // Neither is not an option: the module listens.
    model.setBusDirection(ecu, bus, PipelineGraphModel::BusDirection{false, false});
    EXPECT_TRUE(model.busDirection(ecu, bus).listens);
    EXPECT_FALSE(model.busDirection(ecu, bus).talks);
}

TEST_F(BusModelTest, TheFramesAModuleHearsAreAPropertyOfTheWireItHearsThemOn)
{
    const auto bus = add("can.bus");
    const auto decoder = add("j1939.decoder");
    ASSERT_TRUE(model.attachToBus(decoder, bus));

    const QtNodes::ConnectionId wire{bus, 0, decoder, 0};
    EXPECT_EQ(model.frameFormat(wire), CanFormatMatch::Any);

    model.setFrameFormat(wire, CanFormatMatch::ExtendedOnly);

    EXPECT_EQ(model.frameFormat(wire), CanFormatMatch::ExtendedOnly);
    ASSERT_EQ(description.edges().size(), 1U);
    EXPECT_TRUE(description.edges().front().frameFormat == CanFormatMatch::ExtendedOnly);
}

TEST_F(BusModelTest, ABlockDeletedFromTheBusShortensIt)
{
    const auto bus = add("can.bus");
    const auto near = add("lua.ecu", 300.0, 140.0);
    const auto far = add("lua.ecu", 1500.0, 140.0);
    ASSERT_TRUE(model.attachToBus(near, bus));
    ASSERT_TRUE(model.attachToBus(far, bus));

    const int long_ = model.blockSize(bus).width();
    model.deleteNode(far);

    EXPECT_LT(model.blockSize(bus).width(), long_);
}

TEST_F(BusModelTest, ACardStaysOpenWhenItsBlockIsRenamed)
{
    const auto ecu = add("lua.ecu");
    model.setExpanded(ecu, true);

    ASSERT_TRUE(model.setNodeData(ecu, QtNodes::NodeRole::Caption, QStringLiteral("engine")));

    EXPECT_TRUE(model.isExpanded(ecu));
}

TEST_F(BusModelTest, ACardIsNotLeftOpenForTheNextBlockThatGetsItsName)
{
    const auto ecu = add("lua.ecu");
    model.setExpanded(ecu, true);
    model.deleteNode(ecu);

    const auto again = add("lua.ecu");

    EXPECT_FALSE(model.isExpanded(again));
}

TEST_F(BusModelTest, ABusOnACanvasThatIsReloadedIsHeldOverItsModules)
{
    // A project saved with the rail where the modules were not: it is moved to reach them when it
    // is opened, and nothing is said to a scene that is about to be built anyway.
    const auto bus = add("can.bus", 900.0, 0.0);
    const auto ecu = add("lua.ecu", 300.0, 140.0);
    ASSERT_TRUE(model.attachToBus(ecu, bus));
    description.nodes()[0].x = 900.0;

    model.reload();

    EXPECT_NEAR(
        description.nodes()[0].x, 300.0 - layout::kRailPlateWidth - layout::kRailInset, 0.001);
}

// ---------------------------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------------------------

namespace {

class CanvasBusTest : public testing::Test {
protected:
    CanvasBusTest()
        : catalog{NodeCatalog::withBuiltinTypes()}
        , panel{description, catalog}
    {
        panel.resize(1300, 760);
        panel.show();
        settle();
    }

    ~CanvasBusTest() override { panel.releaseGraph(); }

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
                    settle();
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
        return qobject_cast<QtNodes::BasicGraphicsScene*>(view()->scene());
    }

    [[nodiscard]] PipelineGraphModel* model() const
    {
        return qobject_cast<PipelineGraphModel*>(&scene()->graphModel());
    }

    [[nodiscard]] QtNodes::NodeId idOf(const std::string& id) const
    {
        return model()->canvasId(id);
    }

    [[nodiscard]] QRectF rectOf(const std::string& id) const
    {
        const QtNodes::NodeId node = idOf(id);
        return scene()->nodeGraphicsObject(node)->mapRectToScene(
            QRectF{QPointF{0.0, 0.0}, QSizeF{scene()->nodeGeometry().size(node)}});
    }

    void place(const std::string& id, const QPointF& at)
    {
        model()->setNodeData(idOf(id), QtNodes::NodeRole::Position, at);
        settle();
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

    void drag(const QPointF& fromScene, const QPointF& toScene)
    {
        const QPoint from = view()->mapFromScene(fromScene);
        const QPoint to = view()->mapFromScene(toScene);

        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
        mouse(QEvent::MouseMove, from + (to - from) / 2, Qt::LeftButton);
        mouse(QEvent::MouseMove, to, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
        settle();
    }

    void click(const QPointF& scenePosition)
    {
        const QPoint at = view()->mapFromScene(scenePosition);
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
        settle();
    }

    /// A bus and an ECU on a canvas, the ECU under the bus and not yet wired.
    void aBusAndAnEcuUnderIt()
    {
        addFromPalette("can.bus");
        scene()->clearSelection(); // A block added with the bus selected is put on it.
        addFromPalette("lua.ecu");
        ASSERT_EQ(description.nodes().size(), 2U);

        place(description.nodes()[0].id, QPointF{0.0, 0.0});
        place(description.nodes()[1].id, QPointF{320.0, 160.0});
        scene()->clearSelection();
    }

    [[nodiscard]] QPushButton* button(const char* text) const
    {
        for (auto* candidate : panel.findChildren<QPushButton*>()) {
            if (candidate->text() == QLatin1String(text)) {
                return candidate;
            }
        }
        return nullptr;
    }

    GraphDescription description;
    NodeCatalog catalog;
    CanvasPanel panel;
};

} // namespace

TEST_F(CanvasBusTest, TheBusIsInThePaletteAtTheTop)
{
    auto* palette = panel.findChild<QTreeWidget*>();
    ASSERT_NE(palette, nullptr);
    ASSERT_GT(palette->topLevelItemCount(), 0);

    EXPECT_EQ(palette->topLevelItem(0)->text(0), QStringLiteral("Network"));
    ASSERT_EQ(palette->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(palette->topLevelItem(0)->child(0)->text(0), QStringLiteral("CAN Bus"));
}

TEST_F(CanvasBusTest, ASecondBusIsTheNextChannel)
{
    addFromPalette("can.bus");
    addFromPalette("can.bus");

    ASSERT_EQ(description.nodes().size(), 2U);
    EXPECT_EQ(description.nodes()[0].parameters.integer("channel", -1), 0);
    EXPECT_EQ(description.nodes()[1].parameters.integer("channel", -1), 1);
}

TEST_F(CanvasBusTest, DraggingAModulesOutputOntoTheRailWiresItToTheBus)
{
    aBusAndAnEcuUnderIt();

    // From the output port, on the bottom of the chip, to the middle of the rail: the gesture.
    const QRectF chip = rectOf(description.nodes()[1].id);
    const QRectF rail = rectOf(description.nodes()[0].id);
    drag(QPointF{chip.center().x(), chip.bottom()}, rail.center());

    EXPECT_TRUE(hasEdge(
        description, description.nodes()[1].id.c_str(), 0, description.nodes()[0].id.c_str(), 0));
    // And what it hears: the drop is one thing.
    EXPECT_TRUE(hasEdge(
        description, description.nodes()[0].id.c_str(), 0, description.nodes()[1].id.c_str(), 0));
}

TEST_F(CanvasBusTest, DraggingAModulesInputOntoTheRailWiresItToTheBusToo)
{
    aBusAndAnEcuUnderIt();

    const QRectF chip = rectOf(description.nodes()[1].id);
    const QRectF rail = rectOf(description.nodes()[0].id);
    drag(QPointF{chip.center().x(), chip.top()},
         QPointF{rail.center().x() + 120.0, rail.center().y()});

    EXPECT_EQ(description.edges().size(), 2U);
}

TEST_F(CanvasBusTest, ADragThatEndsOffTheRailWiresNothing)
{
    aBusAndAnEcuUnderIt();

    const QRectF chip = rectOf(description.nodes()[1].id);
    drag(QPointF{chip.center().x(), chip.bottom()},
         QPointF{chip.center().x() + 400.0, chip.bottom() + 200.0});

    EXPECT_TRUE(description.edges().empty());
}

TEST_F(CanvasBusTest, PressingOnTheRailMovesTheBusAndDoesNotStartAWire)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;

    const QRectF rail = rectOf(busId);
    const QPointF before{description.nodes()[0].x, description.nodes()[0].y};
    drag(rail.center(), rail.center() + QPointF{0.0, 70.0});

    EXPECT_TRUE(description.edges().empty());
    EXPECT_NEAR(description.nodes()[0].y, before.y() + 70.0, 2.0);
}

TEST_F(CanvasBusTest, AddingABlockWithTheBusSelectedPutsItOnTheBus)
{
    addFromPalette("can.bus");
    ASSERT_EQ(description.nodes().size(), 1U);
    const std::string busId = description.nodes()[0].id;

    // The bus is the selection a block added is added to.
    scene()->clearSelection();
    scene()->nodeGraphicsObject(idOf(busId))->setSelected(true);
    addFromPalette("lua.ecu");

    ASSERT_EQ(description.nodes().size(), 2U);
    const std::string ecuId = description.nodes()[1].id;
    EXPECT_TRUE(hasEdge(description, busId.c_str(), 0, ecuId.c_str(), 0));
    EXPECT_TRUE(hasEdge(description, ecuId.c_str(), 0, busId.c_str(), 0));

    // Under the rail, a drop away, and on its channel.
    const QRectF rail = rectOf(busId);
    const QRectF chip = rectOf(ecuId);
    EXPECT_NEAR(chip.top(), rail.bottom() + layout::kDropLength, 1.0);
    EXPECT_GE(chip.left(), rail.left() + layout::kRailPlateWidth);
    EXPECT_EQ(description.nodes()[1].parameters.integer("channel", -1), 0);
}

TEST_F(CanvasBusTest, ABlockAddedToTheSameBusTwiceDoesNotLandOnTheFirst)
{
    addFromPalette("can.bus");
    const std::string busId = description.nodes()[0].id;
    scene()->clearSelection();
    scene()->nodeGraphicsObject(idOf(busId))->setSelected(true);

    addFromPalette("lua.ecu");
    scene()->clearSelection();
    scene()->nodeGraphicsObject(idOf(busId))->setSelected(true);
    addFromPalette("lua.ecu");

    ASSERT_EQ(description.nodes().size(), 3U);
    EXPECT_FALSE(rectOf(description.nodes()[1].id).intersects(rectOf(description.nodes()[2].id)));
}

TEST_F(CanvasBusTest, AddingSeveralModulesInARowPutsThemAllOnTheSameBus)
{
    addFromPalette("can.bus"); // The bus is the selection.
    addFromPalette("lua.ecu"); // And now this is: it hangs on the bus.
    addFromPalette("trace.sink");
    addFromPalette("lua.ecu");

    ASSERT_EQ(description.nodes().size(), 4U);
    const std::string busId = description.nodes()[0].id;
    EXPECT_EQ(model()->modulesOn(idOf(busId)).size(), 3U);
}

TEST_F(CanvasBusTest, ABlockWithNoFramesPortIsNotPutOnTheBusItWasAddedNextTo)
{
    addFromPalette("can.bus");
    addFromPalette("signal.plot");

    ASSERT_EQ(description.nodes().size(), 2U);
    EXPECT_TRUE(description.edges().empty());
}

TEST_F(CanvasBusTest, ABlockAddedWithNoBusSelectedIsLeftUnwired)
{
    addFromPalette("can.bus");
    scene()->clearSelection();
    addFromPalette("lua.ecu");

    EXPECT_TRUE(description.edges().empty());
}

TEST_F(CanvasBusTest, ADropCanBeSelectedAndDeletedWithTheKeyboardAndTakesBothHalves)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();
    ASSERT_EQ(description.edges().size(), 2U);

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    ASSERT_NE(scene()->connectionGraphicsObject(wire), nullptr);

    scene()->clearSelection();
    scene()->connectionGraphicsObject(wire)->setSelected(true);
    view()->setFocus();

    QKeyEvent down{QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier};
    QApplication::sendEvent(view(), &down);
    settle();

    EXPECT_TRUE(description.edges().empty());
}

TEST_F(CanvasBusTest, TheWireDrawnToABusIsInsideTheAreaTheSceneRepaintsForIt)
{
    // A drop is painted by the wire's own painter, in an area QtNodes works out from the two ends
    // it knows about. A drop that left that area would be cut off - and so would the pill on it.
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    const QRectF area = object->mapRectToScene(object->boundingRect());
    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);

    const double x = chip.center().x();
    EXPECT_TRUE(area.contains(QPointF{x, chip.top()}));
    EXPECT_TRUE(area.contains(QPointF{x, rail.center().y()}));
    EXPECT_TRUE(area.contains(QPointF{x - 24.0, chip.top() - layout::kDropLength * 0.5}));
    EXPECT_TRUE(area.contains(QPointF{x + 24.0, chip.top() - layout::kDropLength * 0.5}));
}

TEST_F(CanvasBusTest, TheChevronOpensABlockIntoACardAndTheCardsOwnClosesIt)
{
    addFromPalette("lua.ecu");
    ASSERT_EQ(description.nodes().size(), 1U);
    const std::string id = description.nodes()[0].id;
    scene()->nodeGraphicsObject(idOf(id))->setSelected(
        true); // The chevron shows on the selected block.
    settle();

    ASSERT_FALSE(model()->isExpanded(idOf(id)));
    click(layout::chipExpandButton(rectOf(id)).center());
    EXPECT_TRUE(model()->isExpanded(idOf(id)));
    EXPECT_EQ(rectOf(id).size(), QSizeF(layout::kCardWidth, layout::kCardHeight));

    click(layout::cardCollapseButton(rectOf(id)).center());
    EXPECT_FALSE(model()->isExpanded(idOf(id)));
}

TEST_F(CanvasBusTest, DetailsOpensEveryBlockIntoACardAndClosesThemAgain)
{
    aBusAndAnEcuUnderIt();
    QPushButton* details = button("Details");
    ASSERT_NE(details, nullptr);
    ASSERT_TRUE(details->isCheckable());

    details->click();
    settle();

    EXPECT_TRUE(model()->isExpanded(idOf(description.nodes()[1].id)));
    // The bus is not a block that has a card.
    EXPECT_FALSE(model()->isExpanded(idOf(description.nodes()[0].id)));

    // Cards are wider than the room a canvas of chips left: the blocks were arranged for them.
    EXPECT_GE(rectOf(description.nodes()[1].id).width(), layout::kCardWidth);

    details->click();
    settle();
    EXPECT_FALSE(model()->isExpanded(idOf(description.nodes()[1].id)));
}

TEST_F(CanvasBusTest, ArrangePutsTheModulesThatSpeakOverTheBusAndTheOnesThatListenUnderIt)
{
    addFromPalette("can.bus");
    addFromPalette("lua.ecu");
    addFromPalette("trace.sink");
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    const std::string traceId = description.nodes()[2].id;

    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(traceId), QString::fromStdString(busId)));

    QPushButton* arrange = button("Arrange");
    ASSERT_NE(arrange, nullptr);
    arrange->click();
    settle();

    EXPECT_LT(rectOf(ecuId).bottom(), rectOf(busId).top());
    EXPECT_GT(rectOf(traceId).top(), rectOf(busId).bottom());
    EXPECT_FALSE(rectOf(ecuId).intersects(rectOf(traceId)));
}

TEST_F(CanvasBusTest, ArrangeBringsEveryBlockIntoView)
{
    addFromPalette("can.bus");
    for (int i = 0; i < 6; ++i) {
        addFromPalette("lua.ecu");
    }
    const std::string busId = description.nodes()[0].id;
    for (std::size_t i = 1; i < description.nodes().size(); ++i) {
        ASSERT_TRUE(panel.attachToBus(QString::fromStdString(description.nodes()[i].id),
                                      QString::fromStdString(busId)));
    }
    view()->centerOn(QPointF{9000.0, 9000.0});

    button("Arrange")->click();
    settle();

    const QRectF visible = view()->mapToScene(view()->viewport()->rect()).boundingRect();
    for (const auto& node : description.nodes()) {
        EXPECT_TRUE(visible.contains(rectOf(node.id))) << node.id << " is out of view";
    }
}

TEST_F(CanvasBusTest, GroupingTheWiringOnABusReplacesTheBlocksAndShowsTheResult)
{
    // A CAN Channel, an ECU, a CAN Transmit: the wiring a project had before there was a bus.
    NodeDescription source;
    source.id = "can_1";
    source.typeName = "can.source";
    description.addNode(source);

    NodeDescription ecu;
    ecu.id = "ecu";
    ecu.typeName = "lua.ecu";
    description.addNode(ecu);

    NodeDescription transmit;
    transmit.id = "tx";
    transmit.typeName = "can.transmit";
    description.addNode(transmit);

    description.addEdge(EdgeDescription{"can_1", 0, "ecu", 0});
    description.addEdge(EdgeDescription{"ecu", 0, "tx", 0});

    panel.reload();
    settle();

    panel.groupOnBus();
    settle();

    ASSERT_EQ(description.nodes().size(), 2U);
    EXPECT_TRUE(description.find("can_1") != nullptr);
    EXPECT_TRUE(description.find("can_1")->typeName == kCanBusType);
    EXPECT_TRUE(description.find("tx") == nullptr);

    // And the canvas drew it: the bus and the ECU are on it, and the module hangs over the rail.
    EXPECT_NE(idOf("can_1"), QtNodes::InvalidNodeId);
    EXPECT_LT(rectOf("ecu").bottom(), rectOf("can_1").top());
}

TEST_F(CanvasBusTest, TheTinyMlDemoIsBuiltOnABus)
{
    button("TinyML Demo Setup")->click();
    settle();

    int buses = 0;
    for (const auto& node : description.nodes()) {
        if (node.typeName == kCanBusType) {
            ++buses;
        }
        EXPECT_NE(node.typeName, "can.transmit");
        EXPECT_NE(node.typeName, "can.source");
    }
    EXPECT_EQ(buses, 1);

    // The two ECUs listen and speak on it, and the model's plot is fed from the TinyML block alone.
    EXPECT_EQ(model()->modulesOn(idOf("can_1")).size(), 2U);
    EXPECT_TRUE(hasEdge(description, "tinyml_ecu", 1, "tinyml_plot", 0));

    EXPECT_TRUE(description.validate(catalog).succeeded());
}

TEST_F(CanvasBusTest, ARailMovedWithAModuleOnItStillReachesIt)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));

    // The module is taken to the left of where the rail starts to carry it.
    place(ecuId, QPointF{-900.0, 160.0});

    const QRectF rail = rectOf(busId);
    const QRectF chip = rectOf(ecuId);
    EXPECT_GE(chip.left(), rail.left() + layout::kRailPlateWidth);
    EXPECT_GE(rail.right(), chip.right());
}

TEST_F(CanvasBusTest, ArrangingCanBeTakenBackAndDoneAgain)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));

    // Where somebody put them.
    place(ecuId, QPointF{777.0, 333.0});
    const QPointF placed{description.nodes()[1].x, description.nodes()[1].y};
    const QPointF railPlaced{description.nodes()[0].x, description.nodes()[0].y};

    button("Arrange")->click();
    settle();
    const QPointF arranged{description.nodes()[1].x, description.nodes()[1].y};
    ASSERT_NE(arranged, placed);

    scene()->undoStack().undo();
    settle();
    EXPECT_EQ(QPointF(description.nodes()[1].x, description.nodes()[1].y), placed);
    EXPECT_EQ(QPointF(description.nodes()[0].x, description.nodes()[0].y), railPlaced);

    scene()->undoStack().redo();
    settle();
    EXPECT_EQ(QPointF(description.nodes()[1].x, description.nodes()[1].y), arranged);
}

TEST_F(CanvasBusTest, ArrangingAnArrangedDiagramLeavesNothingToUndo)
{
    aBusAndAnEcuUnderIt();
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(description.nodes()[1].id),
                                  QString::fromStdString(description.nodes()[0].id)));

    button("Arrange")->click();
    settle();
    const int steps = scene()->undoStack().count();

    button("Arrange")->click();
    settle();

    EXPECT_EQ(scene()->undoStack().count(), steps);
}

TEST_F(CanvasBusTest, GroupingCanBeTakenBackWholeAndDoneAgain)
{
    NodeDescription source;
    source.id = "can_1";
    source.typeName = "can.source";
    description.addNode(source);

    NodeDescription ecu;
    ecu.id = "ecu";
    ecu.typeName = "lua.ecu";
    description.addNode(ecu);

    NodeDescription transmit;
    transmit.id = "tx";
    transmit.typeName = "can.transmit";
    description.addNode(transmit);

    description.addEdge(EdgeDescription{"can_1", 0, "ecu", 0});
    description.addEdge(EdgeDescription{"ecu", 0, "tx", 0});

    panel.reload();
    settle();
    const GraphDescription original = description;

    panel.groupOnBus();
    settle();
    const GraphDescription grouped = description;
    ASSERT_FALSE(grouped == original);

    scene()->undoStack().undo();
    settle();
    EXPECT_TRUE(description == original);
    EXPECT_NE(idOf("tx"), QtNodes::InvalidNodeId);

    scene()->undoStack().redo();
    settle();
    EXPECT_TRUE(description == grouped);
    EXPECT_EQ(idOf("tx"), QtNodes::InvalidNodeId);
}

TEST_F(CanvasBusTest, OpeningAnotherProjectForgetsWhatCouldHaveBeenUndone)
{
    // The history names blocks by ids, and a project that is opened is given them again.
    aBusAndAnEcuUnderIt();
    button("Arrange")->click();
    settle();
    ASSERT_GT(scene()->undoStack().count(), 0);

    panel.reload();

    EXPECT_EQ(scene()->undoStack().count(), 0);
}

TEST_F(CanvasBusTest, DetailsCanBeTakenBackAndTheButtonFollows)
{
    aBusAndAnEcuUnderIt();
    QPushButton* details = button("Details");
    ASSERT_NE(details, nullptr);
    const std::string ecuId = description.nodes()[1].id;
    const QPointF before{description.nodes()[1].x, description.nodes()[1].y};

    details->click();
    settle();
    ASSERT_TRUE(model()->isExpanded(idOf(ecuId)));
    ASSERT_TRUE(details->isChecked());

    scene()->undoStack().undo();
    settle();

    EXPECT_FALSE(model()->isExpanded(idOf(ecuId)));
    EXPECT_FALSE(details->isChecked());
    EXPECT_EQ(QPointF(description.nodes()[1].x, description.nodes()[1].y), before);
}

TEST_F(CanvasBusTest, TheTinyMlDemoReplacesThePipelineAndCanBeTakenBack)
{
    aBusAndAnEcuUnderIt();
    const GraphDescription original = description;

    button("TinyML Demo Setup")->click();
    settle();
    ASSERT_FALSE(description == original);

    scene()->undoStack().undo();
    settle();

    EXPECT_TRUE(description == original);
}

TEST_F(CanvasBusTest, ClickingThePillOnADropGoesRoundAllElevenBitAndTwentyNineBit)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    // The pill is shown for a wire that is selected - there is something to click - and not
    // otherwise for one that carries everything.
    scene()->clearSelection();
    object->setSelected(true);
    settle();

    const QPointF pill = frameFormatPillRect(*object, *model()).center();
    ASSERT_FALSE(frameFormatPillRect(*object, *model()).isNull());

    EXPECT_EQ(model()->frameFormat(wire), CanFormatMatch::Any);
    click(pill);
    EXPECT_EQ(model()->frameFormat(wire), CanFormatMatch::StandardOnly);
    click(pill);
    EXPECT_EQ(model()->frameFormat(wire), CanFormatMatch::ExtendedOnly);
    click(pill);
    EXPECT_EQ(model()->frameFormat(wire), CanFormatMatch::Any);

    // And the project says so: the description is what is saved.
    ASSERT_EQ(description.edges().size(), 2U);
}

TEST_F(CanvasBusTest, ThePillIsPartOfTheWireItIsOnSoThePointerStaysOnTheWireWhileItGoesToClickIt)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    const QRectF pill = frameFormatPillRect(*object, *model());
    ASSERT_FALSE(pill.isNull());

    // The ends of the pill, which are further from the line than the line's own stroke reaches.
    for (const double x : {pill.left() + 3.0, pill.right() - 3.0}) {
        EXPECT_TRUE(object->shape().contains(object->mapFromScene(QPointF{x, pill.center().y()})))
            << x;
    }
}

// ---------------------------------------------------------------------------------------------
// A drop is the two wires of a CAN bus
// ---------------------------------------------------------------------------------------------
//
// A module is wired to a bus by dragging one wire to it, and what is drawn is the pair that wire
// stands for: CAN_H in amber, on the left, joined to the amber wire of the bus, and CAN_L in
// green, on the right, joined to the green one. The geometry is a pure function of where the
// module and the bus are (`layout::dropWire`), so it is tested for every way the two can be
// placed; what is drawn from it is tested through the scene.

namespace {

[[nodiscard]] double distanceFromSegment(const QPointF& point, const QPointF& a, const QPointF& b)
{
    const QPointF along = b - a;
    const double length2 = along.x() * along.x() + along.y() * along.y();
    double t = 0.0;
    if (length2 > 0.0) {
        t = ((point.x() - a.x()) * along.x() + (point.y() - a.y()) * along.y()) / length2;
    }
    t = std::clamp(t, 0.0, 1.0);

    const QPointF closest = a + along * t;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

[[nodiscard]] double orientation(const QPointF& a, const QPointF& b, const QPointF& c)
{
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

[[nodiscard]] bool
segmentsCross(const QPointF& a, const QPointF& b, const QPointF& c, const QPointF& d)
{
    const double first = orientation(a, b, c);
    const double second = orientation(a, b, d);
    const double third = orientation(c, d, a);
    const double fourth = orientation(c, d, b);

    return ((first > 0.0) != (second > 0.0)) && ((third > 0.0) != (fourth > 0.0)) && first != 0.0
           && second != 0.0 && third != 0.0 && fourth != 0.0;
}

/// The closest two lines made of straight segments come to each other: nothing if they cross, and
/// otherwise the shortest of the distances from the corner of one to the segments of the other,
/// which is where the shortest distance between two lines that do not cross always is.
[[nodiscard]] double closestApproach(const std::vector<QPointF>& first,
                                     const std::vector<QPointF>& second)
{
    for (std::size_t i = 1; i < first.size(); ++i) {
        for (std::size_t j = 1; j < second.size(); ++j) {
            if (segmentsCross(first[i - 1], first[i], second[j - 1], second[j])) {
                return 0.0;
            }
        }
    }

    double nearest = std::numeric_limits<double>::max();
    for (const QPointF& corner : first) {
        for (std::size_t j = 1; j < second.size(); ++j) {
            nearest = std::min(nearest, distanceFromSegment(corner, second[j - 1], second[j]));
        }
    }
    for (const QPointF& corner : second) {
        for (std::size_t i = 1; i < first.size(); ++i) {
            nearest = std::min(nearest, distanceFromSegment(corner, first[i - 1], first[i]));
        }
    }
    return nearest;
}

struct DropPlacement final {
    const char* name;
    double x; ///< The middle of the module.
    double edge; ///< The side of the module the drop leaves from.
    double landing; ///< Where the pair arrives at the bus.
    double busMiddle;
};

/// Over the bus and under it, straight and with a step to either side.
constexpr DropPlacement kPlacements[] = {
    {"over the bus, straight down", 400.0, 100.0, 400.0, 200.0},
    {"under the bus, straight up", 400.0, 300.0, 400.0, 200.0},
    {"over the bus, a step to the right", 100.0, 100.0, 260.0, 200.0},
    {"over the bus, a step to the left", 400.0, 100.0, 260.0, 200.0},
    {"under the bus, a step to the right", 100.0, 300.0, 260.0, 200.0},
    {"under the bus, a step to the left", 400.0, 300.0, 260.0, 200.0},
    {"over the bus, a step of a few pixels", 400.0, 100.0, 403.0, 200.0},
    {"close to the bus, a step to the right", 100.0, 150.0, 260.0, 200.0},
};

} // namespace

TEST(DropGeometryTest, EachWireLeavesTheModuleEitherSideOfItsMiddleAndJoinsItsOwnWireOfTheBus)
{
    constexpr double d = layout::kDropWireOffset;
    constexpr double r = layout::kRailWireOffset;

    for (const DropPlacement& at : kPlacements) {
        SCOPED_TRACE(at.name);

        const auto high = layout::dropWire(true, at.x, at.edge, at.landing, at.busMiddle);
        const auto low = layout::dropWire(false, at.x, at.edge, at.landing, at.busMiddle);
        ASSERT_GE(high.size(), 2U);
        ASSERT_GE(low.size(), 2U);

        // CAN_H on the left of the module's middle, CAN_L on the right, both on the side it leaves.
        EXPECT_DOUBLE_EQ(high.front().x(), at.x - d);
        EXPECT_DOUBLE_EQ(low.front().x(), at.x + d);
        EXPECT_DOUBLE_EQ(high.front().y(), at.edge);
        EXPECT_DOUBLE_EQ(low.front().y(), at.edge);

        // Each ends on the wire of the bus that is its own: CAN_H the upper, CAN_L the lower.
        const double arrival = std::abs(at.landing - at.x) < 0.5 ? at.x : at.landing;
        EXPECT_DOUBLE_EQ(high.back().x(), arrival - d);
        EXPECT_DOUBLE_EQ(low.back().x(), arrival + d);
        EXPECT_DOUBLE_EQ(high.back().y(), at.busMiddle - r);
        EXPECT_DOUBLE_EQ(low.back().y(), at.busMiddle + r);
    }
}

TEST(DropGeometryTest, TheTwoWiresAreStraightLinesAndNeverComeCloserThanTheyStartApart)
{
    for (const DropPlacement& at : kPlacements) {
        SCOPED_TRACE(at.name);

        const auto high = layout::dropWire(true, at.x, at.edge, at.landing, at.busMiddle);
        const auto low = layout::dropWire(false, at.x, at.edge, at.landing, at.busMiddle);

        for (const auto& wire : {high, low}) {
            for (std::size_t i = 1; i < wire.size(); ++i) {
                const bool vertical = wire[i].x() == wire[i - 1].x();
                const bool horizontal = wire[i].y() == wire[i - 1].y();
                EXPECT_TRUE(vertical || horizontal)
                    << "a slanted line from " << wire[i - 1].x() << "," << wire[i - 1].y() << " to "
                    << wire[i].x() << "," << wire[i].y();
            }
        }

        // The same distance apart on the way down as at the module, and round a step too: a pair
        // that pinched at a corner would be two wires touching, which is a short.
        EXPECT_NEAR(closestApproach(high, low), 2.0 * layout::kDropWireOffset, 1e-9);
    }
}

TEST(DropGeometryTest, TheWireThatHasTheFurtherToGoPassesTheOtherWireOfTheBusWithoutJoiningIt)
{
    constexpr double r = layout::kRailWireOffset;

    for (const DropPlacement& at : kPlacements) {
        SCOPED_TRACE(at.name);

        const bool over = at.edge < at.busMiddle;
        const auto far = layout::dropWire(!over, at.x, at.edge, at.landing, at.busMiddle);

        // Over the bus the near wire is CAN_H, so it is CAN_L that goes on to the lower one; under
        // it, the other way round. The last line of that wire runs from before the near wire of
        // the bus to past it.
        const double nearWire = at.busMiddle + (over ? -r : r);
        const QPointF from = far[far.size() - 2];
        const QPointF to = far.back();

        EXPECT_DOUBLE_EQ(from.x(), to.x());
        EXPECT_LT(std::min(from.y(), to.y()), nearWire);
        EXPECT_GT(std::max(from.y(), to.y()), nearWire);
    }
}

TEST(DropGeometryTest, ADropThatComesStraightDownIsTwoPointsPerWireAndAStepIsFour)
{
    const auto straight = layout::dropWire(true, 400.0, 100.0, 400.0, 200.0);
    EXPECT_EQ(straight.size(), 2U);
    EXPECT_EQ(layout::dropCentre(400.0, 100.0, 400.0, 200.0).size(), 2U);

    const auto stepped = layout::dropWire(true, 100.0, 100.0, 260.0, 200.0);
    EXPECT_EQ(stepped.size(), 4U);
    EXPECT_EQ(layout::dropCentre(100.0, 100.0, 260.0, 200.0).size(), 4U);
}

TEST_F(CanvasBusTest, AModuleOnTheBusHangsOffItByTheTwoWiresOfTheBus)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    const WireShape shape = wireShape(*object, *model());
    ASSERT_TRUE(shape.isTap);

    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);
    const double x = chip.center().x();
    const double middle = rail.center().y();
    constexpr double d = layout::kDropWireOffset;
    constexpr double r = layout::kRailWireOffset;

    ASSERT_FALSE(shape.wires[kCanHigh].path.isEmpty());
    ASSERT_FALSE(shape.wires[kCanLow].path.isEmpty());

    // The ECU is under the bus: both wires leave the top of the chip, CAN_H on the left and CAN_L
    // on the right, and each ends on the wire of the bus that is the same colour.
    const QPointF highFrom = object->mapToScene(shape.wires[kCanHigh].moduleEnd);
    const QPointF highTo = object->mapToScene(shape.wires[kCanHigh].railEnd);
    const QPointF lowFrom = object->mapToScene(shape.wires[kCanLow].moduleEnd);
    const QPointF lowTo = object->mapToScene(shape.wires[kCanLow].railEnd);

    EXPECT_NEAR(highFrom.x(), x - d, 0.01);
    EXPECT_NEAR(highFrom.y(), chip.top(), 0.01);
    EXPECT_NEAR(lowFrom.x(), x + d, 0.01);
    EXPECT_NEAR(lowFrom.y(), chip.top(), 0.01);

    EXPECT_NEAR(highTo.x(), x - d, 0.01);
    EXPECT_NEAR(highTo.y(), middle - r, 0.01);
    EXPECT_NEAR(lowTo.x(), x + d, 0.01);
    EXPECT_NEAR(lowTo.y(), middle + r, 0.01);

    // And the paths really run between those ends.
    const QPainterPath high = object->mapToScene(shape.wires[kCanHigh].path);
    const QPainterPath low = object->mapToScene(shape.wires[kCanLow].path);
    EXPECT_NEAR(high.pointAtPercent(0.5).x(), x - d, 0.01);
    EXPECT_NEAR(low.pointAtPercent(0.5).x(), x + d, 0.01);
}

TEST_F(CanvasBusTest, AModuleOverTheBusHangsOffItFromItsBottomEdge)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    place(ecuId, QPointF{320.0, -200.0});
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    const WireShape shape = wireShape(*object, *model());
    ASSERT_TRUE(shape.isTap);
    EXPECT_FALSE(shape.moduleBelow);

    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);
    ASSERT_LT(chip.bottom(), rail.top());

    const QPointF highFrom = object->mapToScene(shape.wires[kCanHigh].moduleEnd);
    const QPointF lowFrom = object->mapToScene(shape.wires[kCanLow].moduleEnd);
    const QPointF highTo = object->mapToScene(shape.wires[kCanHigh].railEnd);
    const QPointF lowTo = object->mapToScene(shape.wires[kCanLow].railEnd);

    EXPECT_NEAR(highFrom.y(), chip.bottom(), 0.01);
    EXPECT_NEAR(lowFrom.y(), chip.bottom(), 0.01);

    // From above, CAN_H is the nearer wire of the bus and CAN_L goes past it to the lower one.
    EXPECT_NEAR(highTo.y(), rail.center().y() - layout::kRailWireOffset, 0.01);
    EXPECT_NEAR(lowTo.y(), rail.center().y() + layout::kRailWireOffset, 0.01);
    EXPECT_LT(highFrom.x(), lowFrom.x());
}

TEST_F(CanvasBusTest, TheShapeOfADropTakesInBothWiresAndWhatIsBetweenThem)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    settle();

    const QtNodes::ConnectionId wire{idOf(busId), 0, idOf(ecuId), 0};
    QtNodes::ConnectionGraphicsObject* object = scene()->connectionGraphicsObject(wire);
    ASSERT_NE(object, nullptr);

    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);
    const double x = chip.center().x();
    const double y = (chip.top() + rail.center().y()) * 0.5;

    // Either wire picks the drop up, a little beside it as a thin line is, and so does the gap
    // between them: the pair is one thing.
    const QPainterPath shape = object->shape();
    constexpr double d = layout::kDropWireOffset;
    for (const double offset : {-(d + 3.0), -d, 0.0, d, d + 3.0}) {
        EXPECT_TRUE(shape.contains(object->mapFromScene(QPointF{x + offset, y}))) << offset;
    }

    // And not what is a little way beside it.
    for (const double offset : {-20.0, 20.0}) {
        EXPECT_FALSE(shape.contains(object->mapFromScene(QPointF{x + offset, y}))) << offset;
    }
}

TEST_F(CanvasBusTest, TheWiresOfADropAreDrawnInTheColoursOfTheWiresOfTheBusInEveryTheme)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    scene()->clearSelection();
    settle();

    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);
    const double x = chip.center().x();
    const double y = chip.top() - 0.0 + (rail.center().y() - chip.top()) * 0.45;

    constexpr int kScale = 8;
    const QRectF area{x - 20.0, y - 2.0, 40.0, 4.0};

    const auto sample = [&](double atX) {
        QImage picture{static_cast<int>(area.width()) * kScale,
                       static_cast<int>(area.height()) * kScale,
                       QImage::Format_ARGB32_Premultiplied};
        picture.fill(Qt::transparent);
        {
            QPainter painter{&picture};
            painter.setRenderHint(QPainter::Antialiasing);
            scene()->render(&painter,
                            QRectF{0.0, 0.0, double(picture.width()), double(picture.height())},
                            area);
        }
        return picture.pixelColor(static_cast<int>((atX - area.left()) * kScale),
                                  static_cast<int>((y - area.top()) * kScale));
    };

    const auto close = [](const QColor& a, const QColor& b) {
        return std::abs(a.red() - b.red()) <= 3 && std::abs(a.green() - b.green()) <= 3
               && std::abs(a.blue() - b.blue()) <= 3;
    };

    for (const Theme& theme : {Theme::dark(), Theme::light()}) {
        panel.applyTheme(theme);
        settle();

        const QColor onTheLeft = sample(x - layout::kDropWireOffset);
        const QColor onTheRight = sample(x + layout::kDropWireOffset);

        EXPECT_TRUE(close(onTheLeft, theme.warning))
            << "CAN_H is " << onTheLeft.name().toStdString() << ", the bus's is "
            << theme.warning.name().toStdString();
        EXPECT_TRUE(close(onTheRight, theme.success))
            << "CAN_L is " << onTheRight.name().toStdString() << ", the bus's is "
            << theme.success.name().toStdString();

        // Beside the pair there is nothing of either colour.
        for (const double beside : {-14.0, 14.0}) {
            const QColor colour = sample(x + beside);
            EXPECT_FALSE(close(colour, theme.warning)) << beside;
            EXPECT_FALSE(close(colour, theme.success)) << beside;
        }
    }
}

TEST_F(CanvasBusTest, WhileRunningBothWiresOfADropGlowInTheirOwnColours)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;
    ASSERT_TRUE(panel.attachToBus(QString::fromStdString(ecuId), QString::fromStdString(busId)));
    scene()->clearSelection();
    settle();

    // The middle of the drop, well clear of the chip (which has a halo while it runs) and of the
    // bus (which has lights of its own), so that what is seen here is the drop's.
    const QRectF chip = rectOf(ecuId);
    const QRectF rail = rectOf(busId);
    const double x = chip.center().x();
    const double y = (chip.top() + rail.center().y()) * 0.5;
    constexpr double d = layout::kDropWireOffset;

    constexpr int kScale = 8;
    const QRectF area{x - 20.0, y - 2.0, 40.0, 4.0};

    const auto pixelAt = [&](double atX) {
        QImage image{static_cast<int>(area.width()) * kScale,
                     static_cast<int>(area.height()) * kScale,
                     QImage::Format_ARGB32_Premultiplied};
        image.fill(Qt::transparent);
        {
            QPainter painter{&image};
            painter.setRenderHint(QPainter::Antialiasing);
            scene()->render(
                &painter, QRectF{0.0, 0.0, double(image.width()), double(image.height())}, area);
        }
        return image.pixelColor(static_cast<int>((atX - area.left()) * kScale),
                                static_cast<int>((y - area.top()) * kScale));
    };

    // Just beside each wire, where the wire itself is not: nothing while it is stopped.
    const double besideAmber = x - d - 1.5;
    const double besideGreen = x + d + 1.5;
    EXPECT_EQ(pixelAt(besideAmber).alpha(), 0);
    EXPECT_EQ(pixelAt(besideGreen).alpha(), 0);

    // While running, the foreground lays a glow along each wire, in the colour of that wire - amber
    // on the left, green on the right - and the two are not the one colour of a single line.
    panel.setSimulationRunning(true);
    settle();

    const QColor amber = pixelAt(besideAmber);
    const QColor green = pixelAt(besideGreen);
    EXPECT_GT(amber.alpha(), 0);
    EXPECT_GT(green.alpha(), 0);
    EXPECT_GT(amber.red(), amber.blue() + 40) << amber.name().toStdString();
    EXPECT_GT(green.green(), green.red() + 20) << green.name().toStdString();
}

TEST_F(CanvasBusTest, WhatTheBusDeliversIsDrawnAboveWhatTheModuleSendsWhicheverWasWiredFirst)
{
    // The two halves of a drop are two items on one pair of wires, and the one with the pill on it
    // has to be painted last or the other's wires are drawn across the pill. Which was created
    // first is not the canvas's to say - it is which end somebody dragged from - so the order is
    // made.
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;

    // The bus delivers first: wired to the module's input.
    model()->addConnection(QtNodes::ConnectionId{idOf(busId), 0, idOf(ecuId), 0});
    settle();
    ASSERT_EQ(description.edges().size(), 2U);

    const auto* delivers =
        scene()->connectionGraphicsObject(QtNodes::ConnectionId{idOf(busId), 0, idOf(ecuId), 0});
    const auto* sends =
        scene()->connectionGraphicsObject(QtNodes::ConnectionId{idOf(ecuId), 0, idOf(busId), 0});
    ASSERT_NE(delivers, nullptr);
    ASSERT_NE(sends, nullptr);

    EXPECT_GT(delivers->zValue(), sends->zValue());
    EXPECT_LT(delivers->zValue(), 0.0) << "still below every block";
}

TEST_F(CanvasBusTest, WhatTheBusDeliversIsAboveWhenTheModuleSendingWasWiredFirstToo)
{
    aBusAndAnEcuUnderIt();
    const std::string busId = description.nodes()[0].id;
    const std::string ecuId = description.nodes()[1].id;

    // The module sends first: dragged from its output to the rail.
    model()->addConnection(QtNodes::ConnectionId{idOf(ecuId), 0, idOf(busId), 0});
    settle();
    ASSERT_EQ(description.edges().size(), 2U);

    const auto* delivers =
        scene()->connectionGraphicsObject(QtNodes::ConnectionId{idOf(busId), 0, idOf(ecuId), 0});
    const auto* sends =
        scene()->connectionGraphicsObject(QtNodes::ConnectionId{idOf(ecuId), 0, idOf(busId), 0});
    ASSERT_NE(delivers, nullptr);
    ASSERT_NE(sends, nullptr);

    EXPECT_GT(delivers->zValue(), sends->zValue());
}

TEST_F(CanvasBusTest, TheSameHoldsForAProjectThatIsOpenedWithEitherOrderOfTheTwoHalves)
{
    // Opened, not drawn: the items are made when the model is reset, and not one wire at a time.
    for (const bool deliversFirst : {true, false}) {
        GraphDescription graph;

        NodeDescription bus;
        bus.id = "bus";
        bus.typeName = "can.bus";
        graph.addNode(bus);

        NodeDescription ecu;
        ecu.id = "ecu";
        ecu.typeName = "lua.ecu";
        ecu.y = 160.0;
        graph.addNode(ecu);

        if (deliversFirst) {
            graph.addEdge(EdgeDescription{"bus", 0, "ecu", 0});
            graph.addEdge(EdgeDescription{"ecu", 0, "bus", 0});
        } else {
            graph.addEdge(EdgeDescription{"ecu", 0, "bus", 0});
            graph.addEdge(EdgeDescription{"bus", 0, "ecu", 0});
        }

        CanvasPanel opened{graph, catalog};
        opened.resize(900, 600);
        opened.show();
        settle();

        auto* openedView = opened.findChild<QtNodes::GraphicsView*>();
        ASSERT_NE(openedView, nullptr);
        auto* openedScene = qobject_cast<QtNodes::BasicGraphicsScene*>(openedView->scene());
        ASSERT_NE(openedScene, nullptr);
        auto* openedModel = qobject_cast<PipelineGraphModel*>(&openedScene->graphModel());
        ASSERT_NE(openedModel, nullptr);

        const QtNodes::NodeId busNode = openedModel->canvasId("bus");
        const QtNodes::NodeId ecuNode = openedModel->canvasId("ecu");

        const auto* delivers =
            openedScene->connectionGraphicsObject(QtNodes::ConnectionId{busNode, 0, ecuNode, 0});
        const auto* sends =
            openedScene->connectionGraphicsObject(QtNodes::ConnectionId{ecuNode, 0, busNode, 0});
        ASSERT_NE(delivers, nullptr) << deliversFirst;
        ASSERT_NE(sends, nullptr) << deliversFirst;

        EXPECT_GT(delivers->zValue(), sends->zValue()) << deliversFirst;

        opened.releaseGraph();
    }
}
