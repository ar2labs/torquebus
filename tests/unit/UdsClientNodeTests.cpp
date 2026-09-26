// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The block, end to end: a question posted by a console reaching an ECU as
// frames on a bus, and the answer coming back up through both layers.
//
// The ECU here is thirty lines of test code, and it is a real one in the way
// that matters - it speaks ISO-TP through the same connection class, so a
// request that is mis-segmented gets no answer, exactly as on a bench. Two
// halves of one implementation agreeing with each other would prove nothing;
// this at least proves the two halves are the two halves.

#include "core/diagnostics/DiagnosticSession.h"
#include "core/diagnostics/UdsClientNode.h"
#include "core/diagnostics/UdsTypes.h"
#include "core/isotp/IsoTpConnection.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/PipelineGraph.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// A source the test drives: whatever is pushed into it is published on the
/// next pass. Stands in for the channel a real graph has.
class QueueSourceNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.queue"; }
    [[nodiscard]] std::string displayName() const override { return "Queue"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    Result prepare(std::size_t) override
    {
        m_published.reserve(64);
        return Result::ok();
    }

    void process(NodeContext& context) override
    {
        m_published = m_queued;
        m_queued.clear();

        if (!m_published.empty()) {
            context.publish<CanFrame>(0, std::span<const CanFrame>{m_published});
        }
    }

    void push(const CanFrame& frame) { m_queued.push_back(frame); }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<CanFrame> m_queued;
    std::vector<CanFrame> m_published;
};

/// Collects whatever reaches it, standing in for the bus the frames go onto.
class CollectSinkNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.collect"; }
    [[nodiscard]] std::string displayName() const override { return "Collect"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            frames.push_back(frame);
        }
    }

    std::vector<CanFrame> frames;

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
};

/// An ECU on the other end of the address pair: receives on 0x7E0, answers on
/// 0x7E8.
class SimulatedEcu final {
public:
    SimulatedEcu()
    {
        IsoTpAddress address;
        address.transmitId = 0x7E8;
        address.receiveId = 0x7E0;

        m_connection = std::make_unique<IsoTpConnection>(address, IsoTpConfig{});
    }

    void onFrame(const CanFrame& frame, std::uint64_t nowNs)
    {
        static_cast<void>(m_connection->onFrame(frame, nowNs));

        for (const IsoTpEvent& event : m_connection->events()) {
            if (event.kind == IsoTpEvent::Kind::MessageReceived) {
                answer(event.data, nowNs);
            }
        }

        m_connection->clearEvents();
        m_connection->poll(nowNs);
    }

    [[nodiscard]] std::vector<CanFrame> takeFrames()
    {
        std::vector<CanFrame> frames{m_connection->pendingFrames().begin(),
                                     m_connection->pendingFrames().end()};
        m_connection->clearPendingFrames();
        return frames;
    }

    void setNextAnswer(Bytes answer) { m_nextAnswer = std::move(answer); }

    [[nodiscard]] const std::vector<Bytes>& heard() const noexcept { return m_heard; }

private:
    void answer(const Bytes& request, std::uint64_t nowNs)
    {
        m_heard.push_back(request);

        if (!m_nextAnswer.empty()) {
            static_cast<void>(m_connection->send(m_nextAnswer, nowNs));
            m_nextAnswer.clear();
            return;
        }

        if (request.empty()) {
            return;
        }

        // TesterPresent with the suppress bit is not answered, which is the
        // whole reason it is sent that way.
        if (request[0] == 0x3E && request.size() >= 2 && (request[1] & 0x80U) != 0) {
            return;
        }

        Bytes response{static_cast<std::uint8_t>(request[0] + 0x40U)};
        response.insert(response.end(), request.begin() + 1, request.end());

        if (request[0] == 0x22) {
            const Bytes vin{'W', 'V', 'W', 'Z', 'Z', 'Z', '1', 'K'};
            response.insert(response.end(), vin.begin(), vin.end());
        }

        static_cast<void>(m_connection->send(response, nowNs));
    }

    std::unique_ptr<IsoTpConnection> m_connection;
    Bytes m_nextAnswer;
    std::vector<Bytes> m_heard;
};

/// One graph, one ECU, and a bus between them.
struct Bench final {
    DiagnosticSession session;

    PipelineGraph graph;
    QueueSourceNode* source{nullptr};
    CollectSinkNode* sink{nullptr};

    SimulatedEcu ecu;

    Bench()
    {
        auto owned = std::make_unique<QueueSourceNode>();
        source = owned.get();
        const NodeId sourceId = graph.addNode(std::move(owned));

        IsoTpAddress address;
        address.transmitId = 0x7E0;
        address.receiveId = 0x7E8;

        const NodeId udsId = graph.addNode(
            std::make_unique<UdsClientNode>(address, IsoTpConfig{}, UdsTiming{}, &session));

        auto collector = std::make_unique<CollectSinkNode>();
        sink = collector.get();
        const NodeId sinkId = graph.addNode(std::move(collector));

        EXPECT_TRUE(graph.connect(PortRef{sourceId, 0}, PortRef{udsId, 0}).succeeded());
        EXPECT_TRUE(graph.connect(PortRef{udsId, 0}, PortRef{sinkId, 0}).succeeded());
        EXPECT_TRUE(graph.compile().succeeded());
    }

    /// Runs the graph, carrying frames to the ECU and its answers back - which
    /// is what a bus does.
    void run(int passes)
    {
        for (int pass = 0; pass < passes; ++pass) {
            graph.execute();

            for (const CanFrame& frame : sink->frames) {
                ecu.onFrame(frame, 0);
            }
            sink->frames.clear();

            for (const CanFrame& frame : ecu.takeFrames()) {
                source->push(frame);
            }
        }
    }
};

} // namespace

TEST(UdsClientNodeTests, AQuestionReachesTheECUAndTheAnswerComesBack)
{
    Bench bench;

    EXPECT_TRUE(bench.session.isActive());

    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.run(8);

    ASSERT_TRUE(bench.ecu.heard().size() >= 1);
    EXPECT_TRUE((bench.ecu.heard().front() == Bytes{0x22, 0xF1, 0x90}));

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);
    EXPECT_TRUE((exchanges.front().response
                 == Bytes{0x62, 0xF1, 0x90, 'W', 'V', 'W', 'Z', 'Z', 'Z', '1', 'K'}));
}

TEST(UdsClientNodeTests, ALongAnswerCrossesInSeveralFramesAndArrivesWhole)
{
    // The point of having ISO-TP underneath: an answer of forty bytes is a
    // first frame, a flow control and four consecutive frames, and none of that
    // is visible from up here.
    Bench bench;

    Bytes answer{0x62, 0xF1, 0x90};
    for (std::uint8_t index = 0; index < 40; ++index) {
        answer.push_back(static_cast<std::uint8_t>('A' + (index % 26)));
    }

    bench.ecu.setNextAnswer(answer);
    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.run(16);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);
    EXPECT_TRUE(exchanges.front().response == answer);
}

TEST(UdsClientNodeTests, ARefusalReachesTheConsoleWithItsReasonInWords)
{
    Bench bench;

    bench.ecu.setNextAnswer({0x7F, 0x22, 0x33});
    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.run(8);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Negative);

    const std::string text = exchanges.front().describe();
    SCOPED_TRACE(::testing::Message() << text);
    EXPECT_TRUE(text.find("security access") != std::string::npos);
}

TEST(UdsClientNodeTests, TwoRequestsPostedAtOnceAreAskedOneAfterTheOther)
{
    // UDS is one question at a time, but a console user pressing Send twice
    // means both - a millisecond apart is not two questions at once.
    Bench bench;

    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.session.postRequest(readDataByIdentifier(0xF18C));

    bench.run(16);

    ASSERT_TRUE(bench.ecu.heard().size() == 2);
    EXPECT_TRUE((bench.ecu.heard()[0] == Bytes{0x22, 0xF1, 0x90}));
    EXPECT_TRUE((bench.ecu.heard()[1] == Bytes{0x22, 0xF1, 0x8C}));

    EXPECT_TRUE(bench.session.takeExchanges().size() == 2);
}

TEST(UdsClientNodeTests, AnECUThatSaysNothingIsReportedAsNoAnswer)
{
    // Not a crash, not a wait: the console shows "no answer in 50 ms", which is
    // a different diagnosis from a refusal and has to look different.
    Bench bench;

    bench.session.postRequest(readDataByIdentifier(0xF190));

    // The graph runs but nothing is carried to the ECU, so nothing comes back.
    for (int pass = 0; pass < 4; ++pass) {
        bench.graph.execute();
        bench.sink->frames.clear();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    bench.graph.execute();

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Timeout);
}

TEST(UdsClientNodeTests, TheConsoleAndTheExecutorDoNotWaitForEachOther)
{
    // takeRequests uses try_lock and reports failure rather than blocking: the
    // thing on the other side of that mutex is a window that may be repainting.
    // This drives both sides at once and checks that everything posted arrives
    // and nothing deadlocks.
    DiagnosticSession session;

    constexpr int kRequests = 2000;

    std::thread console{[&session] {
        for (int index = 0; index < kRequests; ++index) {
            session.postRequest({0x22, static_cast<std::uint8_t>(index & 0xFF), 0x90});
        }
    }};

    std::vector<Bytes> taken;

    while (static_cast<int>(taken.size()) < kRequests) {
        static_cast<void>(session.takeRequests(taken));
    }

    console.join();

    EXPECT_TRUE(static_cast<int>(taken.size()) == kRequests);
}

TEST(UdsClientNodeTests, P2LongerThanP2IsRefusedWhileTheBlockIsOnScreen)
{
    // The one timing value somebody is tempted to "fix" by typing a bigger
    // number, and doing so makes the extended deadline meaningless.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "uds",
                        .typeName = "uds.client",
                        .parameters = {{"p2Ms", ParameterValue::fromInteger(9000)}}});

    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("P2*") != std::string::npos);
}

TEST(UdsClientNodeTests, TheUDSBlockIsOnTheCanvasWithTypedPorts)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* info = catalog.find("uds.client");
    ASSERT_TRUE(info != nullptr);

    EXPECT_TRUE(info->category == "Diagnostics");

    ASSERT_TRUE(info->inputs.size() == 2);
    EXPECT_TRUE(info->inputs[0].type == PortType::Frames);
    EXPECT_TRUE(info->inputs[1].type == PortType::Events);

    ASSERT_TRUE(info->outputs.size() == 2);
    EXPECT_TRUE(info->outputs[0].type == PortType::Frames);
    EXPECT_TRUE(info->outputs[1].type == PortType::Events);
}
