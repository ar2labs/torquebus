// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A Lua ECU answering a real UDS client across a real ISO-TP transport, with
// nothing faked between them. Both ends are the production classes - the tester
// is UdsClientNode and the ECU is LuaEcuNode running a script - so a request
// mis-segmented at either end simply gets no answer, which is what would happen
// on a bench and is the only reason this is worth more than the two suites
// underneath it.
//
// Two graphs rather than one: the tester's frames have to reach the ECU and the
// ECU's have to come back, which in a single graph is a cycle, and the executor
// refuses cycles for good reason. The test is the bus between them.

#include "core/diagnostics/UdsClientNode.h"
#include "core/diagnostics/UdsTypes.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/scripting/LuaEcuNode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// Publishes whatever the test pushes into it.
class SourceNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.source"; }
    [[nodiscard]] std::string displayName() const override { return "Source"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
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
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<CanFrame> m_queued;
    std::vector<CanFrame> m_published;
};

/// Keeps whatever reaches it.
class SinkNode final : public IPipelineNode {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.sink"; }
    [[nodiscard]] std::string displayName() const override { return "Sink"; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kPorts;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override { return {}; }

    void process(NodeContext& context) override
    {
        for (const CanFrame& frame : context.in<CanFrame>(0)) {
            frames.push_back(frame);
        }
    }

    [[nodiscard]] std::vector<CanFrame> take()
    {
        std::vector<CanFrame> taken;
        taken.swap(frames);
        return taken;
    }

    std::vector<CanFrame> frames;

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };
};

/// A tester and a scripted ECU on opposite ends of a bus the test carries.
struct Bench final {
    DiagnosticSession session;

    PipelineGraph tester;
    PipelineGraph ecu;

    SourceNode* toTester{nullptr};
    SinkNode* fromTester{nullptr};

    SourceNode* toEcu{nullptr};
    SinkNode* fromEcu{nullptr};

    explicit Bench(const std::string& source)
    {
        // --- the tester ---------------------------------------------------
        auto testerIn = std::make_unique<SourceNode>();
        toTester = testerIn.get();
        const NodeId testerInId = tester.addNode(std::move(testerIn));

        IsoTpAddress testerAddress;
        testerAddress.transmitId = 0x7E0;
        testerAddress.receiveId = 0x7E8;

        const NodeId clientId = tester.addNode(
            std::make_unique<UdsClientNode>(testerAddress, IsoTpConfig{}, UdsTiming{}, &session));

        auto testerOut = std::make_unique<SinkNode>();
        fromTester = testerOut.get();
        const NodeId testerOutId = tester.addNode(std::move(testerOut));

        EXPECT_TRUE(tester.connect(PortRef{testerInId, 0}, PortRef{clientId, 0}).succeeded());
        EXPECT_TRUE(tester.connect(PortRef{clientId, 0}, PortRef{testerOutId, 0}).succeeded());
        EXPECT_TRUE(tester.compile().succeeded());

        // --- the ECU ------------------------------------------------------
        auto ecuIn = std::make_unique<SourceNode>();
        toEcu = ecuIn.get();
        const NodeId ecuInId = ecu.addNode(std::move(ecuIn));

        auto scripted = std::make_unique<LuaEcuNode>(source, "test.lua");

        IsoTpAddress ecuAddress;
        ecuAddress.receiveId = 0x7E0; // What the tester transmits.
        ecuAddress.transmitId = 0x7E8;

        scripted->enableDiagnostics(ecuAddress, IsoTpConfig{});

        const NodeId scriptedId = ecu.addNode(std::move(scripted));

        auto ecuOut = std::make_unique<SinkNode>();
        fromEcu = ecuOut.get();
        const NodeId ecuOutId = ecu.addNode(std::move(ecuOut));

        EXPECT_TRUE(ecu.connect(PortRef{ecuInId, 0}, PortRef{scriptedId, 0}).succeeded());
        EXPECT_TRUE(ecu.connect(PortRef{scriptedId, 0}, PortRef{ecuOutId, 0}).succeeded());
        EXPECT_TRUE(ecu.compile().succeeded());
    }

    /// One pass of each, carrying what came out of one into the other.
    void run(int passes)
    {
        for (int pass = 0; pass < passes; ++pass) {
            tester.execute();

            for (const CanFrame& frame : fromTester->take()) {
                toEcu->push(frame);
            }

            ecu.execute();

            for (const CanFrame& frame : fromEcu->take()) {
                toTester->push(frame);
            }
        }
    }
};

} // namespace

TEST(LuaUdsEcuTests, ALuaECUAnswersARealTester)
{
    Bench bench{R"(
        function on_enable()
            uds_did(0xF190, "WVWZZZ1KZAW000001")
        end
    )"};

    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.run(12);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);

    // Seventeen characters of VIN: a first frame, a flow control and two
    // consecutive frames, and none of that is visible from the script.
    const Bytes& response = exchanges.front().response;
    ASSERT_TRUE(response.size() == 3 + 17);
    EXPECT_TRUE(response[0] == 0x62);
    EXPECT_TRUE(std::string(response.begin() + 3, response.end()) == "WVWZZZ1KZAW000001");
}

TEST(LuaUdsEcuTests, AnIdentifierTheScriptDidNotDeclareIsRefused)
{
    // The server answers, not the script - which is the point of having one:
    // the ordinary refusals are right without anybody writing them.
    Bench bench{R"(
        function on_enable()
            uds_did(0xF190, "ABC")
        end
    )"};

    bench.session.postRequest(readDataByIdentifier(0xF1A0));
    bench.run(12);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Negative);
    EXPECT_TRUE(exchanges.front().negativeResponse
                == static_cast<std::uint8_t>(UdsNegativeResponse::RequestOutOfRange));
}

TEST(LuaUdsEcuTests, AScriptCanAnswerAServiceTheServerDoesNotImplement)
{
    // RoutineControl, which UdsServer knows nothing about. Three lines of Lua
    // rather than a change to the C++, which is the whole reason the hook
    // exists.
    Bench bench{R"(
        function on_uds_request(request)
            if request:byte(1) == 0x31 then
                return string.char(0x71, 0x01, 0x02, 0x03)
            end
        end
    )"};

    bench.session.postRequest(Bytes{0x31, 0x01, 0x02, 0x03});
    bench.run(12);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);
    EXPECT_TRUE((exchanges.front().response == Bytes{0x71, 0x01, 0x02, 0x03}));
}

TEST(LuaUdsEcuTests, AScriptCanMakeTheECUGoSilent)
{
    // Returning false means "say nothing" - a dead ECU, which is the case a
    // tester has to survive and the only one nothing else can simulate. The
    // tester times out, which is the correct outcome and not an error here.
    Bench bench{R"(
        function on_uds_request(request)
            return false
        end
    )"};

    bench.session.postRequest(readDataByIdentifier(0xF190));

    // Long enough for P2 to expire on the tester's own clock.
    for (int pass = 0; pass < 4; ++pass) {
        bench.run(1);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    bench.run(1);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Timeout);
}

TEST(LuaUdsEcuTests, AScriptThatReturnsNothingLetsTheServerAnswer)
{
    // The three-valued verdict: bytes decide, false silences, nothing declines.
    // A handler that only cares about one service must not swallow the rest.
    Bench bench{R"(
        function on_enable()
            uds_did(0xF190, "ABCD")
        end

        function on_uds_request(request)
            if request:byte(1) == 0x31 then
                return string.char(0x71)
            end
        end
    )"};

    bench.session.postRequest(readDataByIdentifier(0xF190));
    bench.run(12);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();

    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);
    EXPECT_TRUE((exchanges.front().response == Bytes{0x62, 0xF1, 0x90, 'A', 'B', 'C', 'D'}));
}

TEST(LuaUdsEcuTests, SessionAndSecurityComeFromTheScriptSOwnAlgorithm)
{
    Bench bench{R"(
        function on_enable()
            uds_did(0x2001, string.char(0x00, 0x64),
                    { writable = true, session = 3, security = true })
        end

        function on_security_seed(seed)
            local key = ""
            for index = 1, #seed do
                key = key .. string.char(255 - seed:byte(index))
            end
            return key
        end
    )"};

    // Locked, in the default session.
    bench.session.postRequest(readDataByIdentifier(0x2001));
    bench.run(12);

    std::vector<UdsExchange> exchanges = bench.session.takeExchanges();
    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(
        exchanges.front().negativeResponse
        == static_cast<std::uint8_t>(UdsNegativeResponse::ServiceNotSupportedInActiveSession));

    // Extended session, then a seed.
    bench.session.postRequest(diagnosticSessionControl(UdsSession::Extended));
    bench.run(12);
    static_cast<void>(bench.session.takeExchanges());

    bench.session.postRequest(securityAccessSeed(0x01));
    bench.run(12);

    exchanges = bench.session.takeExchanges();
    ASSERT_TRUE(exchanges.size() == 1);
    ASSERT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);

    const Bytes& seed = exchanges.front().response;
    ASSERT_TRUE(seed.size() > 2);

    Bytes key;
    for (std::size_t index = 2; index < seed.size(); ++index) {
        key.push_back(static_cast<std::uint8_t>(255 - seed[index]));
    }

    bench.session.postRequest(securityAccessKey(0x01, key));
    bench.run(12);

    exchanges = bench.session.takeExchanges();
    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE(exchanges.front().outcome == UdsExchange::Outcome::Positive);

    // And now the calibration is readable.
    bench.session.postRequest(readDataByIdentifier(0x2001));
    bench.run(12);

    exchanges = bench.session.takeExchanges();
    ASSERT_TRUE(exchanges.size() == 1);
    EXPECT_TRUE((exchanges.front().response == Bytes{0x62, 0x20, 0x01, 0x00, 0x64}));
}

TEST(LuaUdsEcuTests, AScriptSeesTheFaultsItStoredComeBackAsADTCList)
{
    Bench bench{R"(
        function on_enable()
            uds_dtc(0x012800, 0x2F)
            uds_dtc(0xC03500)
        end
    )"};

    bench.session.postRequest(readDtcByStatusMask(0xFF));
    bench.run(12);

    const std::vector<UdsExchange> exchanges = bench.session.takeExchanges();
    ASSERT_TRUE(exchanges.size() == 1);

    const std::vector<DiagnosticTroubleCode> codes = parseDtcResponse(exchanges.front().response);

    ASSERT_TRUE(codes.size() == 2);
    EXPECT_TRUE(codes[0].name() == "P0128");
    EXPECT_TRUE(codes[1].name() == "U0035");

    // The default status is "confirmed", which is what a script that just wants
    // a fault to exist means.
    EXPECT_TRUE(codes[1].status == 0x08);
}

TEST(LuaUdsEcuTests, AScriptWithNoDiagnosticAddressesHasNoUdsFunctions)
{
    // An ordinary ECU pays nothing for a layer it does not use - and a script
    // that calls uds_did on one does not quietly do nothing. It fails at
    // Start, naming the function, which is the same rule the rest of the
    // application follows: a block that cannot do what its script says is
    // refused while somebody is looking at it rather than running as a
    // half-ECU nobody can see is wrong.
    LuaEcuNode node{R"(
        function on_enable()
            uds_did(0xF190, "ABC")
        end
    )",
                    "plain.lua"};

    EXPECT_FALSE(node.answersDiagnostics());

    const Result result = node.prepare(64);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("uds_did") != std::string::npos);
}
