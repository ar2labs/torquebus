// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the window believes about the measurement, against what the engine is doing.
//
// The two can part. Something that throws on the dispatch thread - and is not a node, which the
// graph now takes out of the run by itself - ends the measurement from inside the engine: the
// guard around the thread reports it and lets the thread return, and nobody pressed Stop. The
// window used to go on showing a measurement that was not there: the timer running, started()
// emitted and stopped() never, and a second Start on an engine that had not been wound up.

#include "core/can/CanEngine.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/PortType.h"
#include "drivers/virtual/VirtualCanBackend.h"
#include "ui/engine/CanEngineController.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

/// A node that does nothing, so that the graph has something wired to the channel.
class IdleNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.idle"; }
    [[nodiscard]] std::string displayName() const override { return "idle"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext&) override { }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
};

bool waitFor(const std::function<bool()>& done)
{
    QElapsedTimer clock;
    clock.start();

    while (!done()) {
        if (clock.elapsed() > 3000) {
            return false;
        }

        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    return true;
}

} // namespace

TEST(EngineControllerTests, AMeasurementThatEndsOnItsOwnIsStoppedOnTheScreenToo)
{
    CanEngineController controller;

    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(
        controller.engine().addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    controller.engine().setGraphBuilder(
        [](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
            const NodeId node = graph.addNode(std::make_unique<IdleNode>());
            return graph.connect(PortRef{sources[0], 0}, PortRef{node, 0});
        });

    // What ends the loop: a statistics consumer that throws, on the dispatch thread.
    std::atomic<bool> throws{true};
    controller.engine().addStatisticsSink([&throws](std::span<const CanStatisticsSnapshot>) {
        if (throws.load()) {
            throw std::runtime_error{"a sink gave up"};
        }
    });

    int started = 0;
    int stopped = 0;
    QObject::connect(&controller, &CanEngineController::started, [&started] { ++started; });
    QObject::connect(&controller, &CanEngineController::stopped, [&stopped] { ++stopped; });

    controller.start();
    ASSERT_EQ(started, 1);

    // Nobody presses Stop. The sink throws, the engine reports it and stops the loop, and the
    // window finds out on its next tick.
    ASSERT_TRUE(waitFor([&stopped] { return stopped > 0; })) << "the window never noticed";
    EXPECT_EQ(stopped, 1);
    EXPECT_FALSE(controller.isRunning());

    // And a second measurement - with the fault gone - starts cleanly on an engine that was wound
    // up, instead of terminating the process over a thread that was never joined.
    throws = false;
    controller.start();
    EXPECT_EQ(started, 2);
    EXPECT_TRUE(controller.isRunning());

    controller.stop();
    EXPECT_EQ(stopped, 2);
    EXPECT_FALSE(controller.isRunning());
}

TEST(EngineControllerTests, StopOnAStoppedControllerSaysNothing)
{
    CanEngineController controller;

    int stopped = 0;
    QObject::connect(&controller, &CanEngineController::stopped, [&stopped] { ++stopped; });

    controller.stop();
    EXPECT_EQ(stopped, 0);
}
