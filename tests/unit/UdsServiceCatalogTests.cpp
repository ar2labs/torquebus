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

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace torquebus;

namespace {

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] const UdsServiceTemplate& form(std::uint8_t service)
{
    const UdsServiceTemplate* found = templateFor(service);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("An identifier goes out big-endian, from a form that says so", "[uds][form]")
{
    // The knowledge this file exists to hold. Typing 90 F1 by hand produces
    // "request out of range", which points at the identifier and not at the
    // byte order - and somebody can lose a morning there.
    Bytes request;

    REQUIRE(buildRequest(form(0x22), std::vector<std::string>{"F190"}, request).succeeded());
    CHECK(request == Bytes{0x22, 0xF1, 0x90});

    // However it is written.
    REQUIRE(buildRequest(form(0x22), std::vector<std::string>{"F1 90"}, request).succeeded());
    CHECK(request == Bytes{0x22, 0xF1, 0x90});

    REQUIRE(buildRequest(form(0x22), std::vector<std::string>{"0xF1 0x90"}, request).succeeded());
    CHECK(request == Bytes{0x22, 0xF1, 0x90});
}

TEST_CASE("A field of the wrong length says how many bytes it wanted", "[uds][form]")
{
    Bytes request;

    const Result result = buildRequest(form(0x22), std::vector<std::string>{"F1"}, request);

    REQUIRE(result.failed());

    const std::string message{result.message()};
    INFO(message);

    // Counted in bytes, which is the unit the field is in: "expected four
    // characters" invites counting spaces.
    CHECK(message.find("Identifier") != std::string::npos);
    CHECK(message.find("1 byte") != std::string::npos);
    CHECK(message.find("2 needed") != std::string::npos);
}

TEST_CASE("Half a byte is refused before it reaches the bus", "[uds][form]")
{
    Bytes request;

    const Result result = buildRequest(form(0x22), std::vector<std::string>{"F19"}, request);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("hex bytes") != std::string::npos);
}

TEST_CASE("A missing required field is named", "[uds][form]")
{
    Bytes request;

    // WriteDataByIdentifier with an identifier and no value.
    const Result result = buildRequest(form(0x2E), std::vector<std::string>{"2001", ""}, request);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("Value") != std::string::npos);
}

TEST_CASE("An optional field left empty is simply absent", "[uds][form]")
{
    // SecurityAccess with no key is a seed request, which is the ordinary first
    // half of the exchange - not an incomplete form.
    Bytes request;

    REQUIRE(buildRequest(form(0x27), std::vector<std::string>{"01", ""}, request).succeeded());
    CHECK(request == Bytes{0x27, 0x01});

    REQUIRE(buildRequest(form(0x27), std::vector<std::string>{"02", "AA BB CC DD"}, request)
                .succeeded());
    CHECK(request == Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD});
}

TEST_CASE("Fields are laid out in the order the standard puts them", "[uds][form]")
{
    Bytes request;

    // 0x2E: service, identifier, value.
    REQUIRE(buildRequest(form(0x2E), std::vector<std::string>{"2001", "01 02 03"}, request)
                .succeeded());
    CHECK(request == Bytes{0x2E, 0x20, 0x01, 0x01, 0x02, 0x03});

    // 0x31: service, control, routine, parameters.
    REQUIRE(buildRequest(form(0x31), std::vector<std::string>{"01", "0203", "FF"}, request)
                .succeeded());
    CHECK(request == Bytes{0x31, 0x01, 0x02, 0x03, 0xFF});

    // 0x19: service, report type, status mask.
    REQUIRE(buildRequest(form(0x19), std::vector<std::string>{"02", "FF"}, request).succeeded());
    CHECK(request == Bytes{0x19, 0x02, 0xFF});
}

TEST_CASE("A form left alone produces the request its defaults describe", "[uds][form]")
{
    // No values at all: every field falls back to its initial, which is what a
    // freshly opened form sends. Read VIN, all sessions extended, clear
    // everything - the answers somebody wants nine times in ten.
    Bytes request;

    REQUIRE(buildRequest(form(0x22), {}, request).succeeded());
    CHECK(request == Bytes{0x22, 0xF1, 0x90});

    REQUIRE(buildRequest(form(0x10), {}, request).succeeded());
    CHECK(request == Bytes{0x10, 0x03});

    REQUIRE(buildRequest(form(0x14), {}, request).succeeded());
    CHECK(request == Bytes{0x14, 0xFF, 0xFF, 0xFF});

    REQUIRE(buildRequest(form(0x3E), {}, request).succeeded());
    CHECK(request == Bytes{0x3E, 0x80});
}

TEST_CASE("A three-byte group is three bytes", "[uds][form]")
{
    Bytes request;

    const Result result = buildRequest(form(0x14), std::vector<std::string>{"FFFF"}, request);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("3 needed") != std::string::npos);
}

TEST_CASE("A service with no form is not an error", "[uds][form]")
{
    // A manufacturer-specific service is still sendable as hex; there is simply
    // no form for it, and the console falls back to the text box.
    CHECK(templateFor(0xBA) == nullptr);
    CHECK(templateFor(0x22) != nullptr);
}

TEST_CASE("Every form starts with its own service byte", "[uds][form]")
{
    // The service byte is not a field - it comes from the template - so a
    // template whose service disagreed with its own request would be wrong in a
    // way no single test above would catch.
    for (const UdsServiceTemplate& candidate : serviceTemplates()) {
        Bytes request;

        // Some forms have required fields with no default; those are expected
        // to fail, and what matters is that the ones that build start right.
        if (buildRequest(candidate, {}, request).succeeded()) {
            REQUIRE_FALSE(request.empty());
            CHECK(request.front() == candidate.service);
        }

        CHECK_FALSE(candidate.name.empty());
        CHECK_FALSE(candidate.fields.empty());
    }
}

TEST_CASE("Sub-function choices are values the service actually takes", "[uds][form]")
{
    // A choice list that offered a value the ECU refuses would be the form
    // teaching somebody the wrong thing.
    const UdsServiceTemplate& session = form(0x10);

    REQUIRE(session.choices.size() == 4);
    CHECK(session.choices[0].value == 0x01);
    CHECK(session.choices[2].value == 0x03);

    Bytes request;
    REQUIRE(buildRequest(session, std::vector<std::string>{"02"}, request).succeeded());
    CHECK(request == Bytes{0x10, 0x02});
}
