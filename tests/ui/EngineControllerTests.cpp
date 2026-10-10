// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the window believes about the measurement, against what the engine is doing.
//
// The two can part. A node that throws ends the measurement from inside the engine - the guard
// around the dispatch thread reports it and lets the thread return - and nobody pressed Stop. The
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

/// A node that throws on its first pass, or does nothing at all.
class ThrowingNode final : public IPipelineNode {
public:
    explicit ThrowingNode(bool throws)
        : m_throws{throws}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.throws"; }
    [[nodiscard]] std::string displayName() const override { return "throws"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext&) override
    {
        if (m_throws) {
            throw std::runtime_error{"a node gave up"};
        }
    }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    bool m_throws;
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

    std::atomic<bool> throws{true};
    controller.engine().setGraphBuilder(
        [&throws](PipelineGraph& graph, std::span<const NodeId> sources) -> Result {
            const NodeId node = graph.addNode(std::make_unique<ThrowingNode>(throws.load()));
            return graph.connect(PortRef{sources[0], 0}, PortRef{node, 0});
        });

    int started = 0;
    int stopped = 0;
    QObject::connect(&controller, &CanEngineController::started, [&started] { ++started; });
    QObject::connect(&controller, &CanEngineController::stopped, [&stopped] { ++stopped; });

    controller.start();
    ASSERT_EQ(started, 1);

    // Nobody presses Stop. The node throws, the engine reports it and stops the loop, and the
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
