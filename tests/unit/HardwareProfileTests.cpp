// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Which interface is CAN 1 is not cosmetic: it is what every channel column in
// a trace, every transmit row and every saved project means. Most of these
// cases are about the two ways that can go wrong - an adapter that was not
// plugged in this morning, and one that was not there yesterday.

#include "services/HardwareProfile.h"
#include "services/SettingsStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using torquebus::services::ChannelPreferences;
using torquebus::services::HardwareProfile;
using torquebus::services::SettingsStore;

namespace {

class ScopedSettings final {
public:
    ScopedSettings()
    {
        REQUIRE(m_directory.isValid());
        m_store = std::make_unique<SettingsStore>(
            QDir{m_directory.path()}.filePath(QStringLiteral("settings.json")));
        REQUIRE(m_store->load());
    }

    [[nodiscard]] SettingsStore& store() const noexcept { return *m_store; }

private:
    QTemporaryDir m_directory;
    std::unique_ptr<SettingsStore> m_store;
};

} // namespace

TEST_CASE("An interface nobody has configured is used, at the default rate",
          "[hardware]")
{
    // The defaults are what the application did before this class existed:
    // every detected interface bound, at 250 kbit/s. Somebody who has just
    // plugged in their first adapter should not have to configure it to see
    // frames.
    const ScopedSettings settings;
    const HardwareProfile profile{settings.store()};

    const ChannelPreferences preferences =
        profile.preferencesFor(QStringLiteral("peak:usb0"));

    CHECK(preferences.enabled);
    CHECK(preferences.bitrate == torquebus::kDefaultBitrate);
    CHECK_FALSE(preferences.canFd);
    CHECK_FALSE(preferences.listenOnly);
    CHECK(preferences.receiveErrorFrames);
}

TEST_CASE("Settings follow the adapter, not the slot", "[hardware]")
{
    // Stored per handle. Unplug an adapter, plug it back in second, and it is
    // still listen-only at 500 kbit/s - because that describes the bus it is
    // connected to, not the position it happens to occupy in a list.
    const ScopedSettings settings;
    HardwareProfile profile{settings.store()};

    ChannelPreferences wanted;
    wanted.bitrate = 500'000;
    wanted.listenOnly = true;
    wanted.receiveErrorFrames = false;

    profile.setPreferencesFor(QStringLiteral("kvaser:1"), wanted);

    // Read back through a second store, to prove it reached the file.
    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());

    const HardwareProfile reloaded{reopened};
    const ChannelPreferences stored = reloaded.preferencesFor(QStringLiteral("kvaser:1"));

    CHECK(stored.bitrate == 500'000);
    CHECK(stored.listenOnly);
    CHECK_FALSE(stored.receiveErrorFrames);

    // And the other adapter is untouched.
    CHECK(reloaded.preferencesFor(QStringLiteral("kvaser:0")).bitrate
          == torquebus::kDefaultBitrate);
}

TEST_CASE("Bit rate switching without FD is not stored as a setting", "[hardware]")
{
    // Not a combination a channel can be opened as. Resolved here rather than
    // being handed to a driver that would refuse it with a message about
    // something else - and the settings file is editable text, so this has to
    // hold on the way in as well as on the way out.
    const ScopedSettings settings;
    HardwareProfile profile{settings.store()};

    ChannelPreferences wanted;
    wanted.canFd = false;
    wanted.bitRateSwitch = true;

    profile.setPreferencesFor(QStringLiteral("peak:usb0"), wanted);

    CHECK_FALSE(profile.preferencesFor(QStringLiteral("peak:usb0")).bitRateSwitch);
}

TEST_CASE("A rate no backend has timing for falls back to the default",
          "[hardware]")
{
    // The settings file is JSON so that people can edit it, which makes a
    // number in it input rather than data. A rate nothing can be opened at
    // would produce a channel that emits error frames instead of failing.
    const ScopedSettings settings;
    settings.store().setIntValue(
        torquebus::services::bitrateKey(QStringLiteral("peak:usb0")), 137);

    const HardwareProfile profile{settings.store()};

    CHECK(profile.preferencesFor(QStringLiteral("peak:usb0")).bitrate
          == torquebus::kDefaultBitrate);
}

TEST_CASE("The saved order decides which interface is CAN 1", "[hardware]")
{
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    const QStringList stored{QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")};

    CHECK(HardwareProfile::arrange(detected, stored) == stored);
}

TEST_CASE("An adapter left at the office does not leave a gap", "[hardware]")
{
    // The saved order names three; two are present. The absent one is simply
    // not there - it must not hold CAN 2 open for itself and push the others
    // down a number.
    const QStringList detected{QStringLiteral("a"), QStringLiteral("c")};
    const QStringList stored{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    REQUIRE(arranged.size() == 2);
    CHECK(arranged.first() == QStringLiteral("c"));
    CHECK(arranged.last() == QStringLiteral("a"));
}

TEST_CASE("A newly plugged adapter becomes the last channel", "[hardware]")
{
    // Rather than being inserted wherever the driver enumerated it. Renumbering
    // the channels somebody has been reading all morning - and that their
    // project's transmit rows point at - is the sort of surprise that makes a
    // tool untrustworthy.
    const QStringList detected{QStringLiteral("new"), QStringLiteral("a"),
                               QStringLiteral("b")};
    const QStringList stored{QStringLiteral("a"), QStringLiteral("b")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    REQUIRE(arranged.size() == 3);
    CHECK(arranged.at(0) == QStringLiteral("a"));
    CHECK(arranged.at(1) == QStringLiteral("b"));
    CHECK(arranged.at(2) == QStringLiteral("new"));
}

TEST_CASE("No saved order means enumeration order", "[hardware]")
{
    // What a machine nobody has arranged gets, which is what every machine got
    // before this existed.
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b")};

    CHECK(HardwareProfile::arrange(detected, {}) == detected);
}

TEST_CASE("A duplicated handle in a hand-edited order is bound once",
          "[hardware]")
{
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b")};
    const QStringList stored{QStringLiteral("a"), QStringLiteral("a"),
                             QStringLiteral("b")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    REQUIRE(arranged.size() == 2);
    CHECK(arranged.first() == QStringLiteral("a"));
}

TEST_CASE("The order survives a restart", "[hardware]")
{
    const ScopedSettings settings;

    {
        HardwareProfile profile{settings.store()};
        profile.setOrder({QStringLiteral("peak:usb0"), QStringLiteral("kvaser:0")});
    }

    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());

    const HardwareProfile profile{reopened};

    REQUIRE(profile.order().size() == 2);
    CHECK(profile.order().first() == QStringLiteral("peak:usb0"));
}
