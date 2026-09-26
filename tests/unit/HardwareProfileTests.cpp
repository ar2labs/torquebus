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

#include <gtest/gtest.h>

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
        EXPECT_TRUE(m_directory.isValid());
        m_store = std::make_unique<SettingsStore>(
            QDir{m_directory.path()}.filePath(QStringLiteral("settings.json")));
        EXPECT_TRUE(m_store->load());
    }

    [[nodiscard]] SettingsStore& store() const noexcept { return *m_store; }

private:
    QTemporaryDir m_directory;
    std::unique_ptr<SettingsStore> m_store;
};

} // namespace

TEST(HardwareProfileTests, AnInterfaceNobodyHasConfiguredIsUsedAtTheDefaultRate)
{
    // The defaults are what the application did before this class existed:
    // every detected interface bound, at 250 kbit/s. Somebody who has just
    // plugged in their first adapter should not have to configure it to see
    // frames.
    const ScopedSettings settings;
    const HardwareProfile profile{settings.store()};

    const ChannelPreferences preferences = profile.preferencesFor(QStringLiteral("peak:usb0"));

    EXPECT_TRUE(preferences.enabled);
    EXPECT_TRUE(preferences.bitrate == torquebus::kDefaultBitrate);
    EXPECT_FALSE(preferences.canFd);
    EXPECT_FALSE(preferences.listenOnly);
    EXPECT_TRUE(preferences.receiveErrorFrames);
}

TEST(HardwareProfileTests, SettingsFollowTheAdapterNotTheSlot)
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
    ASSERT_TRUE(reopened.load());

    const HardwareProfile reloaded{reopened};
    const ChannelPreferences stored = reloaded.preferencesFor(QStringLiteral("kvaser:1"));

    EXPECT_TRUE(stored.bitrate == 500'000);
    EXPECT_TRUE(stored.listenOnly);
    EXPECT_FALSE(stored.receiveErrorFrames);

    // And the other adapter is untouched.
    EXPECT_TRUE(reloaded.preferencesFor(QStringLiteral("kvaser:0")).bitrate
                == torquebus::kDefaultBitrate);
}

TEST(HardwareProfileTests, BitRateSwitchingWithoutFDIsNotStoredAsASetting)
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

    EXPECT_FALSE(profile.preferencesFor(QStringLiteral("peak:usb0")).bitRateSwitch);
}

TEST(HardwareProfileTests, ARateNoBackendHasTimingForFallsBackToTheDefault)
{
    // The settings file is JSON so that people can edit it, which makes a
    // number in it input rather than data. A rate nothing can be opened at
    // would produce a channel that emits error frames instead of failing.
    const ScopedSettings settings;
    settings.store().setIntValue(torquebus::services::bitrateKey(QStringLiteral("peak:usb0")), 137);

    const HardwareProfile profile{settings.store()};

    EXPECT_TRUE(profile.preferencesFor(QStringLiteral("peak:usb0")).bitrate
                == torquebus::kDefaultBitrate);
}

TEST(HardwareProfileTests, TheSavedOrderDecidesWhichInterfaceIsCAN1)
{
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    const QStringList stored{QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")};

    EXPECT_TRUE(HardwareProfile::arrange(detected, stored) == stored);
}

TEST(HardwareProfileTests, AnAdapterLeftAtTheOfficeDoesNotLeaveAGap)
{
    // The saved order names three; two are present. The absent one is simply
    // not there - it must not hold CAN 2 open for itself and push the others
    // down a number.
    const QStringList detected{QStringLiteral("a"), QStringLiteral("c")};
    const QStringList stored{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    ASSERT_TRUE(arranged.size() == 2);
    EXPECT_TRUE(arranged.first() == QStringLiteral("c"));
    EXPECT_TRUE(arranged.last() == QStringLiteral("a"));
}

TEST(HardwareProfileTests, ANewlyPluggedAdapterBecomesTheLastChannel)
{
    // Rather than being inserted wherever the driver enumerated it. Renumbering
    // the channels somebody has been reading all morning - and that their
    // project's transmit rows point at - is the sort of surprise that makes a
    // tool untrustworthy.
    const QStringList detected{QStringLiteral("new"), QStringLiteral("a"), QStringLiteral("b")};
    const QStringList stored{QStringLiteral("a"), QStringLiteral("b")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    ASSERT_TRUE(arranged.size() == 3);
    EXPECT_TRUE(arranged.at(0) == QStringLiteral("a"));
    EXPECT_TRUE(arranged.at(1) == QStringLiteral("b"));
    EXPECT_TRUE(arranged.at(2) == QStringLiteral("new"));
}

TEST(HardwareProfileTests, NoSavedOrderMeansEnumerationOrder)
{
    // What a machine nobody has arranged gets, which is what every machine got
    // before this existed.
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b")};

    EXPECT_TRUE(HardwareProfile::arrange(detected, {}) == detected);
}

TEST(HardwareProfileTests, ADuplicatedHandleInAHandEditedOrderIsBoundOnce)
{
    const QStringList detected{QStringLiteral("a"), QStringLiteral("b")};
    const QStringList stored{QStringLiteral("a"), QStringLiteral("a"), QStringLiteral("b")};

    const QStringList arranged = HardwareProfile::arrange(detected, stored);

    ASSERT_TRUE(arranged.size() == 2);
    EXPECT_TRUE(arranged.first() == QStringLiteral("a"));
}

TEST(HardwareProfileTests, TheOrderSurvivesARestart)
{
    const ScopedSettings settings;

    {
        HardwareProfile profile{settings.store()};
        profile.setOrder({QStringLiteral("peak:usb0"), QStringLiteral("kvaser:0")});
    }

    SettingsStore reopened{settings.store().filePath()};
    ASSERT_TRUE(reopened.load());

    const HardwareProfile profile{reopened};

    ASSERT_TRUE(profile.order().size() == 2);
    EXPECT_TRUE(profile.order().first() == QStringLiteral("peak:usb0"));
}
