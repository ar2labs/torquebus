// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A canvas that is handed a graph nobody would draw.
//
// The description is plain data and anything can write it: a project file somebody edited by
// hand, a plugin that registered a type this build does not have, a script, an older version.
// The loader refuses what it can, but the canvas is the last thing between a malformed graph
// and a person with a window open, and it is built from the description and not from the
// loader. So it is tested with the description directly, holding everything the loader
// would have refused: a block of a type that does not exist, a wire to a port the block does
// not have, the same wire twice, a block wired to itself, two wires into one input, a wire
// that joins things of different kinds, a wire to a block that is not there.
//
// The standard is the one the rest of the pipeline holds: it opens, it draws, it can be
// arranged and edited and emptied, and anything it cannot show it leaves out - it does not
// stop.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/canvas/CanvasPanel.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QImage>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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

[[nodiscard]] NodeDescription block(std::string id, std::string type, double x, double y)
{
    NodeDescription node;
    node.id = std::move(id);
    node.typeName = std::move(type);
    node.x = x;
    node.y = y;
    return node;
}

/// Everything a loader would have refused, in one description.
[[nodiscard]] GraphDescription hostileGraph()
{
    GraphDescription graph;

    graph.addNode(block("can_1", "can.source", 0, 0));
    graph.addNode(block("can_2", "can.source", 0, 140));
    graph.addNode(block("trace", "trace.sink", 400, 0));
    graph.addNode(block("decoder", "dbc.decoder", 200, 0));
    graph.addNode(block("ecu", "lua.ecu", 200, 280));
    graph.addNode(block("bus", "can.bus", 0, 420));
    graph.addNode(block("ghost", "plugin.that.is.not.installed", 600, 0));

    // A block with a settings value of the wrong kind, which the canvas reads for its card.
    NodeDescription odd = block("odd", "lua.ecu", 600, 280);
    odd.parameters.set("channel", ParameterValue::fromText("two"));
    odd.parameters.set("script", ParameterValue::fromInteger(5));
    graph.addNode(odd);

    // The good wire, and then the same one again.
    graph.addEdge(EdgeDescription{"can_1", 0, "trace", 0});
    graph.addEdge(EdgeDescription{"can_1", 0, "trace", 0});

    // A second wire into an input that takes one.
    graph.addEdge(EdgeDescription{"can_2", 0, "trace", 0});

    // From a port the block does not have, and into one.
    graph.addEdge(EdgeDescription{"can_1", 7, "trace", 0});
    graph.addEdge(EdgeDescription{"can_1", 0, "trace", 9});

    // A block wired to itself.
    graph.addEdge(EdgeDescription{"ecu", 0, "ecu", 0});

    // Frames into a block that takes signals, and signals into one that takes frames.
    graph.addEdge(EdgeDescription{"decoder", 0, "trace", 0});

    // To and from a block of a type nobody has.
    graph.addEdge(EdgeDescription{"ghost", 0, "trace", 0});
    graph.addEdge(EdgeDescription{"can_1", 0, "ghost", 0});

    // To a block that is not in the description at all.
    graph.addEdge(EdgeDescription{"can_1", 0, "nobody", 0});
    graph.addEdge(EdgeDescription{"nobody", 0, "trace", 0});

    // A format limit on a wire that carries signals, and one on a bus.
    graph.addEdge(EdgeDescription{"decoder", 0, "ecu", 0, CanFormatMatch::ExtendedOnly});
    graph.addEdge(EdgeDescription{"bus", 0, "bus", 0});
    graph.addEdge(EdgeDescription{"bus", 0, "ecu", 0, CanFormatMatch::StandardOnly});
    graph.addEdge(EdgeDescription{"ecu", 0, "bus", 0});
    graph.addEdge(EdgeDescription{"odd", 0, "bus", 0});

    return graph;
}

class CanvasRobustnessTest : public testing::Test {
protected:
    ~CanvasRobustnessTest() override
    {
        if (panel != nullptr) {
            panel->releaseGraph();
        }
    }

    void open(GraphDescription initial)
    {
        description = std::move(initial);
        panel = std::make_unique<CanvasPanel>(description, catalog);
        panel->resize(1300, 760);
        panel->show();
        settle();
    }

    [[nodiscard]] QtNodes::BasicGraphicsScene* scene() const
    {
        auto* view = panel->findChild<QtNodes::GraphicsView*>();
        return view != nullptr ? qobject_cast<QtNodes::BasicGraphicsScene*>(view->scene())
                               : nullptr;
    }

    void selectEverything() const
    {
        for (QGraphicsItem* item : scene()->items()) {
            item->setSelected(true);
        }
    }

    NodeCatalog catalog{NodeCatalog::withBuiltinTypes()};
    GraphDescription description;
    std::unique_ptr<CanvasPanel> panel;
};

} // namespace

TEST_F(CanvasRobustnessTest, AGraphNobodyWouldDrawOpensAndPaints)
{
    ASSERT_NO_THROW(open(hostileGraph()));
    ASSERT_NE(scene(), nullptr);

    // The blocks are all there, the ones of a type nobody has included: a block this build
    // cannot describe is still a block in the file, and leaving it off the canvas would
    // delete it from the project on the next save.
    EXPECT_EQ(description.nodes().size(), 8U);

    const QImage picture = panel->grab().toImage();
    EXPECT_FALSE(picture.isNull());

    ASSERT_NO_THROW(panel->reload());
    settle();
}

TEST_F(CanvasRobustnessTest, TheDescriptionIsNotRewrittenJustByOpeningIt)
{
    // Opening a project is not an edit. A canvas that tidied the wires it did not like
    // would change a file the person had only looked at.
    GraphDescription graph = hostileGraph();
    const std::size_t wires = graph.edges().size();
    const std::size_t blocks = graph.nodes().size();

    open(std::move(graph));

    EXPECT_EQ(description.nodes().size(), blocks);
    EXPECT_EQ(description.edges().size(), wires);
}

TEST_F(CanvasRobustnessTest, ASceneBuiltFromAGraphLikeThatCanBeArrangedAndOpenedAndClosed)
{
    open(hostileGraph());

    EXPECT_NO_THROW(panel->arrangeBlocks());
    settle();

    EXPECT_NO_THROW(panel->setDetailsShown(true));
    settle();
    EXPECT_FALSE(panel->grab().toImage().isNull());

    EXPECT_NO_THROW(panel->setDetailsShown(false));
    settle();

    EXPECT_NO_THROW(panel->groupOnBus());
    settle();
}

TEST_F(CanvasRobustnessTest, EverythingOnTheCanvasCanBeSelectedAndDeleted)
{
    open(hostileGraph());

    selectEverything();
    ASSERT_NO_THROW(panel->deleteSelectedItems());
    settle();

    // What the canvas did not show it cannot have deleted, and what it did is gone - the
    // test is that this is reached.
    EXPECT_LT(description.nodes().size(), 8U);
}

TEST_F(CanvasRobustnessTest, ABlockCanBeDeletedByNameWhetherOrNotItIsThere)
{
    open(hostileGraph());

    EXPECT_NO_THROW(panel->deleteBlock(QStringLiteral("ghost")));
    EXPECT_NO_THROW(panel->deleteBlock(QStringLiteral("nobody")));
    EXPECT_NO_THROW(panel->deleteBlock(QString{}));
    settle();

    EXPECT_EQ(description.find("ghost"), nullptr);
}

TEST_F(CanvasRobustnessTest, WiringToABusThatIsNotThereOrABlockThatIsNotThereSaysNo)
{
    open(hostileGraph());

    EXPECT_FALSE(panel->attachToBus(QStringLiteral("nobody"), QStringLiteral("bus")));
    EXPECT_FALSE(panel->attachToBus(QStringLiteral("ecu"), QStringLiteral("nobody")));
    EXPECT_FALSE(panel->attachToBus(QStringLiteral("ecu"), QStringLiteral("trace")))
        << "a trace is not a bus";
    EXPECT_FALSE(panel->attachToBus(QStringLiteral("ghost"), QStringLiteral("bus")))
        << "a block of a type nobody has cannot be wired";

    EXPECT_NO_THROW(panel->detachFromBus(QStringLiteral("nobody"), QStringLiteral("bus")));
    EXPECT_NO_THROW(panel->detachFromBus(QStringLiteral("ecu"), QStringLiteral("nobody")));
}

TEST_F(CanvasRobustnessTest, ALargePipelineOpensInReasonableTime)
{
    // Not a benchmark: a floor. Arranging used to move each block on its own, and moving a
    // block lays out its bus and redraws every wire on it - so the time went with the cube of
    // the number of modules: five and a half seconds for these eighty, in a debug build, and
    // eight times that for a hundred and sixty. A person with a big project would have
    // thought the window had hung.
    GraphDescription graph;
    graph.addNode(block("bus", "can.bus", 0, 0));

    constexpr int kModules = 80;
    for (int index = 0; index < kModules; ++index) {
        const std::string id = "ecu_" + std::to_string(index);
        graph.addNode(block(id, "lua.ecu", 200.0 * (index % 20), 160.0 + 120.0 * (index / 20)));
        graph.addEdge(EdgeDescription{"bus", 0, id, 0});
        graph.addEdge(EdgeDescription{id, 0, "bus", 0});
    }

    QElapsedTimer clock;
    clock.start();

    open(std::move(graph));
    panel->arrangeBlocks();
    settle();

    EXPECT_LT(clock.elapsed(), 6000)
        << "opening and arranging 80 modules took " << clock.elapsed() << " ms";
}

// ---------------------------------------------------------------------------------------------
// The one correction made to the library the canvas is built on
// ---------------------------------------------------------------------------------------------

TEST(QtNodesCorrectionTest, DraggingAWireDoesNotReadTheViewTransformOutOfTheViewport)
{
    // QtNodes 3.0.16 casts the widget of a scene event - the viewport - to a QGraphicsView and
    // asks it for its transform, on every mouse move of a wire being dragged. That reads several
    // hundred bytes past the end of the object, which is nearly always other live memory and now
    // and then the end of a page: an access violation, once in a few thousand drags, found as a
    // test that failed once in a thousand runs. The cast is replaced when the library is fetched
    // (cmake/PatchQtNodes.cmake), and this is the check that it still is - a drag cannot show it,
    // because the result of the cast is only wrong, not different, nearly every time.
    const std::filesystem::path source = std::filesystem::path{TORQUEBUS_QTNODES_SOURCE_DIR} / "src"
                                         / "ConnectionGraphicsObject.cpp";

    std::ifstream file{source};
    ASSERT_TRUE(file.is_open()) << source.string();

    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    EXPECT_EQ(text.find("static_cast<QGraphicsView *>(event->widget())"), std::string::npos)
        << "ConnectionGraphicsObject still casts the viewport to a QGraphicsView";
    EXPECT_NE(text.find("TorqueBus: the widget of a scene event is the viewport"),
              std::string::npos)
        << "the correction in cmake/PatchQtNodes.cmake was not applied to " << source.string();
}
