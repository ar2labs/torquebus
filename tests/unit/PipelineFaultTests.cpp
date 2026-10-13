// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the graph does when a node it runs fails.
//
// A node is code somebody wrote - the Lua ECU, a decoder, and since v0.17 types a
// plugin registered - and it runs on the one thread every other node is waiting
// behind. The question these tests answer is how far a failure travels. The answer
// they hold the graph to is: not past the node. A block that throws goes quiet, says
// so once, and the branches it was not feeding carry on; one that cannot start fails
// the compile with its name on it, after the nodes ahead of it have been told the run
// is not happening; one that cannot finish does not stop the others finishing.

#include "core/pipeline/PipelineGraph.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// Publishes one frame per pass, so that something reaches whatever is wired to it.
class Source final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    void process(NodeContext& context) override
    {
        m_frame.identifier = 0x100;
        m_frame.length = 1;
        m_frame.dlc = 1;
        context.publish<CanFrame>(0, std::span<const CanFrame>{&m_frame, 1});
    }

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    CanFrame m_frame;
};

/// How the node under test fails.
enum class Failure {
    None,
    Exception,
    NotAnException,
    PublishThenThrow,
};

/// Forwards what it receives, and fails on request - from process(), prepare() or
/// finish(), as the test chooses.
class Fragile final : public IPipelineNode {
public:
    explicit Fragile(std::string name)
        : m_name{std::move(name)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return m_name; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
    }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    [[nodiscard]] Result prepare(std::size_t) override
    {
        ++prepares;

        if (throwOnPrepare) {
            throw std::runtime_error{"no memory for the buffer"};
        }

        return prepareResult;
    }

    void process(NodeContext& context) override
    {
        ++passes;

        const std::span<const CanFrame> incoming = context.in<CanFrame>(0);

        if (failure == Failure::PublishThenThrow) {
            m_buffer.assign(incoming.begin(), incoming.end());
            context.publish<CanFrame>(0, m_buffer);
        }

        switch (failure) {
        case Failure::Exception:
        case Failure::PublishThenThrow:
            throw std::runtime_error{"boom"};
        case Failure::NotAnException:
            throw 42; // NOLINT(hicpp-exception-baseclass)
        case Failure::None:
            break;
        }

        m_buffer.assign(incoming.begin(), incoming.end());
        context.publish<CanFrame>(0, m_buffer);
    }

    void finish() override
    {
        ++finishes;

        if (throwOnFinish) {
            throw std::runtime_error{"cannot close the file"};
        }
    }

    Failure failure{Failure::None};
    bool throwOnPrepare{false};
    bool throwOnFinish{false};
    Result prepareResult{Result::ok()};

    int prepares{0};
    int passes{0};
    int finishes{0};

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::string m_name;
    std::vector<CanFrame> m_buffer;
};

/// Counts what reaches it.
class Counter final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.counter"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
    }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        received += context.in<CanFrame>(0).size();
        ++passes;
    }

    std::size_t received{0};
    int passes{0};

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

/// Wires `source -> fragile -> counterAfter` and `source -> counterBeside`.
struct Rig final {
    explicit Rig(Failure failure = Failure::None)
    {
        auto sourceNode = std::make_unique<Source>();
        auto fragileNode = std::make_unique<Fragile>("test.fragile");
        auto afterNode = std::make_unique<Counter>();
        auto besideNode = std::make_unique<Counter>();

        fragileNode->failure = failure;

        fragile = fragileNode.get();
        after = afterNode.get();
        beside = besideNode.get();

        source = graph.addNode(std::move(sourceNode));
        fragileId = graph.addNode(std::move(fragileNode));
        const NodeId afterId = graph.addNode(std::move(afterNode));
        const NodeId besideId = graph.addNode(std::move(besideNode));

        EXPECT_TRUE(graph.connect({source, 0}, {fragileId, 0}).succeeded());
        EXPECT_TRUE(graph.connect({fragileId, 0}, {afterId, 0}).succeeded());
        EXPECT_TRUE(graph.connect({source, 0}, {besideId, 0}).succeeded());

        graph.setFaultHandler([this](std::string_view node, std::string_view reason) {
            reports.push_back(std::string{node} + " | " + std::string{reason});
        });
    }

    PipelineGraph graph;
    NodeId source{NodeId::Invalid};
    NodeId fragileId{NodeId::Invalid};
    Fragile* fragile{nullptr};
    Counter* after{nullptr};
    Counter* beside{nullptr};
    std::vector<std::string> reports;
};

} // namespace

TEST(PipelineFaultTests, ANodeThatThrowsIsTakenOutOfTheRunAndTheOtherBranchesGoOn)
{
    Rig rig{Failure::Exception};
    ASSERT_TRUE(rig.graph.compile().succeeded());

    for (int pass = 0; pass < 5; ++pass) {
        rig.graph.execute();
    }

    // Asked once. A node that threw is not asked again: the same exception five times a
    // second would be a log line five times a second and a pass that is slower for it.
    EXPECT_EQ(rig.fragile->passes, 1);
    EXPECT_TRUE(rig.graph.isFaulted(rig.fragileId));
    EXPECT_EQ(rig.graph.faultedNodeCount(), 1U);

    // The branch it was feeding goes quiet, because there is nothing to feed it...
    EXPECT_EQ(rig.after->received, 0U);
    EXPECT_EQ(rig.after->passes, 5) << "the node behind the failure stopped being run too";

    // ...and the one it was not feeding does not notice.
    EXPECT_EQ(rig.beside->received, 5U);

    // Said once, with the node and the reason.
    ASSERT_EQ(rig.reports.size(), 1U);
    EXPECT_NE(rig.reports.front().find("test.fragile"), std::string::npos) << rig.reports.front();
    EXPECT_NE(rig.reports.front().find("boom"), std::string::npos) << rig.reports.front();
}

TEST(PipelineFaultTests, WhatANodePublishedBeforeItThrewIsNotDelivered)
{
    // Half a batch is not a batch. A node that published and then threw has left its
    // buffer in a state nobody knows, and a consumer reading it is reading that.
    Rig rig{Failure::PublishThenThrow};
    ASSERT_TRUE(rig.graph.compile().succeeded());

    rig.graph.execute();
    rig.graph.execute();

    EXPECT_EQ(rig.after->received, 0U);
    EXPECT_EQ(rig.beside->received, 2U);
}

TEST(PipelineFaultTests, SomethingThrownThatIsNotAnExceptionIsContainedToo)
{
    Rig rig{Failure::NotAnException};
    ASSERT_TRUE(rig.graph.compile().succeeded());

    EXPECT_NO_THROW(rig.graph.execute());
    EXPECT_TRUE(rig.graph.isFaulted(rig.fragileId));
    ASSERT_EQ(rig.reports.size(), 1U);
    EXPECT_NE(rig.reports.front().find("not a std::exception"), std::string::npos);
}

TEST(PipelineFaultTests, AFaultHandlerThatThrowsDoesNotTakeTheRunDownWithIt)
{
    // The handler is somebody else's code, called from inside a catch block. A second
    // exception there is std::terminate.
    Rig rig{Failure::Exception};
    rig.graph.setFaultHandler(
        [](std::string_view, std::string_view) { throw std::runtime_error{"the log is gone"}; });
    ASSERT_TRUE(rig.graph.compile().succeeded());

    EXPECT_NO_THROW(rig.graph.execute());
    EXPECT_NO_THROW(rig.graph.execute());
    EXPECT_EQ(rig.beside->received, 2U);
}

TEST(PipelineFaultTests, ARunWithoutAHandlerStillContainsTheFailure)
{
    Rig rig{Failure::Exception};
    rig.graph.setFaultHandler({});
    ASSERT_TRUE(rig.graph.compile().succeeded());

    EXPECT_NO_THROW(rig.graph.execute());
    EXPECT_EQ(rig.graph.faultedNodeCount(), 1U);
}

TEST(PipelineFaultTests, CompilingAgainGivesAFaultedNodeAnotherChance)
{
    // Start again after fixing the script: the measurement is a new one, and what
    // failed in the last is not held against it.
    Rig rig{Failure::Exception};
    ASSERT_TRUE(rig.graph.compile().succeeded());
    rig.graph.execute();
    ASSERT_EQ(rig.graph.faultedNodeCount(), 1U);

    rig.fragile->failure = Failure::None;
    ASSERT_TRUE(rig.graph.compile().succeeded());

    EXPECT_EQ(rig.graph.faultedNodeCount(), 0U);
    EXPECT_FALSE(rig.graph.isFaulted(rig.fragileId));

    rig.graph.execute();
    EXPECT_EQ(rig.after->received, 1U);
}

TEST(PipelineFaultTests, APrepareThatThrowsFailsTheCompileAndNamesTheNode)
{
    PipelineGraph graph;

    auto first = std::make_unique<Fragile>("test.first");
    auto second = std::make_unique<Fragile>("test.second");
    auto third = std::make_unique<Fragile>("test.third");

    second->throwOnPrepare = true;

    Fragile* firstPointer = first.get();
    Fragile* thirdPointer = third.get();

    const NodeId a = graph.addNode(std::make_unique<Source>());
    const NodeId b = graph.addNode(std::move(first));
    const NodeId c = graph.addNode(std::move(second));
    const NodeId d = graph.addNode(std::move(third));

    ASSERT_TRUE(graph.connect({a, 0}, {b, 0}).succeeded());
    ASSERT_TRUE(graph.connect({b, 0}, {c, 0}).succeeded());
    ASSERT_TRUE(graph.connect({c, 0}, {d, 0}).succeeded());

    Result result = Result::ok();
    EXPECT_NO_THROW(result = graph.compile());

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(graph.isCompiled());
    EXPECT_NE(std::string{result.message()}.find("test.second"), std::string::npos)
        << result.message();
    EXPECT_NE(std::string{result.message()}.find("no memory for the buffer"), std::string::npos)
        << result.message();

    // The node ahead of the failure had been prepared - a script has had its on_enable, a
    // logger has opened its file - and is told the run is not happening. The one behind
    // it was never started and is left alone.
    EXPECT_EQ(firstPointer->prepares, 1);
    EXPECT_EQ(firstPointer->finishes, 1);
    EXPECT_EQ(thirdPointer->prepares, 0);
    EXPECT_EQ(thirdPointer->finishes, 0);
}

TEST(PipelineFaultTests, APrepareThatFailsWithAResultAlsoWindsUpTheNodesAheadOfIt)
{
    PipelineGraph graph;

    auto first = std::make_unique<Fragile>("test.first");
    auto second = std::make_unique<Fragile>("test.second");
    second->prepareResult = Result::error(ErrorCode::InvalidArgument, "no database");

    Fragile* firstPointer = first.get();

    const NodeId a = graph.addNode(std::make_unique<Source>());
    const NodeId b = graph.addNode(std::move(first));
    const NodeId c = graph.addNode(std::move(second));

    ASSERT_TRUE(graph.connect({a, 0}, {b, 0}).succeeded());
    ASSERT_TRUE(graph.connect({b, 0}, {c, 0}).succeeded());

    const Result result = graph.compile();

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(firstPointer->finishes, 1);
}

TEST(PipelineFaultTests, ANodeThatCannotFinishDoesNotStopTheOthersFinishing)
{
    // A logger further up the graph still has a file to close.
    Rig rig;
    rig.fragile->throwOnFinish = true;
    ASSERT_TRUE(rig.graph.compile().succeeded());
    rig.graph.execute();

    EXPECT_NO_THROW(rig.graph.finish());

    EXPECT_EQ(rig.fragile->finishes, 1);
    ASSERT_EQ(rig.reports.size(), 1U);
    EXPECT_NE(rig.reports.front().find("cannot close the file"), std::string::npos);
}

TEST(PipelineFaultTests, ANodeThatThrewIsStillToldTheRunIsOver)
{
    // It may hold a file open, or a script that wants its on_disable.
    Rig rig{Failure::Exception};
    ASSERT_TRUE(rig.graph.compile().succeeded());
    rig.graph.execute();

    rig.graph.finish();

    EXPECT_EQ(rig.fragile->finishes, 1);
}
