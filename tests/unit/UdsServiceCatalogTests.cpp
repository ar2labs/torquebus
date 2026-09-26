// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The forms exist to spare people one specific piece of knowledge - that a UDS
// identifier is two bytes written big-endian - so most of these cases are about
// the bytes coming out in the right order and the wrong input being refused
// with a sentence somebody can act on.

#include "core/diagnostics/UdsServiceCatalog.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace torquebus;

namespace {

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] const UdsServiceTemplate& form(std::uint8_t service)
{
    const UdsServiceTemplate* found = templateFor(service);
    EXPECT_TRUE(found != nullptr);
    return *found;
}

} // namespace

TEST(UdsServiceCatalogTests, AnIdentifierGoesOutBigEndianFromAFormThatSaysSo)
{
    // The knowledge this file exists to hold. Typing 90 F1 by hand produces
    // "request out of range", which points at the identifier and not at the
    // byte order - and somebody can lose a morning there.
    Bytes request;

    ASSERT_TRUE(buildRequest(form(0x22), std::vector<std::string>{"F190"}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x22, 0xF1, 0x90}));

    // However it is written.
    ASSERT_TRUE(buildRequest(form(0x22), std::vector<std::string>{"F1 90"}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x22, 0xF1, 0x90}));

    ASSERT_TRUE(
        buildRequest(form(0x22), std::vector<std::string>{"0xF1 0x90"}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x22, 0xF1, 0x90}));
}

TEST(UdsServiceCatalogTests, AFieldOfTheWrongLengthSaysHowManyBytesItWanted)
{
    Bytes request;

    const Result result = buildRequest(form(0x22), std::vector<std::string>{"F1"}, request);

    ASSERT_TRUE(result.failed());

    const std::string message{result.message()};
    SCOPED_TRACE(::testing::Message() << message);

    // Counted in bytes, which is the unit the field is in: "expected four
    // characters" invites counting spaces.
    EXPECT_TRUE(message.find("Identifier") != std::string::npos);
    EXPECT_TRUE(message.find("1 byte") != std::string::npos);
    EXPECT_TRUE(message.find("2 needed") != std::string::npos);
}

TEST(UdsServiceCatalogTests, HalfAByteIsRefusedBeforeItReachesTheBus)
{
    Bytes request;

    const Result result = buildRequest(form(0x22), std::vector<std::string>{"F19"}, request);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("hex bytes") != std::string::npos);
}

TEST(UdsServiceCatalogTests, AMissingRequiredFieldIsNamed)
{
    Bytes request;

    // WriteDataByIdentifier with an identifier and no value.
    const Result result = buildRequest(form(0x2E), std::vector<std::string>{"2001", ""}, request);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("Value") != std::string::npos);
}

TEST(UdsServiceCatalogTests, AnOptionalFieldLeftEmptyIsSimplyAbsent)
{
    // SecurityAccess with no key is a seed request, which is the ordinary first
    // half of the exchange - not an incomplete form.
    Bytes request;

    ASSERT_TRUE(buildRequest(form(0x27), std::vector<std::string>{"01", ""}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x27, 0x01}));

    ASSERT_TRUE(buildRequest(form(0x27), std::vector<std::string>{"02", "AA BB CC DD"}, request)
                    .succeeded());
    EXPECT_TRUE((request == Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD}));
}

TEST(UdsServiceCatalogTests, FieldsAreLaidOutInTheOrderTheStandardPutsThem)
{
    Bytes request;

    // 0x2E: service, identifier, value.
    ASSERT_TRUE(buildRequest(form(0x2E), std::vector<std::string>{"2001", "01 02 03"}, request)
                    .succeeded());
    EXPECT_TRUE((request == Bytes{0x2E, 0x20, 0x01, 0x01, 0x02, 0x03}));

    // 0x31: service, control, routine, parameters.
    ASSERT_TRUE(buildRequest(form(0x31), std::vector<std::string>{"01", "0203", "FF"}, request)
                    .succeeded());
    EXPECT_TRUE((request == Bytes{0x31, 0x01, 0x02, 0x03, 0xFF}));

    // 0x19: service, report type, status mask.
    ASSERT_TRUE(
        buildRequest(form(0x19), std::vector<std::string>{"02", "FF"}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x19, 0x02, 0xFF}));
}

TEST(UdsServiceCatalogTests, AFormLeftAloneProducesTheRequestItsDefaultsDescribe)
{
    // No values at all: every field falls back to its initial, which is what a
    // freshly opened form sends. Read VIN, all sessions extended, clear
    // everything - the answers somebody wants nine times in ten.
    Bytes request;

    ASSERT_TRUE(buildRequest(form(0x22), {}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x22, 0xF1, 0x90}));

    ASSERT_TRUE(buildRequest(form(0x10), {}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x10, 0x03}));

    ASSERT_TRUE(buildRequest(form(0x14), {}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x14, 0xFF, 0xFF, 0xFF}));

    ASSERT_TRUE(buildRequest(form(0x3E), {}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x3E, 0x80}));
}

TEST(UdsServiceCatalogTests, AThreeByteGroupIsThreeBytes)
{
    Bytes request;

    const Result result = buildRequest(form(0x14), std::vector<std::string>{"FFFF"}, request);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("3 needed") != std::string::npos);
}

TEST(UdsServiceCatalogTests, AServiceWithNoFormIsNotAnError)
{
    // A manufacturer-specific service is still sendable as hex; there is simply
    // no form for it, and the console falls back to the text box.
    EXPECT_TRUE(templateFor(0xBA) == nullptr);
    EXPECT_TRUE(templateFor(0x22) != nullptr);
}

TEST(UdsServiceCatalogTests, EveryFormStartsWithItsOwnServiceByte)
{
    // The service byte is not a field - it comes from the template - so a
    // template whose service disagreed with its own request would be wrong in a
    // way no single test above would catch.
    for (const UdsServiceTemplate& candidate : serviceTemplates()) {
        Bytes request;

        // Some forms have required fields with no default; those are expected
        // to fail, and what matters is that the ones that build start right.
        if (buildRequest(candidate, {}, request).succeeded()) {
            ASSERT_FALSE(request.empty());
            EXPECT_TRUE(request.front() == candidate.service);
        }

        EXPECT_FALSE(candidate.name.empty());
        EXPECT_FALSE(candidate.fields.empty());
    }
}

TEST(UdsServiceCatalogTests, SubFunctionChoicesAreValuesTheServiceActuallyTakes)
{
    // A choice list that offered a value the ECU refuses would be the form
    // teaching somebody the wrong thing.
    const UdsServiceTemplate& session = form(0x10);

    ASSERT_TRUE(session.choices.size() == 4);
    EXPECT_TRUE(session.choices[0].value == 0x01);
    EXPECT_TRUE(session.choices[2].value == 0x03);

    Bytes request;
    ASSERT_TRUE(buildRequest(session, std::vector<std::string>{"02"}, request).succeeded());
    EXPECT_TRUE((request == Bytes{0x10, 0x02}));
}
