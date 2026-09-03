// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The decoder is driven through a real PipelineGraph rather than by calling
// process() directly. Calling process() would test the decoding, which
// CanSignalTests already covers; running it through the graph tests the thing
// this milestone actually added, which is that Signals is a port type the
// executor can carry and typecheck.

#include <catch2/catch_test_macros.hpp>

#include "core/database/DbcParser.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/DbcDecoderNode.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/pipeline/nodes/SignalNodes.h"

#include <memory>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr const char* kVehicle = R"(
BO_ 257 VehicleSpeed: 8 ECU
 SG_ SpeedKmh : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECM
BO_ 258 EngineTemp: 8 ECU
 SG_ EngTemp : 0|8@1+ (1,-40) [-40|215] "degC" ECM
 SG_ Warning : 8|2@1+ (1,0) [0|3] "" ECM
)";

[[nodiscard]] std::shared_ptr<CanDatabase> vehicleDatabase()
{
    auto database = std::make_shared<CanDatabase>();
    REQUIRE(DbcParser::parse(kVehicle, *database).succeeded());
    return database;
}

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::vector<std::uint8_t> payload)
{
    CanFrame result;
    result.identifier = identifier;
    result.format = CanFrameFormat::Standard;
    result.length = static_cast<std::uint8_t>(payload.size());
    result.dlc = result.length;
    result.timestampNs = 1'000'000;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        result.data[i] = payload[i];
    }
    return result;
}

/// A source that publishes a fixed list of frames, so a pass is reproducible.
class StaticFrameSource final : public IPipelineNode {
public:
    explicit StaticFrameSource(std::vector<CanFrame> frames)
        : m_frames{std::move(frames)}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "test.source"; }
    [[nodiscard]] std::string displayName() const override { return "static source"; }
    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override { return {}; }
    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    void process(NodeContext& context) override
    {
        context.publish<CanFrame>(0, std::span<const CanFrame>{m_frames});
    }

private:
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
    };

    std::vector<CanFrame> m_frames;
};

/// What a pass delivered, flattened out of the batch.
///
/// The values are copied out rather than the DecodedSignals kept: a batch is
/// only valid inside the callback, which is a contract worth exercising in the
/// tests as well as stating in the header.
struct Captured final {
    std::string message;
    std::string name;
    double value{};
    std::int64_t raw{};
    bool truncated{};
};

[[nodiscard]] bool near(double actual, double expected)
{
    return (actual - expected) < 1e-9 && (expected - actual) < 1e-9;
}

/// Runs one pass of `frames` through a decoder and returns what came out.
[[nodiscard]] std::vector<Captured> decodeOnePass(std::shared_ptr<CanDatabase> database,
                                                  std::vector<CanFrame> frames,
                                                  DbcDecoderNode** decoderOut = nullptr)
{
    std::vector<Captured> captured;

    PipelineGraph graph;
    const NodeId source = graph.addNode(std::make_unique<StaticFrameSource>(std::move(frames)));

    auto decoderNode = std::make_unique<DbcDecoderNode>(std::move(database), "test decoder");
    DbcDecoderNode* decoder = decoderNode.get();
    const NodeId decoderId = graph.addNode(std::move(decoderNode));

    const NodeId sink = graph.addNode(std::make_unique<SignalSinkNode>(
        [&captured](std::span<const DecodedSignal> signals) {
            for (const DecodedSignal& signal : signals) {
                captured.push_back(Captured{std::string{signal.messageName()},
                                            std::string{signal.name()}, signal.value, signal.raw,
                                            signal.truncated});
            }
        }));

    REQUIRE(graph.connect(PortRef{source, 0}, PortRef{decoderId, 0}).succeeded());
    REQUIRE(graph.connect(PortRef{decoderId, 0}, PortRef{sink, 0}).succeeded());
    REQUIRE(graph.compile().succeeded());

    graph.execute();

    if (decoderOut != nullptr) {
        *decoderOut = decoder;
    }

    return captured;
}

} // namespace

TEST_CASE("Frames go in and named signal values come out", "[dbc][pipeline]")
{
    // 850 raw at 0.1 is 85.0 km/h; 0x6E unsigned with offset -40 is 70 degC.
    const std::vector<Captured> captured =
        decodeOnePass(vehicleDatabase(), {frame(0x101, {0x52, 0x03}), frame(0x102, {0x6E, 0x02})});

    REQUIRE(captured.size() == 3);

    CHECK(captured[0].message == "VehicleSpeed");
    CHECK(captured[0].name == "SpeedKmh");
    CHECK(near(captured[0].value, 85.0));
    CHECK(captured[0].raw == 850);

    CHECK(captured[1].name == "EngTemp");
    CHECK(near(captured[1].value, 70.0));

    CHECK(captured[2].name == "Warning");
    CHECK(near(captured[2].value, 2.0));
}

TEST_CASE("A frame the database does not describe is counted, not decoded",
          "[dbc][pipeline]")
{
    // Most traffic on a real bus is outside any one database. Logging each one
    // would bury the line that mattered; a count that reaches 100% of the
    // traffic is the tell that the wrong database is loaded.
    DbcDecoderNode* decoder = nullptr;
    const std::vector<Captured> captured =
        decodeOnePass(vehicleDatabase(),
                      {frame(0x101, {0x52, 0x03}), frame(0x7FF, {0xFF}), frame(0x300, {0x01})},
                      &decoder);

    CHECK(captured.size() == 1);
    REQUIRE(decoder != nullptr);
    CHECK(decoder->decodedFrames() == 1);
    CHECK(decoder->unknownFrames() == 2);
    CHECK(decoder->emittedSignals() == 1);
}

TEST_CASE("A frame shorter than the database says is flagged, not silently zero",
          "[dbc][pipeline]")
{
    // The distinction a decoder returning only a number would destroy: "the
    // sensor reads zero" and "we could not read the sensor" have to look
    // different, because one of them is a fault in the vehicle and the other is
    // a fault in the setup.
    DbcDecoderNode* decoder = nullptr;
    const std::vector<Captured> captured =
        decodeOnePass(vehicleDatabase(), {frame(0x101, {0x52})}, &decoder);

    REQUIRE(captured.size() == 1);
    CHECK(captured[0].name == "SpeedKmh");
    CHECK(captured[0].truncated);
    CHECK(captured[0].raw == 0);

    REQUIRE(decoder != nullptr);
    CHECK(decoder->truncatedSignals() == 1);
}

TEST_CASE("Remote and error frames carry nothing to decode", "[dbc][pipeline]")
{
    // A remote frame has a length but no payload. Decoding it would produce a
    // full set of plausible zeroes, which is the worst possible output: it
    // looks exactly like a real reading.
    CanFrame remote = frame(0x101, {0x52, 0x03});
    remote.rtr = true;

    CanFrame errored = frame(0x102, {0x6E, 0x02});
    errored.error = true;

    DbcDecoderNode* decoder = nullptr;
    const std::vector<Captured> captured =
        decodeOnePass(vehicleDatabase(), {remote, errored}, &decoder);

    CHECK(captured.empty());

    // And they are not counted as unknown either - the database knows them
    // perfectly well.
    REQUIRE(decoder != nullptr);
    CHECK(decoder->unknownFrames() == 0);
    CHECK(decoder->decodedFrames() == 0);
}

TEST_CASE("A Signals output cannot be wired to a Frames input", "[dbc][pipeline][types]")
{
    // The reason PortType exists, and the first time it has had two producers
    // to tell apart. A decoder emits signals; a channel transmits frames.
    // Connecting them is a mistake the diagram should refuse to draw, not one
    // the measurement should discover.
    PipelineGraph graph;

    const NodeId decoder =
        graph.addNode(std::make_unique<DbcDecoderNode>(vehicleDatabase(), "decoder"));
    const NodeId frameSink = graph.addNode(
        std::make_unique<FrameSinkNode>([](std::span<const CanFrame>) {}, "frames"));
    const NodeId signalSink = graph.addNode(
        std::make_unique<SignalSinkNode>([](std::span<const DecodedSignal>) {}, "signals"));

    const Result mismatch = graph.connect(PortRef{decoder, 0}, PortRef{frameSink, 0});
    CHECK(mismatch.failed());

    // The message has to name both types, or it sends the reader back to the
    // header to work out what they wired together.
    const std::string message{mismatch.message()};
    CHECK(message.find("Signals") != std::string::npos);
    CHECK(message.find("Frames") != std::string::npos);

    CHECK(graph.connect(PortRef{decoder, 0}, PortRef{signalSink, 0}).succeeded());
}

TEST_CASE("A decoder with no database compiles and decodes nothing",
          "[dbc][pipeline]")
{
    // What a block just dropped on the canvas is. Refusing to compile would
    // mean a project can only be built up in one order, which is not how
    // anybody uses a diagram.
    const std::vector<Captured> captured = decodeOnePass(nullptr, {frame(0x101, {0x52, 0x03})});

    CHECK(captured.empty());
}

TEST_CASE("A multiplexed message emits only the signals the frame carries",
          "[dbc][pipeline]")
{
    auto database = std::make_shared<CanDatabase>();
    REQUIRE(DbcParser::parse(R"(
BO_ 300 Muxed: 8 ECU
 SG_ Page M : 0|8@1+ (1,0) [0|255] "" ECM
 SG_ Voltage m0 : 8|16@1+ (0.001,0) [0|65.535] "V" ECM
 SG_ Current m1 : 8|16@1+ (0.01,-100) [-100|555.35] "A" ECM
)",
                             *database)
                .succeeded());

    // Page 0 then page 1, in one batch, from the same bytes. The switch has to
    // be read per frame, not once per pass.
    const std::vector<Captured> captured =
        decodeOnePass(database, {frame(0x12C, {0x00, 0xE8, 0x03}),
                                 frame(0x12C, {0x01, 0xE8, 0x03})});

    REQUIRE(captured.size() == 4);
    CHECK(captured[1].name == "Voltage");
    CHECK(near(captured[1].value, 1.0));
    CHECK(captured[3].name == "Current");
    CHECK(near(captured[3].value, -90.0));
}

TEST_CASE("The buffer is sized for the worst case, so a full batch is not clipped",
          "[dbc][pipeline]")
{
    // A decoder that quietly stops decoding when the bus gets busy is worse
    // than no decoder, because the gap looks like the signal went away. This
    // fills a batch with the widest message in the database and checks that
    // every signal of every frame came out.
    auto database = std::make_shared<CanDatabase>();
    REQUIRE(DbcParser::parse(kVehicle, *database).succeeded());

    constexpr std::size_t kFrames = 512;
    std::vector<CanFrame> frames;
    frames.reserve(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        frames.push_back(frame(0x102, {0x6E, 0x02}));
    }

    DbcDecoderNode* decoder = nullptr;
    const std::vector<Captured> captured = decodeOnePass(database, std::move(frames), &decoder);

    // EngineTemp carries two signals.
    CHECK(captured.size() == kFrames * 2);
    REQUIRE(decoder != nullptr);
    CHECK(decoder->emittedSignals() == kFrames * 2);
}
