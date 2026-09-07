// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// UDS is a small protocol with two hard parts, and both are about time: an ECU
// that needs longer than fifty milliseconds says so with 0x78 rather than by
// being slow, and a session that stops being talked to expires. Everything else
// here is vocabulary - which matters too, because the difference between
// showing `7F 22 31` and showing "the ECU does not have that identifier" is
// most of what a diagnostic tool is for.
//
// Time is a parameter, so a five-second timeout is a test that runs instantly.

#include "core/diagnostics/UdsClient.h"
#include "core/diagnostics/UdsTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint64_t kMillisecond = 1'000'000ULL;

using Bytes = std::vector<std::uint8_t>;

} // namespace

TEST_CASE("A positive answer is the service plus 0x40", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    REQUIRE(client.pendingRequests().size() == 1);
    CHECK(client.pendingRequests().front() == Bytes{0x22, 0xF1, 0x90});
    CHECK(client.isBusy());

    CHECK(client.onMessage(Bytes{0x62, 0xF1, 0x90, 'W', 'V', 'W'}, 12 * kMillisecond));

    REQUIRE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    CHECK(exchange.outcome == UdsExchange::Outcome::Positive);
    CHECK(exchange.elapsedNs == 12 * kMillisecond);
    CHECK_FALSE(client.isBusy());
}

TEST_CASE("Reading the VIN is not a request for silence", "[uds]")
{
    // 0x22 0xF1 0x90 has bit 7 set in its second byte, because 0xF1 is half of
    // an identifier. A client that reads that bit as the suppress-positive-
    // response flag - without first asking whether this service even has a
    // sub-function - sends the commonest request in the protocol and then never
    // waits for its answer. This is that check.
    CHECK_FALSE(suppressesResponse(readDataByIdentifier(0xF190)));
    CHECK_FALSE(hasSubFunction(0x22));

    CHECK(suppressesResponse(testerPresent(true)));
    CHECK(hasSubFunction(0x3E));

    UdsClient client;
    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    CHECK(client.isBusy());
}

TEST_CASE("A suppressed request is not waited for", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(testerPresent(true), 0).succeeded());

    CHECK(client.pendingRequests().front() == Bytes{0x3E, 0x80});
    CHECK_FALSE(client.isBusy());

    // And no timeout is invented for an answer nobody asked for.
    client.poll(10'000 * kMillisecond);
    CHECK(client.exchanges().empty());
}

TEST_CASE("Response pending is not a failure, it is a longer clock", "[uds]")
{
    // The single most important behaviour in this file. An ECU that has to
    // erase a flash sector before answering sends 0x78 repeatedly; a tester
    // that treats it as an error gives up on every ECU worth talking to.
    UdsClient client;

    REQUIRE(client.request(Bytes{0x31, 0x01, 0xFF, 0x00}, 0).succeeded());

    // P2 would have expired here.
    CHECK(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 40 * kMillisecond));
    client.poll(60 * kMillisecond);

    CHECK(client.exchanges().empty());
    CHECK(client.isBusy());

    // And again, four seconds later - still inside P2*.
    CHECK(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 4000 * kMillisecond));
    client.poll(4500 * kMillisecond);

    CHECK(client.exchanges().empty());

    CHECK(client.onMessage(Bytes{0x71, 0x01, 0xFF, 0x00}, 6000 * kMillisecond));

    REQUIRE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    CHECK(exchange.outcome == UdsExchange::Outcome::Positive);

    // Both are worth showing: four seconds with nine "still working" answers is
    // a different story from four seconds of silence.
    CHECK(exchange.pendingCount == 2);
    CHECK(exchange.elapsedNs == 6000 * kMillisecond);
}

TEST_CASE("Even a pending ECU is given up on eventually", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(Bytes{0x31, 0x01, 0xFF, 0x00}, 0).succeeded());
    CHECK(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 10 * kMillisecond));

    client.poll(5009 * kMillisecond);
    CHECK(client.exchanges().empty());

    client.poll(5011 * kMillisecond);

    REQUIRE(client.exchanges().size() == 1);
    CHECK(client.exchanges().front().outcome == UdsExchange::Outcome::Timeout);
}

TEST_CASE("An ECU that says nothing at all times out at P2", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    client.poll(49 * kMillisecond);
    CHECK(client.exchanges().empty());

    client.poll(50 * kMillisecond);

    REQUIRE(client.exchanges().size() == 1);
    CHECK(client.exchanges().front().outcome == UdsExchange::Outcome::Timeout);
    CHECK_FALSE(client.isBusy());
}

TEST_CASE("A refusal is reported with the reason, in words", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x7F, 0x22, 0x31}, 5 * kMillisecond));

    REQUIRE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    CHECK(exchange.outcome == UdsExchange::Outcome::Negative);
    CHECK(exchange.negativeResponse == 0x31);

    // The whole point of the vocabulary: "7F 22 31" tells an engineer nothing
    // they could not read off the bus themselves.
    const std::string text = exchange.describe();
    INFO(text);
    CHECK(text.find("ReadDataByIdentifier") != std::string::npos);
    CHECK(text.find("out of range") != std::string::npos);
}

TEST_CASE("An answer to somebody else's request is not ours", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    // A refusal naming a service we did not ask for.
    CHECK_FALSE(client.onMessage(Bytes{0x7F, 0x19, 0x31}, kMillisecond));
    CHECK(client.exchanges().empty());
    CHECK(client.isBusy());
}

TEST_CASE("An answer to the wrong service is reported rather than ignored",
          "[uds]")
{
    // On a bus where this happens - two testers, or an ECU answering late -
    // knowing that it happened is the whole diagnosis.
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x50, 0x03}, kMillisecond));

    REQUIRE(client.exchanges().size() == 1);
    CHECK(client.exchanges().front().outcome == UdsExchange::Outcome::Mismatch);
}

TEST_CASE("A session is believed from the answer, not from the request",
          "[uds]")
{
    // An ECU may answer a request for the programming session with the extended
    // one. Believing the request is how a tester becomes certain of a session
    // it is not in - and then blames the next refusal on something else.
    UdsClient client;

    REQUIRE(client.request(diagnosticSessionControl(UdsSession::Programming), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, 5 * kMillisecond));

    CHECK(client.session() == UdsSession::Extended);
}

TEST_CASE("A non-default session is kept alive without being asked", "[uds]")
{
    // An ECU drops to the default session after S3 of silence, taking security
    // access with it. Forgetting the heartbeat produces a failure that appears
    // minutes later and somewhere else, which is why this is the client's job
    // and not the caller's.
    UdsClient client;

    REQUIRE(client.request(diagnosticSessionControl(UdsSession::Extended), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x50, 0x03}, 5 * kMillisecond));

    client.clearPendingRequests();

    client.poll(1000 * kMillisecond);
    CHECK(client.pendingRequests().empty());

    // Two fifths of S3.
    client.poll(2005 * kMillisecond);

    REQUIRE(client.pendingRequests().size() == 1);
    CHECK(client.pendingRequests().front() == Bytes{0x3E, 0x80});
}

TEST_CASE("The default session needs no heartbeat", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(diagnosticSessionControl(UdsSession::Default), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x50, 0x01}, 5 * kMillisecond));

    client.clearPendingRequests();
    client.poll(60'000 * kMillisecond);

    CHECK(client.pendingRequests().empty());
}

TEST_CASE("A heartbeat never takes the slot a real request needs", "[uds]")
{
    // A keep-alive that blocked a request would be a mechanism that stops the
    // work it exists to protect.
    UdsClient client;

    REQUIRE(client.request(diagnosticSessionControl(UdsSession::Extended), 0).succeeded());
    CHECK(client.onMessage(Bytes{0x50, 0x03}, 0));

    REQUIRE(client.request(readDataByIdentifier(0xF190), 2005 * kMillisecond).succeeded());
    client.clearPendingRequests();

    client.poll(2005 * kMillisecond);

    // Busy with a real exchange: no heartbeat, and no refusal either.
    CHECK(client.pendingRequests().empty());
    CHECK(client.isBusy());
}

TEST_CASE("Two questions at once are refused", "[uds]")
{
    UdsClient client;

    REQUIRE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    CHECK(client.request(readDataByIdentifier(0xF18C), 0).failed());
}

TEST_CASE("The request builders put the bytes where the standard says", "[uds]")
{
    CHECK(readDataByIdentifier(0xF190) == Bytes{0x22, 0xF1, 0x90});
    CHECK(writeDataByIdentifier(0x2001, {0x01, 0x02}) == Bytes{0x2E, 0x20, 0x01, 0x01, 0x02});
    CHECK(diagnosticSessionControl(UdsSession::Extended) == Bytes{0x10, 0x03});
    CHECK(ecuReset(0x01) == Bytes{0x11, 0x01});
    CHECK(readDtcByStatusMask(0xFF) == Bytes{0x19, 0x02, 0xFF});
    CHECK(clearDiagnosticInformation() == Bytes{0x14, 0xFF, 0xFF, 0xFF});
    CHECK(securityAccessSeed(0x01) == Bytes{0x27, 0x01});

    // The key goes back on the level *after* the seed's - sending it on the
    // seed's own level is refused as a sequence error that says nothing about
    // the cause.
    CHECK(securityAccessKey(0x01, {0xAA, 0xBB}) == Bytes{0x27, 0x02, 0xAA, 0xBB});
}

TEST_CASE("Trouble codes are named the way a workshop manual names them",
          "[uds][dtc]")
{
    // The raw number is nothing like the form every scan tool shows: the first
    // two bits are the system letter and the next two the leading digit.
    const std::vector<DiagnosticTroubleCode> codes =
        parseDtcResponse({0x59, 0x02, 0xFF, 0x01, 0x28, 0x00, 0x2F, 0xC0, 0x35, 0x00, 0x08});

    REQUIRE(codes.size() == 2);

    CHECK(codes[0].name() == "P0128");
    CHECK(codes[0].status == 0x2F);

    CHECK(codes[1].name() == "U0035");
    CHECK(codes[1].status == 0x08);
}

TEST_CASE("Anything that is not a DTC list yields no trouble codes", "[uds][dtc]")
{
    // Guessing at another sub-function's layout would invent trouble codes that
    // are not there, which is the worst thing this particular screen can do.
    CHECK(parseDtcResponse({0x62, 0xF1, 0x90, 0x01}).empty());
    CHECK(parseDtcResponse({0x59, 0x01, 0xFF}).empty());
    CHECK(parseDtcResponse({}).empty());

    // A trailing partial code is dropped rather than padded into existence.
    CHECK(parseDtcResponse({0x59, 0x02, 0xFF, 0x01, 0x28}).empty());
}

TEST_CASE("An unknown service is still shown, by number", "[uds]")
{
    // A manufacturer-specific service is still a service. A tool that hides
    // what it cannot name is worse than one that shows a number.
    const std::string text = describeService(0xBA);

    INFO(text);
    CHECK(text.find("BA") != std::string::npos);

    CHECK(describeService(0x22) == "ReadDataByIdentifier");
}
