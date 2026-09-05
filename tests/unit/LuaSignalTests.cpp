// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The join between the scripting layer and the database layer.
//
// Before this, an ECU said what it meant in `string.pack("<I2", ...)` - which
// means every simulated ECU carries its own copy of a bit layout, in the one
// notation in this project that is easiest to get wrong and hardest to notice.
// A mis-packed frame transmits perfectly. It arrives, it is the right length,
// the trace shows it, and the number inside it is wrong.
//
// These tests are about that: a script naming a signal has to produce the bytes
// the database says, and reading one back has to give the number the database
// says.

#include <catch2/catch_test_macros.hpp>

#include "core/database/DbcParser.h"
#include "core/pipeline/PipelineGraph.h"
#include "core/pipeline/nodes/FrameNodes.h"
#include "core/scripting/LuaEcuNode.h"

#include <memory>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr const char* kVehicle = R"(
BO_ 257 VehicleSpeed: 8 ECU
 SG_ SpeedKmh : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECM
 SG_ Gear : 16|4@1+ (1,0) [0|15] "" ECM
BO_ 258 EngineTemp: 8 ECU
 SG_ EngTemp : 0|8@1+ (1,-40) [-40|215] "degC" ECM
)";

[[nodiscard]] std::shared_ptr<CanDatabase> vehicleDatabase()
{
    auto database = std::make_shared<CanDatabase>();
    REQUIRE(DbcParser::parse(kVehicle, *database).succeeded());
    return database;
}

/// One ECU, run for one pass, with the frames it produced collected.
///
/// The graph owns the node, so it is kept alive around every assertion - the
/// lesson from the decoder tests, where a helper returned a pointer into a
/// graph it had already destroyed.
class EcuPass final {
public:
    EcuPass(const std::string& script, std::shared_ptr<const CanDatabase> database,
            std::vector<CanFrame> incoming = {})
    {
        auto ecuNode = std::make_unique<LuaEcuNode>(script, "test ecu", std::uint8_t{0});
        m_ecu = ecuNode.get();

        if (database) {
            m_ecu->setDatabase(std::move(database));
        }

        m_ecu->setLogHandler([this](const std::string& text, bool isError) {
            m_log.push_back(text);
            if (isError) {
                m_errors.push_back(text);
            }
        });

        const NodeId source =
            m_graph.addNode(std::make_unique<StaticSource>(std::move(incoming)));
        const NodeId ecu = m_graph.addNode(std::move(ecuNode));
        const NodeId sink = m_graph.addNode(std::make_unique<FrameSinkNode>(
            [this](std::span<const CanFrame> frames) {
                m_produced.insert(m_produced.end(), frames.begin(), frames.end());
            }));

        REQUIRE(m_graph.connect(PortRef{source, 0}, PortRef{ecu, 0}).succeeded());
        REQUIRE(m_graph.connect(PortRef{ecu, 0}, PortRef{sink, 0}).succeeded());

        // Recorded, not required. A script whose on_enable is *meant* to fail
        // fails the compile, and half these cases are about exactly that - so
        // requiring success here would abort the tests that matter most.
        m_compile = m_graph.compile();
        if (m_compile.succeeded()) {
            m_graph.execute();
        } else {
            m_errors.push_back(std::string{m_compile.message()});
        }
    }

    EcuPass(const EcuPass&) = delete;
    EcuPass& operator=(const EcuPass&) = delete;

    [[nodiscard]] const std::vector<CanFrame>& produced() const noexcept { return m_produced; }
    [[nodiscard]] const std::vector<std::string>& log() const noexcept { return m_log; }
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return m_errors; }
    [[nodiscard]] const LuaEcuNode& ecu() const noexcept { return *m_ecu; }

    /// Whether the graph compiled - which is where an on_enable failure lands.
    [[nodiscard]] const Result& compileResult() const noexcept { return m_compile; }

private:
    /// Publishes a fixed list of frames so a pass is reproducible.
    class StaticSource final : public IPipelineNode {
    public:
        explicit StaticSource(std::vector<CanFrame> frames)
            : m_frames{std::move(frames)}
        {
        }

        [[nodiscard]] std::string_view typeName() const noexcept override
        {
            return "test.source";
        }
        [[nodiscard]] std::string displayName() const override { return "static"; }
        [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
        {
            return {};
        }
        [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
        {
            return kOutputs;
        }

        void process(NodeContext& context) override
        {
            if (!m_frames.empty()) {
                context.publish<CanFrame>(0, std::span<const CanFrame>{m_frames});
            }
        }

    private:
        static constexpr std::array<PortDescriptor, 1> kOutputs{
            PortDescriptor{"frames", PortType::Frames},
        };

        std::vector<CanFrame> m_frames;
    };

    std::vector<CanFrame> m_produced;
    std::vector<std::string> m_log;
    std::vector<std::string> m_errors;
    LuaEcuNode* m_ecu{nullptr};
    Result m_compile;
    PipelineGraph m_graph;
};

[[nodiscard]] CanFrame frame(std::uint32_t identifier, std::vector<std::uint8_t> payload)
{
    CanFrame result;
    result.identifier = identifier;
    result.format = CanFrameFormat::Standard;
    result.length = static_cast<std::uint8_t>(payload.size());
    result.dlc = result.length;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        result.data[i] = payload[i];
    }
    return result;
}

} // namespace

TEST_CASE("A script naming a signal produces the bytes the database specifies",
          "[lua][dbc]")
{
    const EcuPass pass{R"(
function on_enable()
    emit_signal("VehicleSpeed", { SpeedKmh = 85.0, Gear = 3 })
end
)",
                       vehicleDatabase()};

    REQUIRE(pass.produced().size() == 1);

    const CanFrame& sent = pass.produced().front();
    CHECK(sent.identifier == 0x101);
    CHECK(sent.length == 8);

    // 85.0 at a factor of 0.1 is raw 850, which Intel order puts on the wire as
    // 52 03; the gear is a nibble in byte 2.
    CHECK(sent.data[0] == 0x52);
    CHECK(sent.data[1] == 0x03);
    CHECK(sent.data[2] == 0x03);
}

TEST_CASE("Signals the script did not set stay zero", "[lua][dbc]")
{
    // The table is walked, not the message's signal list. A script setting one
    // of two signals means the other is zero - which is what makeFrame already
    // gave it, and what anyone writing the script would expect.
    const EcuPass pass{R"(
function on_enable()
    emit_signal("VehicleSpeed", { Gear = 5 })
end
)",
                       vehicleDatabase()};

    REQUIRE(pass.produced().size() == 1);
    CHECK(pass.produced().front().data[0] == 0x00);
    CHECK(pass.produced().front().data[1] == 0x00);
    CHECK(pass.produced().front().data[2] == 0x05);
}

TEST_CASE("A misspelled name stops the script; a value out of range does not",
          "[lua][dbc]")
{
    // The asymmetry is the whole design. A typo never becomes correct, so the
    // script should stop and say which name was wrong. A value out of range is
    // a number the simulation produced, and taking the ECU down over it would
    // take the rest of the simulation with it.
    SECTION("an unknown message name is an error")
    {
        const EcuPass pass{R"(
function on_enable()
    emit_signal("VehcileSpeed", { SpeedKmh = 10.0 })
end
)",
                           vehicleDatabase()};

        CHECK(pass.produced().empty());
        REQUIRE_FALSE(pass.errors().empty());
        CHECK(pass.errors().front().find("VehcileSpeed") != std::string::npos);
    }

    SECTION("an unknown signal name is an error, and names the message too")
    {
        const EcuPass pass{R"(
function on_enable()
    emit_signal("VehicleSpeed", { SpeedKph = 10.0 })
end
)",
                           vehicleDatabase()};

        CHECK(pass.produced().empty());
        REQUIRE_FALSE(pass.errors().empty());
        CHECK(pass.errors().front().find("SpeedKph") != std::string::npos);
        CHECK(pass.errors().front().find("VehicleSpeed") != std::string::npos);
    }

    SECTION("a value past the field's width saturates, and the frame still goes")
    {
        const EcuPass pass{R"(
function on_enable()
    emit_signal("VehicleSpeed", { SpeedKmh = 99999.0 })
end
)",
                           vehicleDatabase()};

        REQUIRE(pass.produced().size() == 1);
        CHECK(pass.errors().empty());
        CHECK(pass.ecu().saturatedSignals() == 1);

        // Saturated at the widest the 16 bits hold, not wrapped: a torque
        // request of 300% arriving as -56% is the failure that moves an
        // actuator.
        CHECK(pass.produced().front().data[0] == 0xFF);
        CHECK(pass.produced().front().data[1] == 0xFF);
    }
}

TEST_CASE("A script can read an incoming frame by signal name", "[lua][dbc]")
{
    const EcuPass pass{R"(
function on_message(id, data, channel, extended)
    local name, signals = decode(id, data)
    if name ~= nil then
        log_message(string.format("%s SpeedKmh=%.1f", name, signals.SpeedKmh))
    end
end
)",
                       vehicleDatabase(),
                       {frame(0x101, {0x52, 0x03, 0x00})}};

    REQUIRE(pass.log().size() == 1);
    CHECK(pass.log().front().find("VehicleSpeed") != std::string::npos);
    CHECK(pass.log().front().find("85.0") != std::string::npos);
}

TEST_CASE("decode returns nil for a frame the database does not describe",
          "[lua][dbc]")
{
    // What an ECU on a shared bus does all day: look at everything, act on the
    // few identifiers it owns. Returning nil rather than an empty table lets
    // the script write `if name then` and be done.
    const EcuPass pass{R"(
function on_message(id, data, channel, extended)
    local name = decode(id, data)
    log_message(name == nil and "unknown" or name)
end
)",
                       vehicleDatabase(),
                       {frame(0x101, {0x52, 0x03, 0x00}), frame(0x7FF, {0xFF})}};

    REQUIRE(pass.log().size() == 2);
    CHECK(pass.log()[0].find("VehicleSpeed") != std::string::npos);
    CHECK(pass.log()[1].find("unknown") != std::string::npos);
}

TEST_CASE("Without a database, decode says nothing and emit_signal says why",
          "[lua][dbc]")
{
    SECTION("decode returns nil rather than failing")
    {
        const EcuPass pass{R"(
function on_message(id, data, channel, extended)
    local name = decode(id, data)
    log_message(name == nil and "no database" or name)
end
)",
                           nullptr,
                           {frame(0x101, {0x52, 0x03})}};

        REQUIRE(pass.log().size() == 1);
        CHECK(pass.log().front().find("no database") != std::string::npos);
    }

    SECTION("emit_signal reports the missing setting by name")
    {
        // The message has to name the parameter the user needs to fill in. "no
        // database" alone sends somebody looking through a script that is fine.
        const EcuPass pass{R"(
function on_enable()
    emit_signal("VehicleSpeed", { SpeedKmh = 10.0 })
end
)",
                           nullptr};

        REQUIRE_FALSE(pass.errors().empty());
        CHECK(pass.errors().front().find("database") != std::string::npos);
    }
}

TEST_CASE("What a script writes is what the decoder reads back", "[lua][dbc]")
{
    // The round trip across both layers: the script encodes by name, and the
    // database decodes the bytes it produced. It fails if either direction
    // drifts, which asserting on bytes alone would not.
    const auto database = vehicleDatabase();

    const EcuPass pass{R"(
function on_enable()
    emit_signal("EngineTemp", { EngTemp = 70.0 })
end
)",
                       database};

    REQUIRE(pass.produced().size() == 1);

    const CanMessage* message = database->find(pass.produced().front());
    REQUIRE(message != nullptr);
    CHECK(message->name == "EngineTemp");

    const CanSignal* temperature = message->findSignal("EngTemp");
    REQUIRE(temperature != nullptr);
    CHECK(temperature->decode(pass.produced().front()) == 70.0);
}
