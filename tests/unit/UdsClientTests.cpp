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

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

constexpr std::uint64_t kMillisecond = 1'000'000ULL;

using Bytes = std::vector<std::uint8_t>;

} // namespace

TEST(UdsClientTests, APositiveAnswerIsTheServicePlus0x40)
{
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    ASSERT_TRUE(client.pendingRequests().size() == 1);
    EXPECT_TRUE((client.pendingRequests().front() == Bytes{0x22, 0xF1, 0x90}));
    EXPECT_TRUE(client.isBusy());

    EXPECT_TRUE(client.onMessage(Bytes{0x62, 0xF1, 0x90, 'W', 'V', 'W'}, 12 * kMillisecond));

    ASSERT_TRUE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    EXPECT_TRUE(exchange.outcome == UdsExchange::Outcome::Positive);
    EXPECT_TRUE(exchange.elapsedNs == 12 * kMillisecond);
    EXPECT_FALSE(client.isBusy());
}

TEST(UdsClientTests, ReadingTheVINIsNotARequestForSilence)
{
    // 0x22 0xF1 0x90 has bit 7 set in its second byte, because 0xF1 is half of
    // an identifier. A client that reads that bit as the suppress-positive-
    // response flag - without first asking whether this service even has a
    // sub-function - sends the commonest request in the protocol and then never
    // waits for its answer. This is that check.
    EXPECT_FALSE(suppressesResponse(readDataByIdentifier(0xF190)));
    EXPECT_FALSE(hasSubFunction(0x22));

    EXPECT_TRUE(suppressesResponse(testerPresent(true)));
    EXPECT_TRUE(hasSubFunction(0x3E));

    UdsClient client;
    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    EXPECT_TRUE(client.isBusy());
}

TEST(UdsClientTests, ASuppressedRequestIsNotWaitedFor)
{
    UdsClient client;

    ASSERT_TRUE(client.request(testerPresent(true), 0).succeeded());

    EXPECT_TRUE((client.pendingRequests().front() == Bytes{0x3E, 0x80}));
    EXPECT_FALSE(client.isBusy());

    // And no timeout is invented for an answer nobody asked for.
    client.poll(10'000 * kMillisecond);
    EXPECT_TRUE(client.exchanges().empty());
}

TEST(UdsClientTests, ResponsePendingIsNotAFailureItIsALongerClock)
{
    // The single most important behaviour in this file. An ECU that has to
    // erase a flash sector before answering sends 0x78 repeatedly; a tester
    // that treats it as an error gives up on every ECU worth talking to.
    UdsClient client;

    ASSERT_TRUE(client.request(Bytes{0x31, 0x01, 0xFF, 0x00}, 0).succeeded());

    // P2 would have expired here.
    EXPECT_TRUE(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 40 * kMillisecond));
    client.poll(60 * kMillisecond);

    EXPECT_TRUE(client.exchanges().empty());
    EXPECT_TRUE(client.isBusy());

    // And again, four seconds later - still inside P2*.
    EXPECT_TRUE(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 4000 * kMillisecond));
    client.poll(4500 * kMillisecond);

    EXPECT_TRUE(client.exchanges().empty());

    EXPECT_TRUE(client.onMessage(Bytes{0x71, 0x01, 0xFF, 0x00}, 6000 * kMillisecond));

    ASSERT_TRUE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    EXPECT_TRUE(exchange.outcome == UdsExchange::Outcome::Positive);

    // Both are worth showing: four seconds with nine "still working" answers is
    // a different story from four seconds of silence.
    EXPECT_TRUE(exchange.pendingCount == 2);
    EXPECT_TRUE(exchange.elapsedNs == 6000 * kMillisecond);
}

TEST(UdsClientTests, EvenAPendingECUIsGivenUpOnEventually)
{
    UdsClient client;

    ASSERT_TRUE(client.request(Bytes{0x31, 0x01, 0xFF, 0x00}, 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x7F, 0x31, 0x78}, 10 * kMillisecond));

    client.poll(5009 * kMillisecond);
    EXPECT_TRUE(client.exchanges().empty());

    client.poll(5011 * kMillisecond);

    ASSERT_TRUE(client.exchanges().size() == 1);
    EXPECT_TRUE(client.exchanges().front().outcome == UdsExchange::Outcome::Timeout);
}

TEST(UdsClientTests, AnECUThatSaysNothingAtAllTimesOutAtP2)
{
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    client.poll(49 * kMillisecond);
    EXPECT_TRUE(client.exchanges().empty());

    client.poll(50 * kMillisecond);

    ASSERT_TRUE(client.exchanges().size() == 1);
    EXPECT_TRUE(client.exchanges().front().outcome == UdsExchange::Outcome::Timeout);
    EXPECT_FALSE(client.isBusy());
}

TEST(UdsClientTests, ARefusalIsReportedWithTheReasonInWords)
{
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x7F, 0x22, 0x31}, 5 * kMillisecond));

    ASSERT_TRUE(client.exchanges().size() == 1);

    const UdsExchange& exchange = client.exchanges().front();
    EXPECT_TRUE(exchange.outcome == UdsExchange::Outcome::Negative);
    EXPECT_TRUE(exchange.negativeResponse == 0x31);

    // The whole point of the vocabulary: "7F 22 31" tells an engineer nothing
    // they could not read off the bus themselves.
    const std::string text = exchange.describe();
    SCOPED_TRACE(::testing::Message() << text);
    EXPECT_TRUE(text.find("ReadDataByIdentifier") != std::string::npos);
    EXPECT_TRUE(text.find("out of range") != std::string::npos);
}

TEST(UdsClientTests, AnAnswerToSomebodyElseSRequestIsNotOurs)
{
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());

    // A refusal naming a service we did not ask for.
    EXPECT_FALSE(client.onMessage(Bytes{0x7F, 0x19, 0x31}, kMillisecond));
    EXPECT_TRUE(client.exchanges().empty());
    EXPECT_TRUE(client.isBusy());
}

TEST(UdsClientTests, AnAnswerToTheWrongServiceIsReportedRatherThanIgnored)
{
    // On a bus where this happens - two testers, or an ECU answering late -
    // knowing that it happened is the whole diagnosis.
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x50, 0x03}, kMillisecond));

    ASSERT_TRUE(client.exchanges().size() == 1);
    EXPECT_TRUE(client.exchanges().front().outcome == UdsExchange::Outcome::Mismatch);
}

TEST(UdsClientTests, ASessionIsBelievedFromTheAnswerNotFromTheRequest)
{
    // An ECU may answer a request for the programming session with the extended
    // one. Believing the request is how a tester becomes certain of a session
    // it is not in - and then blames the next refusal on something else.
    UdsClient client;

    ASSERT_TRUE(client.request(diagnosticSessionControl(UdsSession::Programming), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, 5 * kMillisecond));

    EXPECT_TRUE(client.session() == UdsSession::Extended);
}

TEST(UdsClientTests, ANonDefaultSessionIsKeptAliveWithoutBeingAsked)
{
    // An ECU drops to the default session after S3 of silence, taking security
    // access with it. Forgetting the heartbeat produces a failure that appears
    // minutes later and somewhere else, which is why this is the client's job
    // and not the caller's.
    UdsClient client;

    ASSERT_TRUE(client.request(diagnosticSessionControl(UdsSession::Extended), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x50, 0x03}, 5 * kMillisecond));

    client.clearPendingRequests();

    client.poll(1000 * kMillisecond);
    EXPECT_TRUE(client.pendingRequests().empty());

    // Two fifths of S3.
    client.poll(2005 * kMillisecond);

    ASSERT_TRUE(client.pendingRequests().size() == 1);
    EXPECT_TRUE((client.pendingRequests().front() == Bytes{0x3E, 0x80}));
}

TEST(UdsClientTests, TheDefaultSessionNeedsNoHeartbeat)
{
    UdsClient client;

    ASSERT_TRUE(client.request(diagnosticSessionControl(UdsSession::Default), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x50, 0x01}, 5 * kMillisecond));

    client.clearPendingRequests();
    client.poll(60'000 * kMillisecond);

    EXPECT_TRUE(client.pendingRequests().empty());
}

TEST(UdsClientTests, AHeartbeatNeverTakesTheSlotARealRequestNeeds)
{
    // A keep-alive that blocked a request would be a mechanism that stops the
    // work it exists to protect.
    UdsClient client;

    ASSERT_TRUE(client.request(diagnosticSessionControl(UdsSession::Extended), 0).succeeded());
    EXPECT_TRUE(client.onMessage(Bytes{0x50, 0x03}, 0));

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 2005 * kMillisecond).succeeded());
    client.clearPendingRequests();

    client.poll(2005 * kMillisecond);

    // Busy with a real exchange: no heartbeat, and no refusal either.
    EXPECT_TRUE(client.pendingRequests().empty());
    EXPECT_TRUE(client.isBusy());
}

TEST(UdsClientTests, TwoQuestionsAtOnceAreRefused)
{
    UdsClient client;

    ASSERT_TRUE(client.request(readDataByIdentifier(0xF190), 0).succeeded());
    EXPECT_TRUE(client.request(readDataByIdentifier(0xF18C), 0).failed());
}

TEST(UdsClientTests, TheRequestBuildersPutTheBytesWhereTheStandardSays)
{
    EXPECT_TRUE((readDataByIdentifier(0xF190) == Bytes{0x22, 0xF1, 0x90}));
    EXPECT_TRUE(
        (writeDataByIdentifier(0x2001, {0x01, 0x02}) == Bytes{0x2E, 0x20, 0x01, 0x01, 0x02}));
    EXPECT_TRUE((diagnosticSessionControl(UdsSession::Extended) == Bytes{0x10, 0x03}));
    EXPECT_TRUE((ecuReset(0x01) == Bytes{0x11, 0x01}));
    EXPECT_TRUE((readDtcByStatusMask(0xFF) == Bytes{0x19, 0x02, 0xFF}));
    EXPECT_TRUE((clearDiagnosticInformation() == Bytes{0x14, 0xFF, 0xFF, 0xFF}));
    EXPECT_TRUE((securityAccessSeed(0x01) == Bytes{0x27, 0x01}));

    // The key goes back on the level *after* the seed's - sending it on the
    // seed's own level is refused as a sequence error that says nothing about
    // the cause.
    EXPECT_TRUE((securityAccessKey(0x01, {0xAA, 0xBB}) == Bytes{0x27, 0x02, 0xAA, 0xBB}));
}

TEST(UdsClientTests, TroubleCodesAreNamedTheWayAWorkshopManualNamesThem)
{
    // The raw number is nothing like the form every scan tool shows: the first
    // two bits are the system letter and the next two the leading digit.
    const std::vector<DiagnosticTroubleCode> codes =
        parseDtcResponse({0x59, 0x02, 0xFF, 0x01, 0x28, 0x00, 0x2F, 0xC0, 0x35, 0x00, 0x08});

    ASSERT_TRUE(codes.size() == 2);

    EXPECT_TRUE(codes[0].name() == "P0128");
    EXPECT_TRUE(codes[0].status == 0x2F);

    EXPECT_TRUE(codes[1].name() == "U0035");
    EXPECT_TRUE(codes[1].status == 0x08);
}

TEST(UdsClientTests, AnythingThatIsNotADTCListYieldsNoTroubleCodes)
{
    // Guessing at another sub-function's layout would invent trouble codes that
    // are not there, which is the worst thing this particular screen can do.
    EXPECT_TRUE(parseDtcResponse({0x62, 0xF1, 0x90, 0x01}).empty());
    EXPECT_TRUE(parseDtcResponse({0x59, 0x01, 0xFF}).empty());
    EXPECT_TRUE(parseDtcResponse({}).empty());

    // A trailing partial code is dropped rather than padded into existence.
    EXPECT_TRUE(parseDtcResponse({0x59, 0x02, 0xFF, 0x01, 0x28}).empty());
}

TEST(UdsClientTests, AnUnknownServiceIsStillShownByNumber)
{
    // A manufacturer-specific service is still a service. A tool that hides
    // what it cannot name is worse than one that shows a number.
    const std::string text = describeService(0xBA);

    SCOPED_TRACE(::testing::Message() << text);
    EXPECT_TRUE(text.find("BA") != std::string::npos);

    EXPECT_TRUE(describeService(0x22) == "ReadDataByIdentifier");
}
