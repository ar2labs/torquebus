// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A simulated ECU is only worth having if it can say no. An ECU that answers
// everything positively is a mirror, not a test target - it lets a tester pass
// that would fail on the bench the moment it met a locked session or a DID the
// ECU does not have. So most of these cases are refusals.

#include "core/diagnostics/UdsServer.h"
#include "core/diagnostics/UdsTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

using namespace torquebus;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kMillisecond = 1'000'000ULL;

/// An ECU with a part number, a VIN, a writeable calibration behind security,
/// and one stored fault.
[[nodiscard]] UdsServer engineEcu()
{
    UdsServer ecu;

    ecu.setIdentifier(
        0xF190, UdsIdentifier{{'W', 'V', 'W', 'Z', 'Z', 'Z'}, false, UdsSession::Default, false});

    ecu.setIdentifier(0xF187,
                      UdsIdentifier{{'0', '4', 'E', '9'}, false, UdsSession::Default, false});

    // A calibration: writeable, extended session, and locked.
    ecu.setIdentifier(0x2001, UdsIdentifier{{0x00, 0x64}, true, UdsSession::Extended, true});

    ecu.addTroubleCode(0x012800, 0x2F);

    return ecu;
}

[[nodiscard]] Bytes ask(UdsServer& ecu, const Bytes& request, std::uint64_t nowNs = 0)
{
    const std::optional<Bytes> response = ecu.handle(request, nowNs);
    return response.value_or(Bytes{});
}

} // namespace

TEST(UdsServerTests, AnIdentifierTheECUHasIsAnsweredWithItsValue)
{
    UdsServer ecu = engineEcu();

    EXPECT_TRUE(
        (ask(ecu, {0x22, 0xF1, 0x90}) == Bytes{0x62, 0xF1, 0x90, 'W', 'V', 'W', 'Z', 'Z', 'Z'}));
}

TEST(UdsServerTests, AnIdentifierTheECUDoesNotHaveIsRefusedNotIgnored)
{
    // The commonest answer a real ECU gives a tester that guessed - and silence
    // here would teach a tester that a missing DID looks like a dead ECU.
    UdsServer ecu = engineEcu();

    EXPECT_TRUE(
        (ask(ecu, {0x22, 0xFF, 0xFF})
         == Bytes{0x7F, 0x22, static_cast<std::uint8_t>(UdsNegativeResponse::RequestOutOfRange)}));
}

TEST(UdsServerTests, ADIDThatNeedsTheExtendedSessionSaysSoInTheDefaultOne)
{
    UdsServer ecu = engineEcu();

    EXPECT_TRUE((ask(ecu, {0x22, 0x20, 0x01})
                 == Bytes{0x7F,
                          0x22,
                          static_cast<std::uint8_t>(
                              UdsNegativeResponse::ServiceNotSupportedInActiveSession)}));

    // In the extended session it is the security that is missing, which is a
    // different refusal and has to be a different code - a tester works through
    // these in order.
    ASSERT_FALSE(ask(ecu, {0x10, 0x03}).empty());

    EXPECT_TRUE((ask(ecu, {0x22, 0x20, 0x01})
                 == Bytes{0x7F,
                          0x22,
                          static_cast<std::uint8_t>(UdsNegativeResponse::SecurityAccessDenied)}));
}

TEST(UdsServerTests, AReadOnlyIdentifierRefusesAWrite)
{
    // Rather than accepting it and changing nothing, which is what makes a
    // tester believe it has written a part number.
    UdsServer ecu = engineEcu();

    EXPECT_TRUE(
        (ask(ecu, {0x2E, 0xF1, 0x87, '9', '9'})
         == Bytes{0x7F, 0x2E, static_cast<std::uint8_t>(UdsNegativeResponse::RequestOutOfRange)}));

    EXPECT_TRUE((ask(ecu, {0x22, 0xF1, 0x87}) == Bytes{0x62, 0xF1, 0x87, '0', '4', 'E', '9'}));
}

TEST(UdsServerTests, SeedAndKeyUnlockTheECUAndAWrongKeyDoesNot)
{
    UdsServer ecu = engineEcu();

    // The algorithm a script would supply: the seed with every byte inverted,
    // which is not security and is exactly what a simulation needs.
    ecu.setSecurityAlgorithm([](std::span<const std::uint8_t> seed) {
        std::vector<std::uint8_t> key;
        for (const std::uint8_t byte : seed) {
            key.push_back(static_cast<std::uint8_t>(~byte));
        }
        return key;
    });

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}).empty());

    const Bytes seed = ask(ecu, {0x27, 0x01});
    ASSERT_TRUE(seed.size() > 2);
    EXPECT_TRUE(seed[0] == 0x67);

    // A wrong key is refused and does not unlock.
    EXPECT_TRUE((ask(ecu, {0x27, 0x02, 0x00, 0x00, 0x00, 0x00})
                 == Bytes{0x7F, 0x27, static_cast<std::uint8_t>(UdsNegativeResponse::InvalidKey)}));
    EXPECT_FALSE(ecu.isUnlocked());

    // And a fresh seed, then the right key.
    const Bytes again = ask(ecu, {0x27, 0x01});

    Bytes key{0x27, 0x02};
    for (std::size_t index = 2; index < again.size(); ++index) {
        key.push_back(static_cast<std::uint8_t>(~again[index]));
    }

    EXPECT_TRUE((ask(ecu, key) == Bytes{0x67, 0x02}));
    EXPECT_TRUE(ecu.isUnlocked());

    // Now the calibration can be read and written.
    EXPECT_TRUE((ask(ecu, {0x22, 0x20, 0x01}) == Bytes{0x62, 0x20, 0x01, 0x00, 0x64}));
    EXPECT_TRUE((ask(ecu, {0x2E, 0x20, 0x01, 0x00, 0xC8}) == Bytes{0x6E, 0x20, 0x01}));
    EXPECT_TRUE((ask(ecu, {0x22, 0x20, 0x01}) == Bytes{0x62, 0x20, 0x01, 0x00, 0xC8}));
}

TEST(UdsServerTests, AKeyWithNoSeedBehindItIsASequenceError)
{
    UdsServer ecu = engineEcu();
    ecu.setSecurityAlgorithm([](std::span<const std::uint8_t>) { return Bytes{0x01}; });

    EXPECT_TRUE((ask(ecu, {0x27, 0x02, 0x01})
                 == Bytes{0x7F,
                          0x27,
                          static_cast<std::uint8_t>(UdsNegativeResponse::RequestSequenceError)}));
}

TEST(UdsServerTests, AnECUWithNoAlgorithmIsLockedRatherThanOpen)
{
    // Answering a seed no key can match would leave a tester trying for ever.
    UdsServer ecu = engineEcu();

    EXPECT_TRUE((ask(ecu, {0x27, 0x01})
                 == Bytes{0x7F,
                          0x27,
                          static_cast<std::uint8_t>(UdsNegativeResponse::ConditionsNotCorrect)}));
}

TEST(UdsServerTests, TheSessionExpiresAfterFiveSecondsOfSilence)
{
    // The rule that makes a tester which forgets its heartbeat fail here
    // instead of in a vehicle.
    UdsServer ecu = engineEcu();

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}, 0).empty());
    EXPECT_TRUE(ecu.session() == UdsSession::Extended);

    ecu.poll(4999 * kMillisecond);
    EXPECT_TRUE(ecu.session() == UdsSession::Extended);

    ecu.poll(5000 * kMillisecond);
    EXPECT_TRUE(ecu.session() == UdsSession::Default);
    EXPECT_FALSE(ecu.isUnlocked());
}

TEST(UdsServerTests, AnyRequestHoldsTheSessionOpenNotOnlyTesterPresent)
{
    // S3 is a silence timer, not a heartbeat counter - which is what the
    // standard says and what a real ECU does.
    UdsServer ecu = engineEcu();

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}, 0).empty());

    static_cast<void>(ask(ecu, {0x22, 0xF1, 0x90}, 4000 * kMillisecond));

    ecu.poll(6000 * kMillisecond);
    EXPECT_TRUE(ecu.session() == UdsSession::Extended);

    ecu.poll(9001 * kMillisecond);
    EXPECT_TRUE(ecu.session() == UdsSession::Default);
}

TEST(UdsServerTests, ASuppressedTesterPresentIsHeardAndNotAnswered)
{
    UdsServer ecu = engineEcu();

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}, 0).empty());

    EXPECT_FALSE(ecu.handle(Bytes{0x3E, 0x80}, 4000 * kMillisecond).has_value());

    ecu.poll(8000 * kMillisecond);
    EXPECT_TRUE(ecu.session() == UdsSession::Extended);
}

TEST(UdsServerTests, LeavingASessionLocksTheECUAgain)
{
    // Security is granted inside a session and does not survive it. The rule a
    // tester most often gets wrong, and the one that makes unlock-then-reset
    // fail.
    UdsServer ecu = engineEcu();
    ecu.setSecurityAlgorithm([](std::span<const std::uint8_t> seed) {
        return std::vector<std::uint8_t>{seed.begin(), seed.end()};
    });

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}).empty());

    const Bytes seed = ask(ecu, {0x27, 0x01});
    Bytes key{0x27, 0x02};
    key.insert(key.end(), seed.begin() + 2, seed.end());

    ASSERT_TRUE((ask(ecu, key) == Bytes{0x67, 0x02}));
    ASSERT_TRUE(ecu.isUnlocked());

    ASSERT_FALSE(ask(ecu, {0x10, 0x01}).empty());
    EXPECT_FALSE(ecu.isUnlocked());
}

TEST(UdsServerTests, StoredFaultsAreReportedAndCleared)
{
    UdsServer ecu = engineEcu();

    // C0 35 00: the *first two* bytes are the code a workshop manual indexes,
    // and the third is the failure type. Writing 0xC00035 here - which is what
    // this test did first - puts the 35 in the failure type and asks for a
    // fault called U0000, which is a different fault and a mistake somebody
    // will make against a real ECU too.
    ecu.addTroubleCode(0xC03500, 0x08);

    const Bytes report = ask(ecu, {0x19, 0x02, 0xFF});

    ASSERT_TRUE(report.size() == 3 + 4 * 2);
    EXPECT_TRUE(report[0] == 0x59);
    EXPECT_TRUE(report[1] == 0x02);

    const std::vector<DiagnosticTroubleCode> codes = parseDtcResponse(report);

    ASSERT_TRUE(codes.size() == 2);
    EXPECT_TRUE(codes[0].name() == "P0128");
    EXPECT_TRUE(codes[1].name() == "U0035");

    // A mask that matches nothing gives an empty list rather than everything.
    const std::vector<DiagnosticTroubleCode> none = parseDtcResponse(ask(ecu, {0x19, 0x02, 0x10}));
    EXPECT_TRUE(none.empty());

    EXPECT_TRUE(ask(ecu, {0x14, 0xFF, 0xFF, 0xFF}) == Bytes{0x54});
    EXPECT_TRUE(parseDtcResponse(ask(ecu, {0x19, 0x02, 0xFF})).empty());
}

TEST(UdsServerTests, SettingAFaultTwiceUpdatesItRatherThanStoringItTwice)
{
    // A script calling this from a timer would otherwise fill memory.
    UdsServer ecu;

    ecu.addTroubleCode(0x012800, 0x08);
    ecu.addTroubleCode(0x012800, 0x2F);

    ASSERT_TRUE(ecu.troubleCodes().size() == 1);
    EXPECT_TRUE(ecu.troubleCodes().front().status == 0x2F);
}

TEST(UdsServerTests, AResetAnswersFirstAndKeepsTheStoredFaults)
{
    // Answered then reset, which is the order a real ECU does it in. And
    // stored means stored: a reset that lost the faults would make the
    // simulation useless for the workflow it exists for.
    UdsServer ecu = engineEcu();

    ASSERT_FALSE(ask(ecu, {0x10, 0x03}).empty());

    EXPECT_TRUE((ask(ecu, {0x11, 0x01}) == Bytes{0x51, 0x01}));
    EXPECT_TRUE(ecu.session() == UdsSession::Default);
    EXPECT_TRUE(ecu.troubleCodes().size() == 1);
}

TEST(UdsServerTests, AServiceTheECUDoesNotImplementIsRefusedNotMetWithSilence)
{
    // A tester cannot tell silence from a broken wire.
    UdsServer ecu = engineEcu();

    EXPECT_TRUE((
        ask(ecu, {0x31, 0x01, 0x02, 0x03})
        == Bytes{0x7F, 0x31, static_cast<std::uint8_t>(UdsNegativeResponse::ServiceNotSupported)}));
}

TEST(UdsServerTests, TheScriptGetsFirstRefusalOnEverything)
{
    UdsServer ecu = engineEcu();

    ecu.setHandler([](std::span<const std::uint8_t> request, std::vector<std::uint8_t>& response) {
        // A routine this ECU does implement, which the server knows nothing
        // about - the whole reason the hook is there.
        if (!request.empty() && request[0] == 0x31) {
            response = {0x71, 0x01, 0x02, 0x03};
            return UdsServer::Verdict::Answered;
        }

        return UdsServer::Verdict::NotHandled;
    });

    EXPECT_TRUE((ask(ecu, {0x31, 0x01, 0x02, 0x03}) == Bytes{0x71, 0x01, 0x02, 0x03}));

    // And everything it does not claim still works.
    EXPECT_TRUE(
        (ask(ecu, {0x22, 0xF1, 0x90}) == Bytes{0x62, 0xF1, 0x90, 'W', 'V', 'W', 'Z', 'Z', 'Z'}));
}

TEST(UdsServerTests, AScriptCanMakeTheECUGoQuiet)
{
    // A dead ECU, a busy one, a wire that fell off: the case a tester has to
    // survive and the one nothing else can simulate.
    UdsServer ecu = engineEcu();

    ecu.setHandler([](std::span<const std::uint8_t>, std::vector<std::uint8_t>&) {
        return UdsServer::Verdict::Silent;
    });

    EXPECT_FALSE(ecu.handle(Bytes{0x22, 0xF1, 0x90}, 0).has_value());
}

TEST(UdsServerTests, ASessionChangeReportsTheTimingsItPromises)
{
    // 50 ms and 5000 ms, in the units the message uses - a tester reads its P2
    // from here rather than assuming.
    UdsServer ecu = engineEcu();

    EXPECT_TRUE((ask(ecu, {0x10, 0x03}) == Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
}
