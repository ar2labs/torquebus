// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The roles of an instrument cluster and the profiles that feed them.
//
// The cluster is meant to be on screen on a vehicle that reports little, so most of what can go
// wrong here goes wrong quietly: a role the QML reads and nobody can feed, a profile that names a
// signal the database renamed, a role with two sources. Each of those is a lamp that never lights
// or a needle that never moves, with nothing in the Output panel to say why.

#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/database/CanMessage.h"
#include "core/database/DbcParser.h"
#include "core/j1939/J1939Diagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace torquebus;

namespace {

[[nodiscard]] std::string contentsOf(const std::filesystem::path& path)
{
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

/// The `vehicle` keys ClusterView.qml reads: num("speed"), flag("lampHigh") and vehicle.ignition.
[[nodiscard]] std::set<std::string> keysTheQmlReads()
{
    const std::string qml =
        contentsOf(std::filesystem::path{TORQUEBUS_CLUSTER_QML_DIR} / "ClusterView.qml");

    std::set<std::string> keys;
    const std::regex pattern{R"re((?:num|flag)\("(\w+)"\)|vehicle\.(\w+))re"};

    for (auto it = std::sregex_iterator(qml.begin(), qml.end(), pattern);
         it != std::sregex_iterator();
         ++it) {
        keys.insert((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());
    }

    return keys;
}

[[nodiscard]] CanDatabase databaseNamed(const char* file)
{
    CanDatabase database;
    const Result result = DbcParser::parseFile(
        (std::filesystem::path{TORQUEBUS_EXAMPLE_DATABASE_DIR} / file).string(), database);

    EXPECT_TRUE(result.succeeded()) << std::string{result.message()};
    return database;
}

} // namespace

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------

TEST(ClusterRoleTests, EveryRoleIsListedOnceAndNamedOnce)
{
    const auto roles = allClusterRoles();

    // DmProtect is the last of the enum, so a role added after it and forgotten in the table is
    // the one this notices.
    EXPECT_EQ(roles.size(), static_cast<std::size_t>(ClusterRole::DmProtect) + 1);

    std::set<std::string> names;
    for (const ClusterRole role : roles) {
        EXPECT_TRUE(names.insert(std::string{nameOf(role)}).second) << nameOf(role);
    }
}

TEST(ClusterRoleTests, TheFlagsAreTheTailOfTheList)
{
    // isFlag() is a comparison against the first flag, which holds only while the numbers come
    // first. Said here so the day somebody adds a number in the middle of the flags, it fails.
    bool seenFlag = false;

    for (const ClusterRole role : allClusterRoles()) {
        if (isFlag(role)) {
            seenFlag = true;
        } else {
            EXPECT_FALSE(seenFlag) << nameOf(role) << " is a number listed after a flag";
        }
    }

    EXPECT_TRUE(isFlag(ClusterRole::Ignition));
    EXPECT_FALSE(isFlag(ClusterRole::AiHealth));
}

TEST(ClusterRoleTests, TheQmlClusterReadsExactlyTheRoles)
{
    // One list in two places. A role the QML does not read is a source that feeds nothing; a key
    // the QML reads that is not a role is an instrument nobody can ever feed, which looks the same
    // on screen as a vehicle that has no data for it.
    std::set<std::string> roles;
    for (const ClusterRole role : allClusterRoles()) {
        roles.insert(std::string{nameOf(role)});
    }

    const std::set<std::string> read = keysTheQmlReads();

    ASSERT_FALSE(read.empty()) << "ClusterView.qml was not found or has no keys";

    for (const std::string& key : read) {
        EXPECT_TRUE(roles.contains(key)) << "ClusterView.qml reads '" << key << "', not a role";
    }
    for (const std::string& role : roles) {
        EXPECT_TRUE(read.contains(role))
            << "ClusterView.qml does not read the role '" << role << "'";
    }
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

TEST(ClusterProfilesTests, TheDefaultProfileIsRegisteredWithoutAnyoneAskingFor)
{
    // A project is opened before anything else has run, and a cluster naming this profile must not
    // be refused for it.
    const ClusterProfile* profile = ClusterProfiles::instance().find(kDefaultClusterProfile);

    ASSERT_TRUE(profile != nullptr);
    EXPECT_FALSE(profile->name.empty());
    EXPECT_TRUE(ClusterProfiles::instance().find("no-such-profile") == nullptr);
    EXPECT_TRUE(ClusterProfiles::instance().find("") == nullptr);
}

TEST(ClusterProfilesTests, ARoleHasOneSourceAndASourceIsComplete)
{
    for (const ClusterProfile& profile : ClusterProfiles::instance().all()) {
        SCOPED_TRACE(profile.id);

        std::set<ClusterRole> seen;
        for (const ClusterSource& source : profile.sources) {
            EXPECT_TRUE(seen.insert(source.role).second)
                << "two sources for '" << nameOf(source.role) << "'";

            switch (source.binding.source) {
            case DashboardBinding::Source::Signal:
                EXPECT_FALSE(source.binding.message.empty());
                EXPECT_FALSE(source.binding.signal.empty());
                break;
            case DashboardBinding::Source::Variable:
                EXPECT_FALSE(source.binding.variable.empty());
                break;
            case DashboardBinding::Source::None:
                ADD_FAILURE() << "'" << nameOf(source.role) << "' has a source bound to nothing";
                break;
            }
        }
    }
}

TEST(ClusterProfilesTests, TheExampleProfileReadsSignalsTheExampleDatabasesHave)
{
    // The profile is a list of names typed by hand, and the databases are files other people edit.
    // This is what stops a renamed signal from becoming a needle that never moves.
    const CanDatabase vehicle = databaseNamed("vehicle.dbc");
    const CanDatabase ecu = databaseNamed("ecu.dbc");

    const ClusterProfile* profile = ClusterProfiles::instance().find(kDefaultClusterProfile);
    ASSERT_TRUE(profile != nullptr);
    ASSERT_FALSE(profile->sources.empty());

    for (const ClusterSource& source : profile->sources) {
        SCOPED_TRACE(std::string{nameOf(source.role)});
        ASSERT_TRUE(source.binding.source == DashboardBinding::Source::Signal);

        const CanMessage* message = vehicle.findByName(source.binding.message);
        if (message == nullptr) {
            message = ecu.findByName(source.binding.message);
        }

        ASSERT_TRUE(message != nullptr) << "no message '" << source.binding.message << "'";
        EXPECT_TRUE(message->findSignal(source.binding.signal) != nullptr)
            << "'" << source.binding.message << "' has no signal '" << source.binding.signal << "'";
    }
}

TEST(ClusterProfilesTests, EveryBusSignalOfTheExampleProfileHasAnAgeLongerThanItsCycle)
{
    // A signal that is not seen for this long is no data. Too short and a signal that is merely
    // slow flickers to dashes between its samples; zero and it never goes at all. The example's
    // ECU sends EngineTemp once a second (docs/development/cluster.md), and the rest faster.
    const ClusterProfile* profile = ClusterProfiles::instance().find(kDefaultClusterProfile);
    ASSERT_TRUE(profile != nullptr);

    for (const ClusterSource& source : profile->sources) {
        if (source.binding.source != DashboardBinding::Source::Signal) {
            continue; // a variable has no age at all
        }

        SCOPED_TRACE(std::string{nameOf(source.role)});
        EXPECT_GT(source.maxAgeMs, 0U);
    }

    const ClusterSource* coolant = profile->sourceOf(ClusterRole::Coolant);
    ASSERT_TRUE(coolant != nullptr);
    EXPECT_GE(coolant->maxAgeMs, 2U * 1000U);
}

TEST(ClusterProfilesTests, TheExampleProfileCoversWhatTheExampleCanShowAndNothingItCannot)
{
    // The roles with a source today. The others are dashes on purpose: what the example vehicle
    // does not send is for the example to grow, not for the profile to cover with a signal it does
    // not have. A source naming a signal that is not in the databases is what the test above finds.
    const ClusterProfile* profile = ClusterProfiles::instance().find(kDefaultClusterProfile);
    ASSERT_TRUE(profile != nullptr);

    for (const ClusterRole role : {ClusterRole::Speed,
                                   ClusterRole::Coolant,
                                   ClusterRole::Rpm,
                                   ClusterRole::AiRegime,
                                   ClusterRole::AiAnomaly,
                                   ClusterRole::AiConfidence,
                                   ClusterRole::AiHealth}) {
        EXPECT_TRUE(profile->sourceOf(role) != nullptr) << nameOf(role);
    }

    for (const ClusterRole role :
         {ClusterRole::Gear, ClusterRole::Fuel, ClusterRole::LampHigh, ClusterRole::DmMil}) {
        EXPECT_TRUE(profile->sourceOf(role) == nullptr) << nameOf(role);
    }
}

TEST(ClusterProfilesTests, RegisteringAnIdTwiceReplacesTheFirst)
{
    // Same rule as the backend registry, so a plugin can shadow a built-in profile while it is
    // being developed. A private id keeps the process-wide registry as it was for the other tests.
    ClusterProfiles& registry = ClusterProfiles::instance();
    const std::size_t before = registry.all().size();

    ClusterProfile profile;
    profile.id = "test-only-replaced";
    profile.name = "First";
    registry.registerProfile(profile);

    profile.name = "Second";
    registry.registerProfile(profile);

    EXPECT_EQ(registry.all().size(), before + 1);

    const ClusterProfile* found = registry.find("test-only-replaced");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->name, "Second");
}

TEST(ClusterProfilesTests, TheJ1939CommercialProfileReadsSignalsTheJ1939DatabaseHas)
{
    const CanDatabase j1939 = databaseNamed("j1939.dbc");

    const ClusterProfile* profile = ClusterProfiles::instance().find(kJ1939CommercialProfile);
    ASSERT_TRUE(profile != nullptr);
    ASSERT_FALSE(profile->sources.empty());

    for (const ClusterSource& source : profile->sources) {
        SCOPED_TRACE(std::string{nameOf(source.role)});

        if (source.binding.source == DashboardBinding::Source::Variable) {
            continue; // checked by TheDm1LampsOfTheJ1939ProfileAreTheVariablesTheJ1939BlockWrites
        }

        ASSERT_TRUE(source.binding.source == DashboardBinding::Source::Signal);

        const CanMessage* message = j1939.findByName(source.binding.message);
        ASSERT_TRUE(message != nullptr) << "no message '" << source.binding.message << "'";
        EXPECT_TRUE(message->findSignal(source.binding.signal) != nullptr)
            << "'" << source.binding.message << "' has no signal '" << source.binding.signal << "'";
    }
}

TEST(ClusterProfilesTests, TheDm1LampsOfTheJ1939ProfileAreTheVariablesTheJ1939BlockWrites)
{
    // Every ECU sends its own DM1 and a database names a message once, so the lamps cannot be read
    // as signals: five ECUs with nothing to report would hide the sixth's red lamp for all but a
    // few milliseconds of every second. The J1939 block writes the OR of them to four variables,
    // and the profile has to read exactly those - a typo here is a lamp that never lights, and
    // nothing says so, because a variable nobody writes simply reads as zero.
    const ClusterProfile* profile = ClusterProfiles::instance().find(kJ1939CommercialProfile);
    ASSERT_TRUE(profile != nullptr);

    const std::pair<ClusterRole, std::string_view> expected[] = {
        {ClusterRole::DmStop, kJ1939LampStopVariable},
        {ClusterRole::DmWarn, kJ1939LampWarningVariable},
        {ClusterRole::DmMil, kJ1939LampMilVariable},
        {ClusterRole::DmProtect, kJ1939LampProtectVariable},
    };

    for (const auto& [role, variable] : expected) {
        SCOPED_TRACE(std::string{nameOf(role)});

        const ClusterSource* source = profile->sourceOf(role);
        ASSERT_TRUE(source != nullptr);
        EXPECT_TRUE(source->binding.source == DashboardBinding::Source::Variable);
        EXPECT_EQ(source->binding.variable, std::string{variable});
    }

    // The engine lamp is the malfunction indicator, which is dmMil. A second source for the same
    // lamp from the per-frame DM1 signal would bring the flapping back through the side door.
    EXPECT_TRUE(profile->sourceOf(ClusterRole::LampEngine) == nullptr);
}

TEST(ClusterProfilesTests, TheJ1939ProfileFeedsTheAiPanelItsRegimeAndNotItsRiskLevel)
{
    // They are both small numbers and they are not the same thing: the cluster names regime 0 Idle
    // and regime 1 Cruise. Fed the risk level it said "engine idling" of a vehicle at 100 km/h
    // whenever the model was calm, and could never show the Anomaly alarm.
    const ClusterProfile* profile = ClusterProfiles::instance().find(kJ1939CommercialProfile);
    ASSERT_TRUE(profile != nullptr);

    const ClusterSource* regime = profile->sourceOf(ClusterRole::AiRegime);
    ASSERT_TRUE(regime != nullptr);
    EXPECT_EQ(regime->binding.message, "TinyML_Proprietary");
    EXPECT_EQ(regime->binding.signal, "RegimeClass");
}

TEST(ClusterProfilesTests, AProfileNeverHasTwoSourcesForOneRole)
{
    // sourceOf() answers with the first, so a second would be a source that is read by nobody and
    // looks as though it is read.
    for (const std::string_view id : {kDefaultClusterProfile, kJ1939CommercialProfile}) {
        const ClusterProfile* profile = ClusterProfiles::instance().find(id);
        ASSERT_TRUE(profile != nullptr);

        std::set<ClusterRole> seen;
        for (const ClusterSource& source : profile->sources) {
            SCOPED_TRACE(std::string{id} + ": " + std::string{nameOf(source.role)});
            EXPECT_TRUE(seen.insert(source.role).second);
        }
    }
}

TEST(ClusterProfilesTests, EveryBusSignalOfTheJ1939CommercialProfileHasValidAge)
{
    const ClusterProfile* profile = ClusterProfiles::instance().find(kJ1939CommercialProfile);
    ASSERT_TRUE(profile != nullptr);

    for (const ClusterSource& source : profile->sources) {
        SCOPED_TRACE(std::string{nameOf(source.role)});
        EXPECT_GT(source.maxAgeMs, 0U);
    }

    const ClusterSource* speed = profile->sourceOf(ClusterRole::Speed);
    ASSERT_TRUE(speed != nullptr);
    EXPECT_EQ(speed->maxAgeMs, 300U);

    const ClusterSource* rpm = profile->sourceOf(ClusterRole::Rpm);
    ASSERT_TRUE(rpm != nullptr);
    EXPECT_EQ(rpm->maxAgeMs, 100U);
}
