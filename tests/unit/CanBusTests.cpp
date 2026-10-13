// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The CAN Bus block: one network, with every module wired to it.
//
// The block is a promise about what runs - a module wired to the bus hears what
// is on it and what the module sends goes onto it - made by a drawing, so these
// tests run the drawing. A graph is described, built on an engine over a virtual
// bus, and frames are sent through it; what the bus delivers is counted. The
// structural cases then check that the promise is kept the way the architecture
// says it must be (a wire into a bus is a CAN Transmit of its own, a limited wire
// is a filter), because a graph that behaves and is built some other way is one
// the executor has two rules for.

#include <gtest/gtest.h>

#include "core/can/CanEngine.h"
#include "core/pipeline/BusGrouping.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "drivers/virtual/VirtualCanBackend.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace torquebus;

namespace {

[[nodiscard]] NodeDescription
block(std::string id, std::string type, NodeParameters parameters = {})
{
    return NodeDescription{.id = std::move(id),
                           .typeName = std::move(type),
                           .parameters = std::move(parameters),
                           .x = 0.0,
                           .y = 0.0};
}

[[nodiscard]] NodeParameters onChannel(std::int64_t channel)
{
    return NodeParameters{{"channel", ParameterValue::fromInteger(channel)}};
}

/// A Lua ECU that sends `reply` whenever it hears `trigger`.
///
/// Only that identifier, so the frames it sends are not answered in their turn:
/// the virtual bus echoes a transmit back to every reader, this one included.
[[nodiscard]] NodeDescription
ecuAnswering(std::string id, std::uint32_t trigger, std::uint32_t reply)
{
    return block(std::move(id),
                 "lua.ecu",
                 NodeParameters{{"script",
                                 ParameterValue::fromText(std::format(
                                     R"(function on_message(id)
                                            if id == {} then emit({}, "\1") end
                                        end)",
                                     trigger,
                                     reply))}});
}

/// Wires a module onto a bus the way the canvas draws one drop: what the bus
/// carries into the module, and what the module sends onto the bus.
void attach(GraphDescription& description,
            const std::string& bus,
            const std::string& module,
            CanFormatMatch hears = CanFormatMatch::Any)
{
    EdgeDescription listening{bus, 0, module, 0};
    listening.frameFormat = hears;

    description.addEdge(listening);
    description.addEdge(EdgeDescription{module, 0, bus, 0});
}

[[nodiscard]] CanFrame standardFrame(std::uint32_t identifier)
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.length = 1;
    frame.dlc = 1;
    return frame;
}

[[nodiscard]] CanFrame extendedFrame(std::uint32_t identifier)
{
    CanFrame frame = standardFrame(identifier);
    frame.format = CanFrameFormat::Extended;
    return frame;
}

[[nodiscard]] long long count(const std::vector<CanFrame>& frames, std::uint32_t identifier)
{
    return std::count_if(frames.begin(), frames.end(), [identifier](const CanFrame& frame) {
        return frame.identifier == identifier;
    });
}

/// One virtual channel, the described graph on it, and what was seen on the bus.
class BusRun final {
public:
    explicit BusRun(const GraphDescription& description, std::size_t channels = 1)
    {
        for (std::size_t index = 0; index < channels; ++index) {
            CanChannelConfig config;
            config.deviceHandle = std::format("virtual:{}", index);
            config.timing.bitrate = 500'000;
            EXPECT_TRUE(
                m_engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());
        }

        m_engine.addFrameSink([this](std::span<const CanFrame> batch) {
            const std::lock_guard lock{m_mutex};
            m_seen.insert(m_seen.end(), batch.begin(), batch.end());
        });

        m_engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());
        m_started = m_engine.start();
    }

    ~BusRun() { m_engine.stop(); }

    [[nodiscard]] const Result& started() const noexcept { return m_started; }
    [[nodiscard]] CanEngine& engine() noexcept { return m_engine; }

    void send(const CanFrame& frame, std::uint8_t channel = 0)
    {
        EXPECT_TRUE(m_engine.transmit(channel, frame).succeeded());
    }

    /// Everything the bus carried, once it has had time to carry it.
    [[nodiscard]] std::vector<CanFrame> settle()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{150});

        const std::lock_guard lock{m_mutex};
        return m_seen;
    }

private:
    CanEngine m_engine;
    Result m_started{Result::ok()};
    std::mutex m_mutex;
    std::vector<CanFrame> m_seen;
};

[[nodiscard]] std::size_t nodesOfType(CanEngine& engine, std::string_view typeName)
{
    std::size_t found = 0;
    for (const NodeId id : engine.graph().nodeIds()) {
        if (engine.graph().node(id)->typeName() == typeName) {
            ++found;
        }
    }
    return found;
}

} // namespace

// ---------------------------------------------------------------------------
// What the catalog says the bus is
// ---------------------------------------------------------------------------

TEST(CanBusTests, TheCatalogDeclaresTheBusAsOneBlockWithAnInputThatTakesAnyNumberOfWires)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    const NodeTypeInfo* bus = catalog.find(kCanBusType);
    ASSERT_TRUE(bus != nullptr);
    EXPECT_TRUE(bus->displayName == "CAN Bus");
    EXPECT_TRUE(bus->category == "Network");

    ASSERT_TRUE(bus->inputs.size() == 1);
    EXPECT_TRUE(bus->inputs.front().type == PortType::Frames);
    EXPECT_TRUE(bus->inputs.front().fanIn);

    ASSERT_TRUE(bus->outputs.size() == 1);
    EXPECT_TRUE(bus->outputs.front().type == PortType::Frames);
    EXPECT_FALSE(bus->outputs.front().fanIn);

    // The block a pipeline starts from, so the one a palette opens with.
    EXPECT_TRUE(catalog.types().front().typeName == kCanBusType);
}

TEST(CanBusTests, OnlyTheBlocksThatAnswerOnABusAreDeclaredToTalkOnIt)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    // An ECU answers; a filter, a decoder and a trace must not. The filter is the
    // one that matters: it has a Frames input and a Frames output like an ECU
    // does, and wiring its output back onto the bus it listens to is a loop.
    for (const char* talks :
         {"lua.ecu", "lua.test", "tinyml.ecu", "uds.client", "isotp.transport"}) {
        ASSERT_TRUE(catalog.find(talks) != nullptr) << talks;
        EXPECT_TRUE(catalog.find(talks)->talksOnBus) << talks;
    }

    for (const char* listens :
         {"can.filter", "dbc.decoder", "j1939.decoder", "trace.sink", "can.log"}) {
        ASSERT_TRUE(catalog.find(listens) != nullptr) << listens;
        EXPECT_FALSE(catalog.find(listens)->talksOnBus) << listens;
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST(CanBusTests, ABusTakesAWireFromEveryModuleWhereAnOrdinaryInputTakesOne)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription onABus;
    onABus.addNode(block("bus", "can.bus"));
    onABus.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    onABus.addNode(ecuAnswering("ecu_b", 0x100, 0x202));
    attach(onABus, "bus", "ecu_a");
    attach(onABus, "bus", "ecu_b");

    const Result valid = onABus.validate(catalog);
    SCOPED_TRACE(std::string{valid.message()});
    EXPECT_TRUE(valid.succeeded());

    // The rule that made the example project unstartable is still the rule for
    // everything that is not a bus: two wires into one CAN Transmit are refused.
    GraphDescription onATransmit;
    onATransmit.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    onATransmit.addNode(ecuAnswering("ecu_b", 0x100, 0x202));
    onATransmit.addNode(block("tx", "can.transmit"));
    onATransmit.addEdge(EdgeDescription{"ecu_a", 0, "tx", 0});
    onATransmit.addEdge(EdgeDescription{"ecu_b", 0, "tx", 0});

    const Result refused = onATransmit.validate(catalog);
    ASSERT_TRUE(refused.failed());
    EXPECT_TRUE(std::string{refused.message()}.find("tx") != std::string::npos);
}

TEST(CanBusTests, AFrameFormatIsRefusedOnAWireThatCarriesNoFrames)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    GraphDescription description;
    description.addNode(block(
        "decoder", "dbc.decoder", NodeParameters{{"database", ParameterValue::fromText("x.dbc")}}));
    description.addNode(block("plot", "signal.plot"));

    EdgeDescription wire{"decoder", 0, "plot", 0};
    wire.frameFormat = CanFormatMatch::ExtendedOnly;
    description.addEdge(wire);

    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    const std::string message{result.message()};
    EXPECT_TRUE(message.find("decoder") != std::string::npos);
    EXPECT_TRUE(message.find("plot") != std::string::npos);
    EXPECT_TRUE(message.find("Signals") != std::string::npos);
}

// ---------------------------------------------------------------------------
// What runs
// ---------------------------------------------------------------------------

TEST(CanBusTests, ModulesWiredToABusHearItAndAnswerOnIt)
{
    GraphDescription description;
    description.addNode(block("bus", "can.bus", onChannel(0)));
    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    description.addNode(ecuAnswering("ecu_b", 0x100, 0x202));
    attach(description, "bus", "ecu_a");
    attach(description, "bus", "ecu_b");

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    run.send(standardFrame(0x100));
    const std::vector<CanFrame> seen = run.settle();

    // Both heard the request and both answered: neither needed a CAN Channel
    // block to hear it, or a CAN Transmit block of its own to be heard.
    EXPECT_TRUE(count(seen, 0x201) >= 1);
    EXPECT_TRUE(count(seen, 0x202) >= 1);
}

TEST(CanBusTests, EveryWireIntoTheBusEndsInATransmitOnItsChannel)
{
    GraphDescription description;
    description.addNode(block("bus", "can.bus", onChannel(0)));
    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    description.addNode(ecuAnswering("ecu_b", 0x100, 0x202));
    description.addNode(ecuAnswering("ecu_c", 0x100, 0x203));
    attach(description, "bus", "ecu_a");
    attach(description, "bus", "ecu_b");
    attach(description, "bus", "ecu_c");

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    // Three modules transmit on the bus, so the executor walks three CAN
    // Transmit nodes - each with the one input a running node has - and one CAN
    // Channel for the bus to be read from.
    EXPECT_EQ(nodesOfType(run.engine(), "can.transmit"), 3U);
    EXPECT_EQ(nodesOfType(run.engine(), "can.filter"), 0U);
}

TEST(CanBusTests, AModuleThatOnlyListensAddsNoTransmit)
{
    GraphDescription description;
    description.addNode(block("bus", "can.bus", onChannel(0)));
    description.addNode(block("trace", "trace.sink"));
    description.addEdge(EdgeDescription{"bus", 0, "trace", 0});

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    EXPECT_EQ(nodesOfType(run.engine(), "can.transmit"), 0U);
}

TEST(CanBusTests, AWireLimitedToOneFrameFormatCarriesOnlyThoseFrames)
{
    // Three modules that would each answer to both a standard and an extended
    // frame. What tells them apart is the wire, and the proof is in what does
    // *not* come back: the one limited to 29-bit frames never answers the 11-bit
    // one, and the other way round - while the unlimited one, which proves the
    // bus runs at all, answers both.
    constexpr std::uint32_t kStandard = 0x100;
    constexpr std::uint32_t kExtended = 0x18FF0100;

    const auto answersBoth = [](std::string id, std::uint32_t reply) {
        return block(std::move(id),
                     "lua.ecu",
                     NodeParameters{{"script",
                                     ParameterValue::fromText(std::format(
                                         R"(function on_message(id)
                                                if id == {} or id == {} then emit({}, "\1") end
                                            end)",
                                         kStandard,
                                         kExtended,
                                         reply))}});
    };

    GraphDescription description;
    description.addNode(block("bus", "can.bus", onChannel(0)));
    description.addNode(answersBoth("hears_everything", 0x301));
    description.addNode(answersBoth("hears_11_bit", 0x302));
    description.addNode(answersBoth("hears_29_bit", 0x303));
    attach(description, "bus", "hears_everything");
    attach(description, "bus", "hears_11_bit", CanFormatMatch::StandardOnly);
    attach(description, "bus", "hears_29_bit", CanFormatMatch::ExtendedOnly);

    ASSERT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    run.send(standardFrame(kStandard));
    run.send(extendedFrame(kExtended));
    const std::vector<CanFrame> seen = run.settle();

    // Each limited module heard the half its wire lets through, and answered it...
    EXPECT_GE(count(seen, 0x302), 1);
    EXPECT_GE(count(seen, 0x303), 1);

    // ... and not the other half: the one with no limit, which heard both,
    // answered more than either. Compared with each other rather than with a
    // number, because the bus echoes what is sent and an answer may be counted
    // twice - the same way for all three.
    EXPECT_GT(count(seen, 0x301), count(seen, 0x302));
    EXPECT_GT(count(seen, 0x301), count(seen, 0x303));

    // And the limit is a filter in the graph - one per limited wire, named for
    // the wire so the Statistics panel can say which one rejected what.
    EXPECT_EQ(nodesOfType(run.engine(), "can.filter"), 2U);

    std::vector<std::string> names;
    for (const NodeId id : run.engine().graph().nodeIds()) {
        names.push_back(run.engine().graph().node(id)->displayName());
    }
    EXPECT_TRUE(std::ranges::any_of(names, [](const std::string& name) {
        return name.find("hears_29_bit") != std::string::npos
               && name.find("29-bit") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(names, [](const std::string& name) {
        return name.find("hears_11_bit") != std::string::npos
               && name.find("11-bit") != std::string::npos;
    }));
}

TEST(CanBusTests, AFrameFormatOnAnyWireOfFramesIsAFilterNotAThingOnlyTheBusDoes)
{
    // The limit is a property of the wire. Nothing about it is the bus's, so it
    // holds from any block that produces frames.
    constexpr std::uint32_t kStandard = 0x100;
    constexpr std::uint32_t kExtended = 0x18FF0100;

    GraphDescription description;
    description.addNode(block("can_1", "can.source", onChannel(0)));
    description.addNode(block("trace", "trace.sink"));

    EdgeDescription wire{"can_1", 0, "trace", 0};
    wire.frameFormat = CanFormatMatch::ExtendedOnly;
    description.addEdge(wire);

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    run.send(standardFrame(kStandard));
    run.send(extendedFrame(kExtended));
    (void)run.settle();

    EXPECT_EQ(nodesOfType(run.engine(), "can.filter"), 1U);
}

TEST(CanBusTests, ASwitchedOffBusSilencesEverythingWiredToIt)
{
    GraphDescription description;
    NodeDescription bus = block("bus", "can.bus", onChannel(0));
    bus.enabled = false;
    description.addNode(std::move(bus));
    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    attach(description, "bus", "ecu_a");

    // Off is off, and not an error: the module stays in the project, unwired.
    ASSERT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());

    BusRun run{description};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    run.send(standardFrame(0x100));
    const std::vector<CanFrame> seen = run.settle();

    EXPECT_EQ(count(seen, 0x201), 0);
    EXPECT_EQ(nodesOfType(run.engine(), "can.transmit"), 0U);
}

TEST(CanBusTests, ABusNamesTheChannelItIsAndTwoBusesAreTwoNetworks)
{
    GraphDescription description;
    description.addNode(block("can_1", "can.bus", onChannel(0)));
    description.addNode(block("can_2", "can.bus", onChannel(1)));
    description.addNode(ecuAnswering("on_first", 0x100, 0x201));
    description.addNode(ecuAnswering("on_second", 0x100, 0x202));
    attach(description, "can_1", "on_first");
    attach(description, "can_2", "on_second");

    BusRun run{description, 2};
    ASSERT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

    // A request on the second channel is heard by the module on the second bus
    // alone. Each virtual channel is a bus of its own.
    run.send(standardFrame(0x100), 1);
    const std::vector<CanFrame> seen = run.settle();

    EXPECT_GE(count(seen, 0x202), 1);
    EXPECT_EQ(count(seen, 0x201), 0);
}

TEST(CanBusTests, ABusOnAChannelThatIsNotConfiguredSaysSoInTheUsersNumbering)
{
    GraphDescription description;
    description.addNode(block("can_3", "can.bus", onChannel(2)));

    CanEngine engine;
    CanChannelConfig config;
    config.deviceHandle = "virtual:0";
    config.timing.bitrate = 500'000;
    ASSERT_TRUE(engine.addChannel(std::make_unique<VirtualCanBackend>(), config).succeeded());

    engine.setGraphDescription(description, NodeCatalog::withBuiltinTypes());
    const Result started = engine.start();

    ASSERT_TRUE(started.failed());
    const std::string message{started.message()};
    EXPECT_TRUE(message.find("can_3") != std::string::npos);
    EXPECT_TRUE(message.find("CAN 3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// From blocks per ECU to a bus
// ---------------------------------------------------------------------------

namespace {

/// The wiring the project had before there was a bus: a CAN Channel, and a CAN
/// Transmit for each ECU.
[[nodiscard]] GraphDescription legacyWiring()
{
    GraphDescription description;

    NodeDescription source = block("can_1", "can.source", onChannel(0));
    source.x = 10.0;
    source.y = 20.0;
    description.addNode(std::move(source));

    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    description.addNode(block("tx_a", "can.transmit", onChannel(0)));
    description.addNode(ecuAnswering("ecu_b", 0x100, 0x202));
    description.addNode(block("tx_b", "can.transmit", onChannel(0)));
    description.addNode(block("trace", "trace.sink"));

    description.addEdge(EdgeDescription{"can_1", 0, "ecu_a", 0});
    description.addEdge(EdgeDescription{"ecu_a", 0, "tx_a", 0});
    description.addEdge(EdgeDescription{"can_1", 0, "ecu_b", 0});
    description.addEdge(EdgeDescription{"ecu_b", 0, "tx_b", 0});
    description.addEdge(EdgeDescription{"can_1", 0, "trace", 0});

    return description;
}

} // namespace

TEST(CanBusTests, GroupingTheOldWiringOnABusPutsOneBusWhereTheChannelWasAndRemovesWhatItReplaces)
{
    GraphDescription description = legacyWiring();
    ASSERT_TRUE(canGroupOnBuses(description));

    const BusGrouping grouping = groupOnBuses(description);

    EXPECT_EQ(grouping.busesAdded, 1U);
    EXPECT_EQ(grouping.blocksRemoved, 3U); // The CAN Channel and both CAN Transmits.
    EXPECT_EQ(grouping.wiresMoved, 5U);
    EXPECT_TRUE(grouping.changed());

    // The bus is the CAN Channel, in its place under its name.
    const NodeDescription* bus = description.find("can_1");
    ASSERT_TRUE(bus != nullptr);
    EXPECT_TRUE(bus->typeName == kCanBusType);
    EXPECT_EQ(bus->parameters.integer("channel", -1), 0);
    EXPECT_EQ(bus->x, 10.0);
    EXPECT_EQ(bus->y, 20.0);
    EXPECT_TRUE(description.nodes().front().id == "can_1");

    EXPECT_TRUE(description.find("tx_a") == nullptr);
    EXPECT_TRUE(description.find("tx_b") == nullptr);

    // Each module listens on the bus and transmits on it; the trace only listens.
    const auto wired = [&description](const EdgeDescription& edge) {
        return std::ranges::find(description.edges(), edge) != description.edges().end();
    };
    EXPECT_TRUE(wired(EdgeDescription{"can_1", 0, "ecu_a", 0}));
    EXPECT_TRUE(wired(EdgeDescription{"ecu_a", 0, "can_1", 0}));
    EXPECT_TRUE(wired(EdgeDescription{"can_1", 0, "ecu_b", 0}));
    EXPECT_TRUE(wired(EdgeDescription{"ecu_b", 0, "can_1", 0}));
    EXPECT_TRUE(wired(EdgeDescription{"can_1", 0, "trace", 0}));
    EXPECT_EQ(description.edges().size(), 5U);

    EXPECT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());
    EXPECT_FALSE(canGroupOnBuses(description));
}

TEST(CanBusTests, AGroupedProjectRunsFrameForFrameAsItDidBefore)
{
    // The reason offering the action is safe: what the bus builds into is what
    // the blocks it replaced were.
    const auto answers = [](GraphDescription description) {
        BusRun run{description};
        EXPECT_TRUE(run.started().succeeded()) << std::string{run.started().message()};

        run.send(standardFrame(0x100));
        return run.settle();
    };

    GraphDescription grouped = legacyWiring();
    groupOnBuses(grouped);

    const std::vector<CanFrame> before = answers(legacyWiring());
    const std::vector<CanFrame> after = answers(grouped);

    for (const std::uint32_t reply : {0x201U, 0x202U}) {
        EXPECT_GE(count(before, reply), 1) << reply;
        EXPECT_EQ(count(after, reply) >= 1, count(before, reply) >= 1) << reply;
    }
}

TEST(CanBusTests, GroupingTwiceChangesNothingTheSecondTime)
{
    GraphDescription description = legacyWiring();
    groupOnBuses(description);

    const GraphDescription once = description;
    const BusGrouping again = groupOnBuses(description);

    EXPECT_FALSE(again.changed());
    EXPECT_EQ(again.busesAdded, 0U);
    EXPECT_EQ(again.blocksRemoved, 0U);
    EXPECT_TRUE(description == once);
}

TEST(CanBusTests, EachChannelThatWasWiredGetsABusOfItsOwn)
{
    GraphDescription description;
    description.addNode(block("can_1", "can.source", onChannel(0)));
    description.addNode(block("can_2", "can.source", onChannel(1)));
    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    description.addNode(block("tx_a", "can.transmit", onChannel(0)));
    description.addNode(block("trace", "trace.sink"));

    description.addEdge(EdgeDescription{"can_1", 0, "ecu_a", 0});
    description.addEdge(EdgeDescription{"ecu_a", 0, "tx_a", 0});
    description.addEdge(EdgeDescription{"can_2", 0, "trace", 0});

    const BusGrouping grouping = groupOnBuses(description);

    EXPECT_EQ(grouping.busesAdded, 2U);

    const NodeDescription* first = description.find("can_1");
    const NodeDescription* second = description.find("can_2");
    ASSERT_TRUE(first != nullptr && second != nullptr);
    EXPECT_TRUE(first->typeName == kCanBusType);
    EXPECT_TRUE(second->typeName == kCanBusType);
    EXPECT_EQ(first->parameters.integer("channel", -1), 0);
    EXPECT_EQ(second->parameters.integer("channel", -1), 1);

    EXPECT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());
}

TEST(CanBusTests, ABusThatIsAlreadyOnTheChannelIsTheOneThingsAreMovedOnto)
{
    GraphDescription description = legacyWiring();
    description.addNode(block("backbone", "can.bus", onChannel(0)));

    const BusGrouping grouping = groupOnBuses(description);

    EXPECT_EQ(grouping.busesAdded, 0U);
    EXPECT_TRUE(description.find("backbone") != nullptr);
    EXPECT_TRUE(std::ranges::find(description.edges(), EdgeDescription{"backbone", 0, "ecu_a", 0})
                != description.edges().end());
    EXPECT_TRUE(std::ranges::find(description.edges(), EdgeDescription{"ecu_a", 0, "backbone", 0})
                != description.edges().end());

    // No second bus on the one channel.
    const auto buses = std::ranges::count_if(description.nodes(), [](const NodeDescription& node) {
        return node.typeName == kCanBusType;
    });
    EXPECT_EQ(buses, 1);
}

TEST(CanBusTests, AChannelFeedingATransmitOnAnotherChannelIsAGatewayAndIsLeftAlone)
{
    GraphDescription description;
    description.addNode(block("from_can_1", "can.source", onChannel(0)));
    description.addNode(block("to_can_2", "can.transmit", onChannel(1)));
    description.addEdge(EdgeDescription{"from_can_1", 0, "to_can_2", 0});

    EXPECT_FALSE(canGroupOnBuses(description));

    const GraphDescription before = description;
    EXPECT_FALSE(groupOnBuses(description).changed());
    EXPECT_TRUE(description == before);
}

TEST(CanBusTests, TwoTransmitBlocksFedFromOneOutputBecomeOneWireToTheBus)
{
    GraphDescription description;
    description.addNode(ecuAnswering("ecu_a", 0x100, 0x201));
    description.addNode(block("tx_1", "can.transmit", onChannel(0)));
    description.addNode(block("tx_2", "can.transmit", onChannel(0)));
    description.addEdge(EdgeDescription{"ecu_a", 0, "tx_1", 0});
    description.addEdge(EdgeDescription{"ecu_a", 0, "tx_2", 0});

    const BusGrouping grouping = groupOnBuses(description);

    EXPECT_EQ(grouping.busesAdded, 1U);
    EXPECT_EQ(description.edges().size(), 1U);
    EXPECT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());
}

TEST(CanBusTests, GroupingKeepsTheFrameFormatOfAWire)
{
    GraphDescription description;
    description.addNode(block("can_1", "can.source", onChannel(0)));
    description.addNode(block("trace", "trace.sink"));

    EdgeDescription wire{"can_1", 0, "trace", 0};
    wire.frameFormat = CanFormatMatch::ExtendedOnly;
    description.addEdge(wire);

    groupOnBuses(description);

    ASSERT_EQ(description.edges().size(), 1U);
    EXPECT_TRUE(description.edges().front().fromNode == "can_1");
    EXPECT_TRUE(description.edges().front().frameFormat == CanFormatMatch::ExtendedOnly);
}

TEST(CanBusTests, ABlockNobodyWiredIsNotTouched)
{
    GraphDescription description;
    description.addNode(block("lonely", "can.source", onChannel(0)));
    description.addNode(block("tx", "can.transmit", onChannel(0)));

    EXPECT_FALSE(canGroupOnBuses(description));
    EXPECT_FALSE(groupOnBuses(description).changed());
    EXPECT_EQ(description.nodes().size(), 2U);
}

// ---------------------------------------------------------------------------
// A bus wired to itself
// ---------------------------------------------------------------------------

TEST(CanBusTests, ABusWiredToItselfIsRefusedBeforeItCanRepeatEveryFrameForever)
{
    // For most blocks a wire to themselves is a cycle, and compile() says so. A bus is
    // built as two nodes - one reads the channel, one writes it - so there is no cycle
    // in the graph, and the loop closes on the wire instead: everything the bus hears is
    // sent back onto it, heard, and sent again.
    GraphDescription description;
    description.addNode(block("bus", "can.bus", onChannel(0)));
    description.addEdge(EdgeDescription{"bus", 0, "bus", 0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    const Result result = description.validate(catalog);

    ASSERT_TRUE(result.failed());
    EXPECT_NE(std::string{result.message()}.find("'bus' is wired to itself"), std::string::npos)
        << result.message();

    PipelineGraph graph;
    EXPECT_TRUE(description.build(catalog, NodeBuildContext{}, graph).failed());
}

TEST(CanBusTests, TwoBusesThatAreTheSameChannelCannotBeWiredToEachOther)
{
    GraphDescription description;
    description.addNode(block("one", "can.bus", onChannel(1)));
    description.addNode(block("two", "can.bus", onChannel(1)));
    description.addEdge(EdgeDescription{"one", 0, "two", 0});

    const Result result = description.validate(NodeCatalog::withBuiltinTypes());

    ASSERT_TRUE(result.failed());
    EXPECT_NE(std::string{result.message()}.find("CAN 2"), std::string::npos) << result.message();
}

TEST(CanBusTests, TwoBusesThatAreDifferentChannelsMayBeJoinedAsAGateway)
{
    // The rule is about the loop, not about buses being wired together.
    GraphDescription description;
    description.addNode(block("one", "can.bus", onChannel(0)));
    description.addNode(block("two", "can.bus", onChannel(1)));
    description.addEdge(EdgeDescription{"one", 0, "two", 0});

    EXPECT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());
}

TEST(CanBusTests, ABusSwitchedOffIsNotHeldAgainstTheGraph)
{
    // validate() and build() agree about what is off: a wire to a block that is disabled
    // does not exist at build time, so it cannot be a loop.
    GraphDescription description;
    NodeDescription bus = block("bus", "can.bus", onChannel(0));
    bus.enabled = false;
    description.addNode(bus);
    description.addEdge(EdgeDescription{"bus", 0, "bus", 0});

    EXPECT_TRUE(description.validate(NodeCatalog::withBuiltinTypes()).succeeded());
}

TEST(CanBusTests, AnOrdinaryBlockWiredToItselfIsNamedAtValidateAndNotOnlyAtCompile)
{
    GraphDescription description;
    description.addNode(block(
        "ecu", "lua.ecu", NodeParameters{{"script", ParameterValue::fromText("-- nothing")}}));
    description.addEdge(EdgeDescription{"ecu", 0, "ecu", 0});

    const Result result = description.validate(NodeCatalog::withBuiltinTypes());

    ASSERT_TRUE(result.failed());
    EXPECT_NE(std::string{result.message()}.find("'ecu' is wired to itself"), std::string::npos)
        << result.message();
}
