// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A recent-projects list is almost all policy, and every case here is one of
// the policies: what counts as the same file, what happens when the list is
// full, and what a missing file is allowed to do to it.

#include "services/RecentProjects.h"
#include "services/SettingsStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using torquebus::services::RecentProjects;
using torquebus::services::SettingsStore;

namespace {

/// A settings file of its own per test, so the suite never touches the
/// developer's real one.
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

    /// A path inside the temporary directory. Absolute and real-looking,
    /// which matters: the list resolves what it is given.
    [[nodiscard]] QString path(const QString& name) const
    {
        return QDir{m_directory.path()}.filePath(name);
    }

private:
    QTemporaryDir m_directory;
    std::unique_ptr<SettingsStore> m_store;
};

} // namespace

TEST_CASE("The most recently opened project is first", "[recent]")
{
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    CHECK(recent.isEmpty());

    recent.add(settings.path(QStringLiteral("one.tbsproj")));
    recent.add(settings.path(QStringLiteral("two.tbsproj")));

    REQUIRE(recent.paths().size() == 2);
    CHECK(recent.paths().first().endsWith(QStringLiteral("two.tbsproj")));
}

TEST_CASE("Opening a project again moves it up rather than adding it twice",
          "[recent]")
{
    // Order is use, not time. Somebody who alternates between two projects
    // should find both at the top, not one of them buried under nine copies of
    // the other.
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    recent.add(settings.path(QStringLiteral("a.tbsproj")));
    recent.add(settings.path(QStringLiteral("b.tbsproj")));
    recent.add(settings.path(QStringLiteral("a.tbsproj")));

    REQUIRE(recent.paths().size() == 2);
    CHECK(recent.paths().first().endsWith(QStringLiteral("a.tbsproj")));
}

TEST_CASE("The same file spelled two ways is one entry", "[recent]")
{
    // Paths reach this class from a file dialog, a command line and a settings
    // file written by an older build. They differ in separators long before
    // they differ in meaning.
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    const QString native = settings.path(QStringLiteral("bench.tbsproj"));

    recent.add(native);
    recent.add(QDir::fromNativeSeparators(native));

    CHECK(recent.paths().size() == 1);
}

TEST_CASE("The list stops at ten", "[recent]")
{
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    for (int index = 0; index < RecentProjects::kMaximum + 5; ++index) {
        recent.add(settings.path(QStringLiteral("project%1.tbsproj").arg(index)));
    }

    REQUIRE(recent.paths().size() == RecentProjects::kMaximum);

    // The oldest went, not the newest.
    CHECK(recent.paths().first().endsWith(QStringLiteral("project14.tbsproj")));
    CHECK_FALSE(recent.paths().last().endsWith(QStringLiteral("project0.tbsproj")));
}

TEST_CASE("A project that has never been saved is not remembered", "[recent]")
{
    // Save on an untitled project goes through Save As, and the moment before
    // that is not a project worth putting in a menu.
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    recent.add(QString{});
    recent.add(QStringLiteral("   "));

    CHECK(recent.isEmpty());
}

TEST_CASE("The list survives a restart", "[recent]")
{
    // Written through on every change rather than at exit: the list is worth
    // least in exactly the case where an exit does not happen.
    const ScopedSettings settings;

    {
        RecentProjects recent{settings.store()};
        recent.add(settings.path(QStringLiteral("kept.tbsproj")));
    }

    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());

    const RecentProjects recent{reopened};
    REQUIRE(recent.paths().size() == 1);
    CHECK(recent.paths().first().endsWith(QStringLiteral("kept.tbsproj")));
}

TEST_CASE("An entry is only forgotten when asked", "[recent]")
{
    // A project on a network share is not gone because it is unreachable this
    // morning. Nothing here checks whether a file exists - the window removes
    // an entry when opening it actually failed, which is the only evidence.
    const ScopedSettings settings;
    RecentProjects recent{settings.store()};

    const QString missing = settings.path(QStringLiteral("never-created.tbsproj"));

    recent.add(missing);
    CHECK(recent.paths().size() == 1);

    recent.remove(missing);
    CHECK(recent.isEmpty());
}

TEST_CASE("Clearing the list empties the setting too", "[recent]")
{
    const ScopedSettings settings;

    {
        RecentProjects recent{settings.store()};
        recent.add(settings.path(QStringLiteral("gone.tbsproj")));
        recent.clear();
        CHECK(recent.isEmpty());
    }

    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());
    CHECK(reopened.stringListValue(
                  QString::fromLatin1(torquebus::services::keys::kRecentProjects))
              .isEmpty());
}

TEST_CASE("A hand-edited settings file cannot put rubbish in the menu", "[recent]")
{
    // The settings file is JSON on purpose, so somebody will edit it. Blanks,
    // duplicates and a list longer than the maximum are all cleaned on the way
    // in rather than being allowed to reach a menu.
    const ScopedSettings settings;

    QStringList written;
    written << settings.path(QStringLiteral("real.tbsproj"))
            << QString{}
            << settings.path(QStringLiteral("real.tbsproj"))
            << QStringLiteral("  ");

    for (int index = 0; index < 20; ++index) {
        written << settings.path(QStringLiteral("filler%1.tbsproj").arg(index));
    }

    settings.store().setStringListValue(
        QString::fromLatin1(torquebus::services::keys::kRecentProjects), written);

    const RecentProjects recent{settings.store()};

    CHECK(recent.paths().size() == RecentProjects::kMaximum);
    CHECK(recent.paths().first().endsWith(QStringLiteral("real.tbsproj")));

    for (const QString& path : recent.paths()) {
        CHECK_FALSE(path.trimmed().isEmpty());
    }
}

TEST_CASE("A string list survives being stored and read back", "[settings]")
{
    // Stored as a JSON array rather than a joined string, so a path containing
    // a semicolon - legal on every platform this runs on - cannot split in two.
    const ScopedSettings settings;

    const QStringList original{QStringLiteral("C:/work/a;b.tbsproj"),
                               QStringLiteral("/home/me/c.tbsproj")};

    settings.store().setStringListValue(QStringLiteral("test/list"), original);
    REQUIRE(settings.store().save());

    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());

    CHECK(reopened.stringListValue(QStringLiteral("test/list")) == original);
}
