// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a block does with a setting that is not what it asked for.
//
// A parameter reaches a block from a form, a project file somebody edited by hand, or
// a script that built a graph. The map it travels in is untyped, so for a long time a
// wrong value was not an error but a different value: a channel typed as 256 was
// channel 0 (the cast reduces it), "two" read as 0, a database given as a number read
// as no database at all. Each built a block that ran, on the wrong thing, and said
// nothing. The catalog now checks every declared parameter against its descriptor
// while the block is on screen, in the words of the form, and a block that cannot be
// built says so with its name - including when the code that builds it throws.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using namespace torquebus;

namespace {

[[nodiscard]] Result validate(const char* type, const NodeParameters& parameters)
{
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    return catalog.validateParameters(type, parameters, "block");
}

[[nodiscard]] bool mentions(const Result& result, std::string_view text)
{
    return std::string{result.message()}.find(text) != std::string::npos;
}

} // namespace

TEST(ParameterValidationTests, AChannelOutsideOneByteIsRefusedNotWrappedToAnotherChannel)
{
    // 256 used to be channel 0 and -1 used to be channel 255.
    for (const std::int64_t channel : {256, -1, 100000}) {
        NodeParameters parameters;
        parameters.set("channel", ParameterValue::fromInteger(channel));

        const Result result = validate("can.source", parameters);

        ASSERT_TRUE(result.failed()) << "channel " << channel;
        EXPECT_TRUE(mentions(result, "Channel")) << result.message();
        EXPECT_TRUE(mentions(result, "between 0 and 255")) << result.message();
    }

    NodeParameters fine;
    fine.set("channel", ParameterValue::fromInteger(3));
    EXPECT_TRUE(validate("can.source", fine).succeeded());
}

TEST(ParameterValidationTests, EveryBlockThatTakesAChannelChecksIt)
{
    NodeParameters parameters;
    parameters.set("channel", ParameterValue::fromInteger(999));
    parameters.set("script", ParameterValue::fromText("-- nothing"));

    for (const char* type : {"can.bus",
                             "can.source",
                             "can.transmit",
                             "lua.ecu",
                             "lua.test",
                             "transmit.list",
                             "uds.client",
                             "isotp.transport",
                             "tinyml.ecu"}) {
        EXPECT_TRUE(validate(type, parameters).failed()) << type;
    }
}

TEST(ParameterValidationTests, ATextWhereANumberBelongsIsRefusedNotReadAsZero)
{
    NodeParameters parameters;
    parameters.set("channel", ParameterValue::fromText("two"));

    const Result result = validate("can.source", parameters);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "whole number")) << result.message();
}

TEST(ParameterValidationTests, ANumberWithAFractionWhereAWholeNumberBelongsIsRefused)
{
    // 1.5 as a channel is not channel 1.
    NodeParameters parameters;
    parameters.set("channel", ParameterValue::fromReal(1.5));

    EXPECT_TRUE(validate("can.source", parameters).failed());
}

TEST(ParameterValidationTests, ANumberWhereTextBelongsIsRefusedNotReadAsEmpty)
{
    // "database": 5 built a decoder with no database, which is a decoder that decodes
    // nothing and says nothing.
    NodeParameters parameters;
    parameters.set("database", ParameterValue::fromInteger(5));

    const Result result = validate("dbc.decoder", parameters);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "Database")) << result.message();
    EXPECT_TRUE(mentions(result, "text")) << result.message();
}

TEST(ParameterValidationTests, ABooleanTakesTrueFalseOrAZeroOrAOneAndNothingElse)
{
    NodeParameters asBoolean;
    asBoolean.set("padding", ParameterValue::fromBoolean(false));
    EXPECT_TRUE(validate("uds.client", asBoolean).succeeded());

    // A project written by hand says "padding": 1, and that has always read as true.
    NodeParameters asOne;
    asOne.set("padding", ParameterValue::fromInteger(1));
    EXPECT_TRUE(validate("uds.client", asOne).succeeded());

    NodeParameters asSeven;
    asSeven.set("padding", ParameterValue::fromInteger(7));
    EXPECT_TRUE(validate("uds.client", asSeven).failed());

    NodeParameters asWord;
    asWord.set("padding", ParameterValue::fromText("yes"));
    EXPECT_TRUE(validate("uds.client", asWord).failed());
}

TEST(ParameterValidationTests, AnIdentifierIsAtMostTwentyNineBits)
{
    NodeParameters parameters;
    parameters.set("udsRequestId", ParameterValue::fromInteger(0x2000'0000));
    parameters.set("script", ParameterValue::fromText("-- nothing"));

    const Result result = validate("lua.ecu", parameters);

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "Diagnostic request ID")) << result.message();

    NodeParameters filter;
    filter.set("from", ParameterValue::fromInteger(-1));
    EXPECT_TRUE(validate("can.filter", filter).failed());

    NodeParameters inRange;
    inRange.set("from", ParameterValue::fromInteger(0x100));
    inRange.set("to", ParameterValue::fromInteger(0x1FFF'FFFF));
    EXPECT_TRUE(validate("can.filter", inRange).succeeded());
}

TEST(ParameterValidationTests, APeriodIsBoundedToADay)
{
    NodeParameters parameters;
    parameters.set("requestIntervalMs", ParameterValue::fromInteger(-5));
    EXPECT_TRUE(validate("isotp.transport", parameters).failed());

    NodeParameters huge;
    huge.set("requestIntervalMs", ParameterValue::fromInteger(std::int64_t{1} << 40));
    EXPECT_TRUE(validate("isotp.transport", huge).failed());

    NodeParameters fine;
    fine.set("requestIntervalMs", ParameterValue::fromInteger(500));
    EXPECT_TRUE(validate("isotp.transport", fine).succeeded());
}

TEST(ParameterValidationTests, ASpeedOfNothingOrOfNaNIsNotASpeed)
{
    NodeParameters zero;
    zero.set("path", ParameterValue::fromText("x.tblog"));
    zero.set("speed", ParameterValue::fromReal(0.0));
    EXPECT_TRUE(validate("log.source", zero).failed());

    NodeParameters notANumber;
    notANumber.set("path", ParameterValue::fromText("x.tblog"));
    notANumber.set("speed", ParameterValue::fromReal(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_TRUE(validate("log.source", notANumber).failed());

    NodeParameters infinite;
    infinite.set("path", ParameterValue::fromText("x.tblog"));
    infinite.set("speed", ParameterValue::fromReal(std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(validate("log.source", infinite).failed());

    NodeParameters half;
    half.set("path", ParameterValue::fromText("x.tblog"));
    half.set("speed", ParameterValue::fromInteger(2)); // an integer is a fine real
    EXPECT_TRUE(validate("log.source", half).succeeded());
}

TEST(ParameterValidationTests, TheMessageNamesTheBlockTheFieldAndTheValue)
{
    NodeParameters parameters;
    parameters.set("channel", ParameterValue::fromInteger(300));

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    const Result result = catalog.validateParameters("can.source", parameters, "can_7");

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "can_7")) << result.message();
    EXPECT_TRUE(mentions(result, "Channel")) << result.message();
    EXPECT_TRUE(mentions(result, "300")) << result.message();
}

TEST(ParameterValidationTests, ADescriptionWithABadValueDoesNotBuild)
{
    // The rule validate() and build() keep: whatever one rejects the other rejects.
    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "ch",
                        .typeName = "can.source",
                        .parameters = {{"channel", ParameterValue::fromInteger(256)}},
                        .x = 0.0,
                        .y = 0.0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    EXPECT_TRUE(description.validate(catalog).failed());

    PipelineGraph graph;
    EXPECT_TRUE(description.build(catalog, NodeBuildContext{}, graph).failed());
    EXPECT_EQ(graph.nodeCount(), 0U) << "a node was built before the description was checked";
}

TEST(ParameterValidationTests, AChannelNumberThatDoesNotExistIsAnErrorWhenTheBlockIsBuilt)
{
    // Asked of the creator directly, without a validate() first: it must not trust that
    // one happened.
    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();

    NodeParameters parameters;
    parameters.set("channel", ParameterValue::fromInteger(256));

    NodeBuildContext context;
    context.channel = [](std::uint8_t) -> CanChannel* { return nullptr; };

    std::unique_ptr<IPipelineNode> node;
    const Result result = catalog.create("can.source", parameters, context, "ch", node);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(node, nullptr);
    EXPECT_TRUE(mentions(result, "256")) << result.message();
}

// ---------------------------------------------------------------------------
// Code that is not ours
// ---------------------------------------------------------------------------

TEST(ParameterValidationTests, ACreatorThatThrowsIsABlockThatCouldNotBeBuilt)
{
    // A plugin's creator is code this repository has never seen, and Start is a button.
    NodeCatalog catalog;
    catalog.registerType(
        NodeTypeInfo{.typeName = "plugin.fragile", .displayName = "Fragile", .category = "Test"},
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::unique_ptr<IPipelineNode>&) -> Result {
            throw std::runtime_error{"disk on fire"};
        });

    std::unique_ptr<IPipelineNode> node;
    Result result = Result::ok();

    EXPECT_NO_THROW(result =
                        catalog.create("plugin.fragile", {}, NodeBuildContext{}, "frag_1", node));

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "frag_1")) << result.message();
    EXPECT_TRUE(mentions(result, "plugin.fragile")) << result.message();
    EXPECT_TRUE(mentions(result, "disk on fire")) << result.message();
}

TEST(ParameterValidationTests, ACreatorThatThrowsSomethingThatIsNotAnExceptionIsContainedToo)
{
    NodeCatalog catalog;
    catalog.registerType(
        NodeTypeInfo{.typeName = "plugin.odd", .displayName = "Odd", .category = "Test"},
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::unique_ptr<IPipelineNode>&) -> Result { throw 7; }); // NOLINT

    std::unique_ptr<IPipelineNode> node;
    Result result = Result::ok();

    EXPECT_NO_THROW(result = catalog.create("plugin.odd", {}, NodeBuildContext{}, "odd_1", node));
    EXPECT_TRUE(result.failed());
}

TEST(ParameterValidationTests, AValidatorThatThrowsIsABlockThatCouldNotBeChecked)
{
    // Validators run for every block on the canvas whenever one changes. One that throws
    // must not end the editor.
    NodeCatalog catalog;
    catalog.registerType(
        NodeTypeInfo{.typeName = "plugin.checker", .displayName = "Checker", .category = "Test"},
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::unique_ptr<IPipelineNode>&) -> Result { return Result::ok(); },
        [](const NodeParameters&, std::string_view) -> Result {
            throw std::runtime_error{"checker broke"};
        });

    Result result = Result::ok();
    EXPECT_NO_THROW(result = catalog.validateParameters("plugin.checker", {}, "chk_1"));

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "chk_1")) << result.message();
    EXPECT_TRUE(mentions(result, "checker broke")) << result.message();
}

TEST(ParameterValidationTests, ATapCreatorThatThrowsIsContained)
{
    NodeCatalog catalog;
    catalog.registerType(
        NodeTypeInfo{.typeName = "plugin.bus", .displayName = "Bus", .category = "Test"},
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::unique_ptr<IPipelineNode>&) -> Result { return Result::ok(); },
        {},
        [](const NodeParameters&,
           const NodeBuildContext&,
           std::string_view,
           std::size_t,
           std::unique_ptr<IPipelineNode>&) -> Result { throw std::runtime_error{"tap broke"}; });

    std::unique_ptr<IPipelineNode> node;
    Result result = Result::ok();

    EXPECT_NO_THROW(result =
                        catalog.createTap("plugin.bus", {}, NodeBuildContext{}, "bus_1", 0, node));
    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(mentions(result, "tap broke")) << result.message();
}
