// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/SettingsStore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>

using torquebus::services::SettingsStore;

namespace {

/// Every test writes into its own temporary directory, so the suite never
/// touches the developer's real settings file.
class ScopedSettingsFile final {
public:
    ScopedSettingsFile() { EXPECT_TRUE(m_directory.isValid()); }

    [[nodiscard]] QString path() const
    {
        return QDir{m_directory.path()}.filePath(QStringLiteral("settings.json"));
    }

private:
    QTemporaryDir m_directory;
};

} // namespace

TEST(SettingsStoreTests, AMissingSettingsFileIsAFirstRunNotAnError)
{
    const ScopedSettingsFile file;
    SettingsStore store{file.path()};

    EXPECT_TRUE(store.load());
    EXPECT_TRUE(store.value(QStringLiteral("ui/theme"), QStringLiteral("dark")) == "dark");
}

TEST(SettingsStoreTests, ValuesSurviveASaveLoadRoundTrip)
{
    const ScopedSettingsFile file;

    {
        SettingsStore store{file.path()};
        ASSERT_TRUE(store.load());

        store.setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
        store.setBoolValue(QStringLiteral("ui/window/restoreLayout"), false);
        store.setIntValue(QStringLiteral("can/defaultBitrate"), 500'000);
        store.setBinaryValue(QStringLiteral("ui/window/geometry"),
                             QByteArray::fromRawData("\x01\x00\xFF\x7F", 4));

        ASSERT_TRUE(store.save());
    }

    SettingsStore reloaded{file.path()};
    ASSERT_TRUE(reloaded.load());

    EXPECT_TRUE(reloaded.value(QStringLiteral("ui/theme")) == "light");
    EXPECT_TRUE(reloaded.boolValue(QStringLiteral("ui/window/restoreLayout"), true) == false);
    EXPECT_TRUE(reloaded.intValue(QStringLiteral("can/defaultBitrate")) == 500'000);
    EXPECT_TRUE(reloaded.binaryValue(QStringLiteral("ui/window/geometry"))
                == QByteArray::fromRawData("\x01\x00\xFF\x7F", 4));
}

TEST(SettingsStoreTests, TheSettingsFileIsHumanReadableJSON)
{
    // The whole reason for choosing JSON over the registry: an engineer must be
    // able to read, diff and copy this file between machines.
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    ASSERT_TRUE(store.load());
    store.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
    ASSERT_TRUE(store.save());

    QFile written{file.path()};
    ASSERT_TRUE(written.open(QIODevice::ReadOnly | QIODevice::Text));

    const QString contents = QString::fromUtf8(written.readAll());
    EXPECT_TRUE(contents.contains(QStringLiteral("\"ui/theme\"")));
    EXPECT_TRUE(contents.contains(QStringLiteral("dark")));
    EXPECT_TRUE(contents.contains(QLatin1Char('\n'))); // indented, not minified
}

TEST(SettingsStoreTests, ACorruptSettingsFileDoesNotPreventStartup)
{
    const ScopedSettingsFile file;

    QFile corrupt{file.path()};
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{ this is not json");
    corrupt.close();

    SettingsStore store{file.path()};

    // load() reports the problem...
    EXPECT_FALSE(store.load());

    // ...but the store is usable, and defaults apply.
    EXPECT_TRUE(store.value(QStringLiteral("ui/theme"), QStringLiteral("dark")) == "dark");
    EXPECT_TRUE(store.save());
}

TEST(SettingsStoreTests, RemovingAKeyRemovesItFromTheFile)
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    ASSERT_TRUE(store.load());

    store.setValue(QStringLiteral("project/lastOpened"), QStringLiteral("C:/EngineTest.tbsproj"));
    EXPECT_TRUE(store.contains(QStringLiteral("project/lastOpened")));

    store.remove(QStringLiteral("project/lastOpened"));
    EXPECT_FALSE(store.contains(QStringLiteral("project/lastOpened")));

    ASSERT_TRUE(store.save());

    SettingsStore reloaded{file.path()};
    ASSERT_TRUE(reloaded.load());
    EXPECT_FALSE(reloaded.contains(QStringLiteral("project/lastOpened")));
}

TEST(SettingsStoreTests, AnEmptyBinaryValueClearsItsKeyRatherThanStoringNothing)
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    ASSERT_TRUE(store.load());

    store.setBinaryValue(QStringLiteral("ui/window/dockLayout"), QByteArray{"layout"});
    ASSERT_TRUE(store.contains(QStringLiteral("ui/window/dockLayout")));

    store.setBinaryValue(QStringLiteral("ui/window/dockLayout"), QByteArray{});
    EXPECT_FALSE(store.contains(QStringLiteral("ui/window/dockLayout")));
}

// ---------------------------------------------------------------------------
// Per-interface bitrate
// ---------------------------------------------------------------------------

TEST(SettingsStoreTests, ABitrateKeyNamesTheInterfaceItBelongsTo)
{
    // The shape is a format, not a convenience: these keys are read back by a
    // later run and are the sort of thing people hand-edit.
    EXPECT_TRUE(torquebus::services::bitrateKey(QStringLiteral("peak:usb0"))
                == QStringLiteral("can/bitrate/peak:usb0"));
    EXPECT_TRUE(torquebus::services::bitrateKey(QStringLiteral("kvaser:0"))
                == QStringLiteral("can/bitrate/kvaser:0"));
}

TEST(SettingsStoreTests, AnInterfaceNobodyConfiguredGetsTheDefault)
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    ASSERT_TRUE(store.load());

    EXPECT_TRUE(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
                == torquebus::kDefaultBitrate);
}

TEST(SettingsStoreTests, AStoredBitrateSurvivesASaveAndALoad)
{
    const ScopedSettingsFile file;

    {
        SettingsStore store{file.path()};
        ASSERT_TRUE(store.load());
        store.setIntValue(torquebus::services::bitrateKey(QStringLiteral("peak:usb0")), 500'000);
        ASSERT_TRUE(store.save());
    }

    SettingsStore reloaded{file.path()};
    ASSERT_TRUE(reloaded.load());

    EXPECT_TRUE(torquebus::services::bitrateFor(reloaded, QStringLiteral("peak:usb0")) == 500'000);

    // And it belongs to that interface alone. Two adapters on one desk are
    // usually on two different buses, which is the whole reason this is keyed
    // per device rather than per machine.
    EXPECT_TRUE(torquebus::services::bitrateFor(reloaded, QStringLiteral("kvaser:0"))
                == torquebus::kDefaultBitrate);
}

TEST(SettingsStoreTests, ARateNoBackendCanConfigureIsReplacedNotObeyed)
{
    // This file is JSON so that people can edit it, which makes anything in it
    // input. A controller is configured with segment timing rather than with a
    // frequency, so a rate nothing has timing for would open a channel that
    // produces error frames instead of failing outright - the worst of the
    // available outcomes.
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    ASSERT_TRUE(store.load());

    const QString key = torquebus::services::bitrateKey(QStringLiteral("kvaser:0"));

    store.setIntValue(key, 33'333);
    EXPECT_TRUE(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
                == torquebus::kDefaultBitrate);

    store.setIntValue(key, -1);
    EXPECT_TRUE(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
                == torquebus::kDefaultBitrate);

    store.setIntValue(key, 0);
    EXPECT_TRUE(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
                == torquebus::kDefaultBitrate);

    // Every rate the list offers is accepted, which is the other half of the
    // same claim - a check that only ever rejects would pass this test too.
    for (const std::uint32_t rate : torquebus::standardBitrates()) {
        store.setIntValue(key, static_cast<int>(rate));
        SCOPED_TRACE(::testing::Message() << "rate " << rate);
        EXPECT_TRUE(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0")) == rate);
    }
}

TEST(SettingsStoreTests, BitratesAreDescribedTheWayPeopleSayThem)
{
    EXPECT_TRUE(torquebus::describeBitrate(250'000) == "250 kbit/s");
    EXPECT_TRUE(torquebus::describeBitrate(1'000'000) == "1 Mbit/s");
    EXPECT_TRUE(torquebus::describeBitrate(83'000) == "83 kbit/s");

    // Not "1000 kbit/s", which is nobody's way of saying it and is the kind of
    // odd row out that gets a list misread.
    EXPECT_TRUE(torquebus::describeBitrate(1'000'000) != "1000 kbit/s");
}
