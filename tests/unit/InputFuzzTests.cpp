// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Everything that reads bytes somebody else wrote, fed bytes that are nearly right.
//
// A fuzzer's value is the surprise, and a surprise is not a thing a fixed test can
// hold. What can be held is the contract the readers all make: whatever arrives, the
// answer is a Result or an event, never an exception out of the function, never a
// read past the end of a buffer, never a loop that does not end. Each test below takes
// an input that is valid, damages it in the ways a real file or a real bus gets
// damaged (a flipped byte, a cut in the middle, a number grown by a digit, a frame that
// says it is longer than it is), and offers the result to the reader. The assertion is
// that the call returns - everything else the readers promise is tested where the
// reader is.
//
// The generator is seeded, so a failure is reproducible from the iteration number in
// its message, and the count is small enough to run in a couple of seconds. Set
// TORQUEBUS_FUZZ_SCALE=50 to run it fifty times longer; under AddressSanitizer, where a read
// past the end of a buffer is a report rather than a silent success, that is the
// useful way to run it; the count is a multiple of the default, so one setting scales
// every test alike.

#include "core/FilePath.h"
#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/diagnostics/UdsClient.h"
#include "core/diagnostics/UdsClientNode.h"
#include "core/diagnostics/UdsServer.h"
#include "core/isotp/IsoTpConnection.h"
#include "core/isotp/IsoTpNode.h"
#include "core/j1939/J1939Node.h"
#include "core/j1939/J1939Transport.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/pipeline/nodes/DbcDecoderNode.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"
#include "core/tinyml/TinyMlEcuNode.h"
#include "core/tinyml/TinyMlModel.h"
#include "core/trace/TraceStore.h"
#include "services/ProjectFile.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// xorshift64*: small, fast, and the same on every platform, which std::mt19937's
/// distributions are not.
class Random final {
public:
    explicit Random(std::uint64_t seed)
        : m_state{seed != 0 ? seed : 0x9E3779B97F4A7C15ULL}
    { }

    [[nodiscard]] std::uint64_t next() noexcept
    {
        m_state ^= m_state >> 12U;
        m_state ^= m_state << 25U;
        m_state ^= m_state >> 27U;
        return m_state * 0x2545F4914F6CDD1DULL;
    }

    /// 0 .. bound-1; 0 when bound is 0.
    [[nodiscard]] std::size_t below(std::size_t bound) noexcept
    {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

    [[nodiscard]] std::uint8_t byte() noexcept { return static_cast<std::uint8_t>(next() >> 56U); }

    [[nodiscard]] bool chance(std::size_t oneIn) noexcept { return below(oneIn) == 0; }

private:
    std::uint64_t m_state;
};

[[nodiscard]] std::size_t iterations(std::size_t fallback)
{
    bool ok = false;
    const int scale = qEnvironmentVariableIntValue("TORQUEBUS_FUZZ_SCALE", &ok);

    return ok && scale > 0 ? fallback * static_cast<std::size_t>(scale) : fallback;
}

/// Pieces that break a text format in the ways a hand edit does.
constexpr std::array<const char*, 18> kFragments{
    "0",
    "-1",
    "65536",
    "4294967296",
    "99999999999999999999",
    "1e999",
    "-",
    ".",
    "\"",
    "\\",
    "\n",
    ":",
    ";",
    "|",
    "@",
    "(",
    "/*",
    "\xFF\xFE",
};

[[nodiscard]] std::string mutateText(std::string text, Random& random)
{
    const std::size_t edits = 1 + random.below(4);

    for (std::size_t edit = 0; edit < edits; ++edit) {
        if (text.empty()) {
            text.push_back(static_cast<char>(random.byte()));
            continue;
        }

        const std::size_t at = random.below(text.size());

        switch (random.below(6)) {
        case 0:
            text[at] = static_cast<char>(random.byte());
            break;
        case 1:
            text.erase(at, 1 + random.below(8));
            break;
        case 2:
            text.insert(at, kFragments[random.below(kFragments.size())]);
            break;
        case 3:
            text.resize(at); // a file cut short
            break;
        case 4: {
            // A line repeated, as a merge does.
            const std::size_t start = text.rfind('\n', at);
            const std::size_t end = text.find('\n', at);
            if (start != std::string::npos && end != std::string::npos && end > start) {
                text.insert(end, text.substr(start, end - start));
            }
            break;
        }
        default: {
            // A digit grown, as a typing slip does.
            const std::size_t digit = text.find_first_of("0123456789", at);
            if (digit != std::string::npos) {
                text.insert(digit, 1, static_cast<char>('0' + random.below(10)));
            }
            break;
        }
        }
    }

    return text;
}

[[nodiscard]] std::vector<std::uint8_t> mutateBytes(std::vector<std::uint8_t> bytes, Random& random)
{
    const std::size_t edits = 1 + random.below(6);

    for (std::size_t edit = 0; edit < edits; ++edit) {
        if (bytes.empty()) {
            bytes.push_back(random.byte());
            continue;
        }

        const std::size_t at = random.below(bytes.size());

        switch (random.below(4)) {
        case 0:
            bytes[at] = random.byte();
            break;
        case 1:
            bytes[at] ^= static_cast<std::uint8_t>(1U << random.below(8));
            break;
        case 2:
            bytes.resize(at);
            break;
        default:
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), random.byte());
            break;
        }
    }

    return bytes;
}

constexpr const char* kDatabase = R"(VERSION "1.0"

NS_ :
	NS_DESC_
	CM_
	BA_DEF_
	BA_
	VAL_

BS_:

BU_: PCM1 FOO

BO_ 496 ExampleMessage: 8 PCM1
 SG_ Temperature : 0|12@0- (0.01,250) [229.52|270.47] "degK"  PCM1,FOO
 SG_ AverageRadius : 6|6@0+ (0.1,0) [0|5] "m" Vector__XXX
 SG_ Enable : 7|1@0+ (1,0) [0|0] "-" Vector__XXX
BO_ 2566844926 Extended: 8 FOO
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ WhenZero m0 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ WhenOne m1 : 8|16@1+ (0.5,0) [0|32767] "" Vector__XXX
BO_ 300 BigFrame: 64 FOO
 SG_ Late : 504|8@1+ (1,0) [0|255] "" Vector__XXX
BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX
 SG_ Orphan : 0|8@1+ (1,0) [0|255] "" Vector__XXX

CM_ BO_ 496 "Example message used as template in MotoHawk models.";
CM_ SG_ 496 Enable "A switch.";
BA_DEF_ BO_  "GenMsgCycleTime" INT 0 65535;
BA_DEF_DEF_  "GenMsgCycleTime" 0;
BA_ "GenMsgCycleTime" BO_ 496 100;
VAL_ 496 Enable 0 "Disabled" 1 "Enabled" ;
)";

constexpr const char* kModel = R"(TBUSML 1
model_name fuzz
feature_mean 0 0 0 0 0 0
feature_scale 1 1 1 1 1 1
layer head 6 7 linear 0.1
weights 1 0 0 0 0 0
weights 0 1 0 0 0 0
weights 0 0 1 0 0 0
weights 0 0 0 1 0 0
weights 0 0 0 0 1 0
weights 0 0 0 0 0 1
weights 1 1 1 1 1 1
biases 0 0 0 0 0 0 0
)";

} // namespace

TEST(InputFuzzTests, DamagedDatabasesAreResultsAndTheSignalsTheyDescribeAreSafeToRead)
{
    Random random{1};

    for (std::size_t round = 0; round < iterations(1500); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        CanDatabase database;
        const Result result = DbcParser::parse(mutateText(kDatabase, random), database);

        if (result.failed()) {
            EXPECT_FALSE(std::string{result.message()}.empty());
            continue;
        }

        // Whatever was accepted has to be readable, at any payload length a frame can
        // have, from a buffer that is exactly that long.
        for (const CanMessage& message : database.messages()) {
            EXPECT_LE(message.length, kMaxCanPayload);

            const CanFrame frame = message.makeFrame();
            EXPECT_LE(frame.length, kMaxCanPayload);

            for (const CanSignal& signal : message.signalList) {
                for (const std::size_t length :
                     {std::size_t{0}, std::size_t{1}, std::size_t{8}, kMaxCanPayload}) {
                    std::vector<std::uint8_t> payload(length);
                    for (std::uint8_t& byte : payload) {
                        byte = random.byte();
                    }

                    const std::uint8_t* data = payload.empty() ? nullptr : payload.data();

                    (void)signal.fitsIn(length);
                    (void)signal.rawValue(data, length);
                    (void)signal.decode(data, length);
                    (void)signal.encode(1.0, payload.data(), length);
                }
            }

            (void)message.signalsIn(frame.data.data(), frame.length);
        }
    }
}

TEST(InputFuzzTests, DamagedModelsAreResultsAndAcceptedOnesEvaluate)
{
    Random random{2};

    for (std::size_t round = 0; round < iterations(1500); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        TinyMlModel model;
        const Result result = TinyMlModel::parse(mutateText(kModel, random), model);

        if (result.failed()) {
            continue;
        }

        TinyMlArena arena{std::max<std::size_t>(model.requiredArenaBytes(), 4096)};
        TinyMlFeatureVector features{};
        TinyMlInferenceResult out{};
        (void)model.evaluate(features, arena, 65.0F, out);
    }
}

TEST(InputFuzzTests, DamagedLogFilesAreReadToTheirEndOrToTheirDamage)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path()
        / pathFromUtf8(std::format("torquebus-fuzz-log-{:08x}", std::random_device{}()));
    std::filesystem::create_directories(folder);

    const std::string good = utf8FromPath(folder / "good.tblog");

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(good, 99).succeeded());

        std::vector<CanFrame> frames(40);
        for (std::size_t index = 0; index < frames.size(); ++index) {
            frames[index].timestampNs = index * 1000;
            frames[index].identifier = static_cast<std::uint32_t>(0x100 + index);
            frames[index].length = static_cast<std::uint8_t>(index % 9);
            frames[index].dlc = frames[index].length;
        }

        ASSERT_TRUE(writer.append(frames).succeeded());
        writer.close();
    }

    std::vector<std::uint8_t> original;
    {
        std::ifstream in{pathFromUtf8(good), std::ios::binary};
        original.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    ASSERT_GT(original.size(), 64U);

    Random random{3};
    const std::string damaged = utf8FromPath(folder / "damaged.tblog");

    for (std::size_t round = 0; round < iterations(100); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        const std::vector<std::uint8_t> bytes = mutateBytes(original, random);
        {
            std::ofstream out{pathFromUtf8(damaged), std::ios::binary | std::ios::trunc};
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }

        TraceLogReader reader;
        if (reader.open(damaged).failed()) {
            continue;
        }

        std::vector<CanFrame> frames(16);
        std::size_t total = 0;

        // A reader that does not end on damage would hang this loop.
        for (int pass = 0; pass < 100000; ++pass) {
            const std::size_t got = reader.read(frames);
            total += got;

            if (got < frames.size()) {
                break;
            }
        }

        EXPECT_LE(total, bytes.size()) << "more frames than there were bytes";

        TraceLogSummary summary;
        (void)summarize(damaged, summary);
    }

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST(InputFuzzTests, DamagedProjectsAreResultsAndWhatLoadsCanBeValidatedAndBuilt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString goodPath = directory.filePath(QStringLiteral("good.tbsproj"));
    const QString damagedPath = directory.filePath(QStringLiteral("damaged.tbsproj"));

    {
        GraphDescription pipeline;
        pipeline.addNode(NodeDescription{
            .id = "can_1", .typeName = "can.source", .parameters = {}, .x = 0, .y = 0});
        pipeline.addNode(
            NodeDescription{.id = "bus", .typeName = "can.bus", .parameters = {}, .x = 0, .y = 80});
        pipeline.addNode(
            NodeDescription{.id = "ecu",
                            .typeName = "lua.ecu",
                            .parameters = {{"script", ParameterValue::fromText("-- x")},
                                           {"channel", ParameterValue::fromInteger(0)},
                                           {"gain", ParameterValue::fromReal(0.5)}},
                            .x = 100,
                            .y = 80});
        pipeline.addNode(NodeDescription{
            .id = "trace", .typeName = "trace.sink", .parameters = {}, .x = 200, .y = 0});
        pipeline.addEdge(EdgeDescription{"can_1", 0, "trace", 0});
        pipeline.addEdge(EdgeDescription{"bus", 0, "ecu", 0});
        pipeline.addEdge(EdgeDescription{"ecu", 0, "bus", 0, CanFormatMatch::ExtendedOnly});

        TransmitList transmit;
        DashboardDescription dashboard;

        ASSERT_TRUE(
            services::ProjectFile::save(goodPath, pipeline, transmit, dashboard).succeeded());
    }

    std::string original;
    {
        QFile file{goodPath};
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        original = file.readAll().toStdString();
    }

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    Random random{4};

    for (std::size_t round = 0; round < iterations(150); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        {
            QFile file{damagedPath};
            ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const std::string text = mutateText(original, random);
            file.write(text.data(), static_cast<qint64>(text.size()));
        }

        GraphDescription pipeline;
        TransmitList transmit;
        DashboardDescription dashboard;

        if (services::ProjectFile::load(damagedPath, pipeline, transmit, dashboard).failed()) {
            continue;
        }

        // What loaded is a description like any other: it validates or says why not,
        // and building it without an engine fails with a message rather than faulting.
        (void)pipeline.validate(catalog);

        PipelineGraph graph;
        (void)pipeline.build(catalog, NodeBuildContext{}, graph);
    }
}

TEST(InputFuzzTests, IsoTpHearsFramesThatLieAboutThemselves)
{
    Random random{5};

    for (const bool fd : {false, true}) {
        IsoTpAddress address;
        address.transmitId = 0x7E0;
        address.receiveId = 0x7E8;

        IsoTpConfig config;
        config.canFd = fd;

        IsoTpConnection connection{address, config};

        std::uint64_t now = 1;

        for (std::size_t round = 0; round < iterations(20000); ++round) {
            CanFrame frame;
            frame.identifier = random.chance(8) ? static_cast<std::uint32_t>(random.below(0x800))
                                                : address.receiveId;
            frame.format = CanFrameFormat::Standard;
            frame.fd = fd;

            // The first byte is the protocol control information, and half the time it is
            // one of the four it should be.
            frame.length = static_cast<std::uint8_t>(random.below(fd ? 65 : 9));
            frame.dlc = dlcFromPayloadLength(frame.length, fd);

            for (std::uint8_t& byte : frame.data) {
                byte = random.byte();
            }

            if (frame.length > 0 && random.chance(2)) {
                frame.data[0] =
                    static_cast<std::uint8_t>((random.below(4) << 4U) | random.below(16));
            }

            // A first frame that announces a huge message, a lot of the time.
            if (frame.length > 5 && random.chance(10)) {
                frame.data[0] = 0x10;
                frame.data[1] = 0x00;
                frame.data[2] = random.byte();
                frame.data[3] = random.byte();
                frame.data[4] = random.byte();
                frame.data[5] = random.byte();
            }

            now += 1'000'000ULL * random.below(300);

            EXPECT_NO_THROW(static_cast<void>(connection.onFrame(frame, now)));
            EXPECT_NO_THROW(connection.poll(now));

            if (random.chance(500)) {
                std::vector<std::uint8_t> message(random.below(300));
                for (std::uint8_t& byte : message) {
                    byte = random.byte();
                }
                static_cast<void>(connection.send(message, now));
            }

            connection.clearEvents();
            connection.clearPendingFrames();
        }
    }
}

TEST(InputFuzzTests, TheJ1939TransportHearsAnnouncementsThatLieAboutThemselves)
{
    Random random{6};

    J1939Transport transport;
    std::uint64_t now = 1;

    constexpr std::array<std::uint32_t, 4> kTransportPgns{
        0x0'EC00U, 0x0'EB00U, 0x0'C800U, 0x0'C700U};

    for (std::size_t round = 0; round < iterations(30000); ++round) {
        CanFrame frame;
        frame.format = CanFrameFormat::Extended;

        const std::uint32_t pgn = kTransportPgns[random.below(kTransportPgns.size())];
        const std::uint32_t destination =
            random.chance(3) ? 0xFFU : static_cast<std::uint32_t>(random.below(4));
        const auto source = static_cast<std::uint32_t>(random.below(4));

        frame.identifier = (6U << 26U) | (pgn << 8U) | (destination << 8U) | source;
        frame.identifier &= kMaxExtendedIdentifier;
        frame.length = random.chance(10) ? static_cast<std::uint8_t>(random.below(65)) : 8;
        frame.dlc = frame.length;

        for (std::uint8_t& byte : frame.data) {
            byte = random.byte();
        }

        // Announcements that are almost real: a control byte that exists, and a size.
        if (frame.length >= 8 && random.chance(2)) {
            constexpr std::array<std::uint8_t, 6> kControls{16, 17, 19, 20, 32, 255};
            frame.data[0] = kControls[random.below(kControls.size())];
        }

        now += 1'000'000ULL * random.below(400);

        EXPECT_NO_THROW(static_cast<void>(transport.onFrame(frame, now)));
        EXPECT_NO_THROW(transport.poll(now));
        transport.clearEvents();

        EXPECT_LT(transport.openSessions(), 1000U) << "sessions are accumulating";
    }
}

TEST(InputFuzzTests, TheUdsServerAnswersOrStaysSilentToAnyRequest)
{
    Random random{7};

    UdsServer server;
    server.setIdentifier(0xF190, UdsIdentifier{});
    server.addTroubleCode(0x123456);

    std::uint64_t now = 1;

    for (std::size_t round = 0; round < iterations(30000); ++round) {
        std::vector<std::uint8_t> request(random.below(40));
        for (std::uint8_t& byte : request) {
            byte = random.byte();
        }

        // The services this server knows, a lot of the time.
        constexpr std::array<std::uint8_t, 12> kServices{
            0x10, 0x11, 0x14, 0x19, 0x22, 0x27, 0x28, 0x2E, 0x31, 0x3E, 0x85, 0x23};
        if (!request.empty() && random.chance(2)) {
            request[0] = kServices[random.below(kServices.size())];
        }

        now += 1'000'000ULL * random.below(2000);

        EXPECT_NO_THROW(static_cast<void>(server.handle(request, now)));
        EXPECT_NO_THROW(server.poll(now));
    }
}

TEST(InputFuzzTests, TheUdsClientReadsAnyResponseToAnyRequest)
{
    Random random{8};

    UdsClient client;
    std::uint64_t now = 1;

    for (std::size_t round = 0; round < iterations(20000); ++round) {
        if (!client.isBusy()) {
            std::vector<std::uint8_t> request(1 + random.below(8));
            for (std::uint8_t& byte : request) {
                byte = random.byte();
            }
            static_cast<void>(client.request(std::move(request), now));
        }

        std::vector<std::uint8_t> response(random.below(20));
        for (std::uint8_t& byte : response) {
            byte = random.byte();
        }
        if (!response.empty() && random.chance(2)) {
            response[0] =
                random.chance(2) ? 0x7F : static_cast<std::uint8_t>(0x40 | random.below(0x40));
        }

        now += 1'000'000ULL * random.below(3000);

        EXPECT_NO_THROW(static_cast<void>(client.onMessage(response, now)));
        EXPECT_NO_THROW(client.poll(now));
        client.clearExchanges();
        client.clearPendingRequests();
    }
}

TEST(InputFuzzTests, TheLuaBindingsTakeAnyArgumentsAndAnswerWithAnErrorAtWorst)
{
    // What a script can call is a C++ function that reads a Lua stack, and a C++ function
    // that reads a Lua stack can read the wrong thing: an argument of the wrong type, a table
    // where a number was, an integer too large for the int it is stored in, a message
    // formatted with a conversion the formatter does not have. The last one was emit(), and
    // it ended the application. The bindings are called here with every kind of value, in
    // every position, from the top of a script and from a handler.
    constexpr std::array<const char*, 18> kBindings{"emit",
                                                    "emit_signal",
                                                    "decode",
                                                    "set_timer",
                                                    "log_message",
                                                    "get_time_us",
                                                    "every",
                                                    "cyclic",
                                                    "stop_cyclic",
                                                    "fault",
                                                    "bus_last",
                                                    "bus_stats",
                                                    "var_get",
                                                    "var_set",
                                                    "uds_did",
                                                    "uds_dtc",
                                                    "uds_clear_dtc",
                                                    "uds_session"};

    constexpr std::array<const char*, 34> kValues{"nil",
                                                  "true",
                                                  "false",
                                                  "0",
                                                  "-1",
                                                  "1",
                                                  "8",
                                                  "255",
                                                  "256",
                                                  "0x7FF",
                                                  "0x800",
                                                  "496",
                                                  "0x1FFFFFFF",
                                                  "0x20000000",
                                                  "2^31",
                                                  "math.maxinteger",
                                                  "math.mininteger",
                                                  "1e308",
                                                  "0/0",
                                                  "1/0",
                                                  "0.5",
                                                  "''",
                                                  "'x'",
                                                  "('x'):rep(8)",
                                                  "('x'):rep(9)",
                                                  "('x'):rep(64)",
                                                  "('x'):rep(65)",
                                                  "('\\xFF'):rep(4096)",
                                                  "'ExampleMessage'",
                                                  "'Temperature'",
                                                  "{}",
                                                  "{extended = true, fd = true, brs = true}",
                                                  "{1, 2, 3}",
                                                  "function() end"};

    TraceStore trace;
    SystemVariables variables;

    auto database = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parse(kDatabase, *database).succeeded());

    Random random{9};

    for (std::size_t round = 0; round < iterations(300); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        std::string script;

        const auto call = [&](const std::string& binding) {
            script += "pcall(" + binding;
            const std::size_t arguments = random.below(6);
            for (std::size_t index = 0; index < arguments; ++index) {
                script += ", ";
                script += kValues[random.below(kValues.size())];
            }
            script += ")\n";
        };

        for (std::size_t index = 0; index < 3 + random.below(8); ++index) {
            call(kBindings[random.below(kBindings.size())]);
        }

        script += "function on_message(id, data, channel, extended)\n";
        for (std::size_t index = 0; index < 2 + random.below(4); ++index) {
            call(kBindings[random.below(kBindings.size())]);
        }
        script += "end\n";

        LuaEcuNode ecu{script, "fuzz.lua"};
        ecu.setTraceStore(&trace);
        ecu.setSystemVariables(&variables);
        ecu.setDatabase(database);
        ecu.enableDiagnostics(IsoTpAddress{}, IsoTpConfig{});
        ecu.setLogHandler([](const std::string&, bool) { });

        // Whether the script loads is not the question - the pcall around each call means
        // it nearly always does. That it returns is.
        ASSERT_NO_THROW(static_cast<void>(ecu.prepare(16)));

        CanFrame frame;
        frame.identifier = random.chance(2) ? 496 : static_cast<std::uint32_t>(random.below(0x800));
        frame.length = static_cast<std::uint8_t>(random.below(9));
        frame.dlc = frame.length;
        for (std::uint8_t& byte : frame.data) {
            byte = random.byte();
        }

        const std::array<PortBatch, 1> inputs{PortBatch{std::span<const CanFrame>{&frame, 1}}};
        std::array<PortBatch, 1> outputs{};
        NodeContext context{inputs, outputs};

        for (int pass = 0; pass < 3; ++pass) {
            ASSERT_NO_THROW(ecu.process(context));
        }

        ecu.finish();
    }
}

namespace {

/// Frames from the fuzzer, a batch every pass. The lengths lie on purpose: a driver, a
/// plugin or a database can set that byte to anything.
class LyingSource final : public IPipelineNode {
public:
    explicit LyingSource(Random& random, std::vector<std::uint32_t> identifiers)
        : m_random{random}
        , m_identifiers{std::move(identifiers)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "fuzz.source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kPorts;
    }

    void process(NodeContext& context) override
    {
        m_frames.resize(1 + m_random.below(48));

        for (CanFrame& frame : m_frames) {
            frame = CanFrame{};
            frame.identifier = m_random.chance(3)
                                   ? static_cast<std::uint32_t>(m_random.below(0x2000'0000))
                                   : m_identifiers[m_random.below(m_identifiers.size())];
            frame.format = frame.identifier > kMaxStandardIdentifier || m_random.chance(4)
                               ? CanFrameFormat::Extended
                               : CanFrameFormat::Standard;
            frame.fd = m_random.chance(4);
            frame.rtr = m_random.chance(20);
            frame.error = m_random.chance(30);
            frame.length = m_random.chance(3) ? static_cast<std::uint8_t>(m_random.below(256))
                                              : static_cast<std::uint8_t>(m_random.below(9));
            frame.dlc = static_cast<std::uint8_t>(m_random.below(16));
            frame.timestampNs = m_clock += 1000 * m_random.below(5000);

            for (std::uint8_t& byte : frame.data) {
                byte = m_random.byte();
            }
        }

        context.publish<CanFrame>(0, m_frames);
    }

private:
    static constexpr std::array<PortDescriptor, 1> kPorts{
        PortDescriptor{"frames", PortType::Frames},
    };

    Random& m_random;
    std::vector<std::uint32_t> m_identifiers;
    std::vector<CanFrame> m_frames;
    std::uint64_t m_clock{0};
};

} // namespace

TEST(InputFuzzTests, TheBlocksHearFramesThatLieAboutThemselves)
{
    // The channel holds a received frame's length to the array before any block reads it. A
    // block is also reached from other blocks and from plugins, which have not been through the
    // channel, and the contract it keeps is its own: a frame that claims 255 bytes of 64 is read
    // as the 64 it has, and every other claim in it - a code that does not match, a remote
    // request with a payload, an error frame - is handled, not trusted.
    Random random{10};

    auto database = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parse(kDatabase, *database).succeeded());

    auto j1939Database = std::make_shared<CanDatabase>();
    ASSERT_TRUE(DbcParser::parseFile(std::string{TORQUEBUS_EXAMPLE_DATABASE_DIR} + "/j1939.dbc",
                                     *j1939Database)
                    .succeeded());

    for (std::size_t round = 0; round < iterations(60); ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);

        PipelineGraph graph;

        const NodeId source = graph.addNode(std::make_unique<LyingSource>(
            random,
            std::vector<std::uint32_t>{
                496, 0x18FEEE00, 0x18FECA00, 0x0CF00400, 0x7E0, 0x7E8, 0x18DA00F1, 0x101, 0x102}));

        const auto attach = [&](std::unique_ptr<IPipelineNode> node) {
            const NodeId id = graph.addNode(std::move(node));
            EXPECT_TRUE(graph.connect({source, 0}, {id, 0}).succeeded());
            return id;
        };

        attach(std::make_unique<DbcDecoderNode>(database, "fuzz"));
        attach(std::make_unique<J1939Node>(j1939Database, "fuzz"));
        attach(std::make_unique<FrameFilterNode>());
        attach(std::make_unique<TinyMlEcuNode>());
        attach(std::make_unique<IsoTpNode>(
            IsoTpAddress{}, IsoTpConfig{}, std::vector<std::uint8_t>{0x22, 0xF1, 0x90}, 20));
        attach(
            std::make_unique<UdsClientNode>(IsoTpAddress{}, IsoTpConfig{}, UdsTiming{}, nullptr));

        auto ecu = std::make_unique<LuaEcuNode>(
            R"(
            function on_message(id, data, channel, extended)
              local n = #data
              if n > 0 then local b = data:byte(1) end
              if id == 0x101 then emit(0x200, data:sub(1, 8)) end
            end
            )",
            "lying.lua");
        ecu->setLogHandler([](const std::string&, bool) { });
        attach(std::move(ecu));

        ASSERT_TRUE(graph.compile(64).succeeded());

        for (int pass = 0; pass < 12; ++pass) {
            ASSERT_NO_THROW(graph.execute());
        }

        ASSERT_NO_THROW(graph.finish());
        EXPECT_EQ(graph.faultedNodeCount(), 0U) << "a block threw on a frame it was handed";
    }
}
