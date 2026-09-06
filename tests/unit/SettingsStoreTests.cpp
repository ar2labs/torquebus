// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/SettingsStore.h"

#include <catch2/catch_test_macros.hpp>

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
    ScopedSettingsFile()
    {
        REQUIRE(m_directory.isValid());
    }

    [[nodiscard]] QString path() const
    {
        return QDir{m_directory.path()}.filePath(QStringLiteral("settings.json"));
    }

private:
    QTemporaryDir m_directory;
};

} // namespace

TEST_CASE("A missing settings file is a first run, not an error", "[settings]")
{
    const ScopedSettingsFile file;
    SettingsStore store{file.path()};

    CHECK(store.load());
    CHECK(store.value(QStringLiteral("ui/theme"), QStringLiteral("dark")) == "dark");
}

TEST_CASE("Values survive a save/load round trip", "[settings]")
{
    const ScopedSettingsFile file;

    {
        SettingsStore store{file.path()};
        REQUIRE(store.load());

        store.setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
        store.setBoolValue(QStringLiteral("ui/window/restoreLayout"), false);
        store.setIntValue(QStringLiteral("can/defaultBitrate"), 500'000);
        store.setBinaryValue(QStringLiteral("ui/window/geometry"),
                             QByteArray::fromRawData("\x01\x00\xFF\x7F", 4));

        REQUIRE(store.save());
    }

    SettingsStore reloaded{file.path()};
    REQUIRE(reloaded.load());

    CHECK(reloaded.value(QStringLiteral("ui/theme")) == "light");
    CHECK(reloaded.boolValue(QStringLiteral("ui/window/restoreLayout"), true) == false);
    CHECK(reloaded.intValue(QStringLiteral("can/defaultBitrate")) == 500'000);
    CHECK(reloaded.binaryValue(QStringLiteral("ui/window/geometry"))
          == QByteArray::fromRawData("\x01\x00\xFF\x7F", 4));
}

TEST_CASE("The settings file is human readable JSON", "[settings]")
{
    // The whole reason for choosing JSON over the registry: an engineer must be
    // able to read, diff and copy this file between machines.
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    REQUIRE(store.load());
    store.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
    REQUIRE(store.save());

    QFile written{file.path()};
    REQUIRE(written.open(QIODevice::ReadOnly | QIODevice::Text));

    const QString contents = QString::fromUtf8(written.readAll());
    CHECK(contents.contains(QStringLiteral("\"ui/theme\"")));
    CHECK(contents.contains(QStringLiteral("dark")));
    CHECK(contents.contains(QLatin1Char('\n'))); // indented, not minified
}

TEST_CASE("A corrupt settings file does not prevent startup", "[settings]")
{
    const ScopedSettingsFile file;

    QFile corrupt{file.path()};
    REQUIRE(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{ this is not json");
    corrupt.close();

    SettingsStore store{file.path()};

    // load() reports the problem...
    CHECK_FALSE(store.load());

    // ...but the store is usable, and defaults apply.
    CHECK(store.value(QStringLiteral("ui/theme"), QStringLiteral("dark")) == "dark");
    CHECK(store.save());
}

TEST_CASE("Removing a key removes it from the file", "[settings]")
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    REQUIRE(store.load());

    store.setValue(QStringLiteral("project/lastOpened"), QStringLiteral("C:/EngineTest.tbsproj"));
    CHECK(store.contains(QStringLiteral("project/lastOpened")));

    store.remove(QStringLiteral("project/lastOpened"));
    CHECK_FALSE(store.contains(QStringLiteral("project/lastOpened")));

    REQUIRE(store.save());

    SettingsStore reloaded{file.path()};
    REQUIRE(reloaded.load());
    CHECK_FALSE(reloaded.contains(QStringLiteral("project/lastOpened")));
}

TEST_CASE("An empty binary value clears its key rather than storing nothing",
          "[settings]")
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    REQUIRE(store.load());

    store.setBinaryValue(QStringLiteral("ui/window/dockLayout"), QByteArray{"layout"});
    REQUIRE(store.contains(QStringLiteral("ui/window/dockLayout")));

    store.setBinaryValue(QStringLiteral("ui/window/dockLayout"), QByteArray{});
    CHECK_FALSE(store.contains(QStringLiteral("ui/window/dockLayout")));
}

// ---------------------------------------------------------------------------
// Per-interface bitrate
// ---------------------------------------------------------------------------

TEST_CASE("A bitrate key names the interface it belongs to", "[settings][bitrate]")
{
    // The shape is a format, not a convenience: these keys are read back by a
    // later run and are the sort of thing people hand-edit.
    CHECK(torquebus::services::bitrateKey(QStringLiteral("peak:usb0"))
          == QStringLiteral("can/bitrate/peak:usb0"));
    CHECK(torquebus::services::bitrateKey(QStringLiteral("kvaser:0"))
          == QStringLiteral("can/bitrate/kvaser:0"));
}

TEST_CASE("An interface nobody configured gets the default", "[settings][bitrate]")
{
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    REQUIRE(store.load());

    CHECK(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
          == torquebus::kDefaultBitrate);
}

TEST_CASE("A stored bitrate survives a save and a load", "[settings][bitrate]")
{
    const ScopedSettingsFile file;

    {
        SettingsStore store{file.path()};
        REQUIRE(store.load());
        store.setIntValue(torquebus::services::bitrateKey(QStringLiteral("peak:usb0")),
                          500'000);
        REQUIRE(store.save());
    }

    SettingsStore reloaded{file.path()};
    REQUIRE(reloaded.load());

    CHECK(torquebus::services::bitrateFor(reloaded, QStringLiteral("peak:usb0")) == 500'000);

    // And it belongs to that interface alone. Two adapters on one desk are
    // usually on two different buses, which is the whole reason this is keyed
    // per device rather than per machine.
    CHECK(torquebus::services::bitrateFor(reloaded, QStringLiteral("kvaser:0"))
          == torquebus::kDefaultBitrate);
}

TEST_CASE("A rate no backend can configure is replaced, not obeyed", "[settings][bitrate]")
{
    // This file is JSON so that people can edit it, which makes anything in it
    // input. A controller is configured with segment timing rather than with a
    // frequency, so a rate nothing has timing for would open a channel that
    // produces error frames instead of failing outright - the worst of the
    // available outcomes.
    const ScopedSettingsFile file;

    SettingsStore store{file.path()};
    REQUIRE(store.load());

    const QString key = torquebus::services::bitrateKey(QStringLiteral("kvaser:0"));

    store.setIntValue(key, 33'333);
    CHECK(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
          == torquebus::kDefaultBitrate);

    store.setIntValue(key, -1);
    CHECK(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
          == torquebus::kDefaultBitrate);

    store.setIntValue(key, 0);
    CHECK(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0"))
          == torquebus::kDefaultBitrate);

    // Every rate the list offers is accepted, which is the other half of the
    // same claim - a check that only ever rejects would pass this test too.
    for (const std::uint32_t rate : torquebus::standardBitrates()) {
        store.setIntValue(key, static_cast<int>(rate));
        INFO("rate " << rate);
        CHECK(torquebus::services::bitrateFor(store, QStringLiteral("kvaser:0")) == rate);
    }
}

TEST_CASE("Bitrates are described the way people say them", "[bitrate]")
{
    CHECK(torquebus::describeBitrate(250'000) == "250 kbit/s");
    CHECK(torquebus::describeBitrate(1'000'000) == "1 Mbit/s");
    CHECK(torquebus::describeBitrate(83'000) == "83 kbit/s");

    // Not "1000 kbit/s", which is nobody's way of saying it and is the kind of
    // odd row out that gets a list misread.
    CHECK(torquebus::describeBitrate(1'000'000) != "1000 kbit/s");
}
