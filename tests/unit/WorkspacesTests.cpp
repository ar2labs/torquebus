// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A workspace is a name and a layout blob, so the cases worth writing are the
// two ways a name and a blob can betray you: a name the settings file would
// read back as something else, and a blob describing panels this build does not
// have.

#include "services/SettingsStore.h"
#include "services/Workspaces.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QTemporaryDir>

#include <memory>

using torquebus::services::SettingsStore;
using torquebus::services::Workspaces;

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

/// Stands in for a KDDockWidgets layout: this class never looks inside one.
[[nodiscard]] QByteArray layout(const char* marker)
{
    return QByteArray{"layout:"} + marker;
}

} // namespace

TEST_CASE("A saved arrangement comes back under its name", "[workspace]")
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    REQUIRE(workspaces.save(QStringLiteral("Vehicle Testing"), layout("a"), 6));

    CHECK(workspaces.names() == QStringList{QStringLiteral("Vehicle Testing")});
    CHECK(workspaces.layoutFor(QStringLiteral("Vehicle Testing"), 6) == layout("a"));
}

TEST_CASE("Workspaces stay in the order they were created", "[workspace]")
{
    // Not most-recently-used. A menu whose entries move around is a menu that
    // has to be read every time instead of aimed at.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    REQUIRE(workspaces.save(QStringLiteral("Logging"), layout("a"), 6));
    REQUIRE(workspaces.save(QStringLiteral("Diagnostics"), layout("b"), 6));
    REQUIRE(workspaces.save(QStringLiteral("Logging"), layout("c"), 6));

    REQUIRE(workspaces.names().size() == 2);
    CHECK(workspaces.names().first() == QStringLiteral("Logging"));

    // And saving over one replaces its layout.
    CHECK(workspaces.layoutFor(QStringLiteral("Logging"), 6) == layout("c"));
}

TEST_CASE("A workspace from a build with other panels is refused, not half applied",
          "[workspace]")
{
    // The blob names every panel that existed when it was written. Restoring it
    // into a build with a panel that did not exist then leaves that panel
    // silently missing - which nobody reports as a bug, they just rearrange the
    // window again and wonder why it keeps happening.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    REQUIRE(workspaces.save(QStringLiteral("Old"), layout("a"), 5));

    CHECK(workspaces.contains(QStringLiteral("Old")));
    CHECK(workspaces.isStale(QStringLiteral("Old"), 6));
    CHECK(workspaces.layoutFor(QStringLiteral("Old"), 6).isEmpty());

    // And it is still there to be saved over, rather than having been deleted
    // behind the user's back.
    CHECK(workspaces.names() == QStringList{QStringLiteral("Old")});

    REQUIRE(workspaces.save(QStringLiteral("Old"), layout("new"), 6));
    CHECK_FALSE(workspaces.isStale(QStringLiteral("Old"), 6));
    CHECK(workspaces.layoutFor(QStringLiteral("Old"), 6) == layout("new"));
}

TEST_CASE("A workspace saved before versions were stored counts as stale",
          "[workspace]")
{
    const ScopedSettings settings;

    settings.store().setStringListValue(QStringLiteral("ui/workspaces"),
                                        {QStringLiteral("Ancient")});
    settings.store().setBinaryValue(QStringLiteral("ui/workspace/Ancient/layout"),
                                    layout("a"));

    const Workspaces workspaces{settings.store()};

    CHECK(workspaces.isStale(QStringLiteral("Ancient"), 6));
    CHECK(workspaces.layoutFor(QStringLiteral("Ancient"), 6).isEmpty());
}

TEST_CASE("A name the settings file could not store is refused", "[workspace]")
{
    // The name is the key. A slash would split it, and the workspace would read
    // back as a different one or as none - so it is refused where the user can
    // see it rather than mangled into something they did not type.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    CHECK_FALSE(Workspaces::isValidName(QString{}));
    CHECK_FALSE(Workspaces::isValidName(QStringLiteral("   ")));
    CHECK_FALSE(Workspaces::isValidName(QStringLiteral("bench/one")));
    CHECK(Workspaces::isValidName(QStringLiteral("Vehicle Testing")));

    CHECK_FALSE(workspaces.save(QStringLiteral("bench/one"), layout("a"), 6));
    CHECK(workspaces.names().isEmpty());
}

TEST_CASE("Surrounding space in a name is not a second workspace", "[workspace]")
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    REQUIRE(workspaces.save(QStringLiteral("  Logging  "), layout("a"), 6));

    CHECK(workspaces.names() == QStringList{QStringLiteral("Logging")});
    CHECK(workspaces.contains(QStringLiteral("Logging")));
    CHECK(workspaces.layoutFor(QStringLiteral("Logging"), 6) == layout("a"));
}

TEST_CASE("Deleting one leaves the others", "[workspace]")
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    REQUIRE(workspaces.save(QStringLiteral("A"), layout("a"), 6));
    REQUIRE(workspaces.save(QStringLiteral("B"), layout("b"), 6));

    workspaces.remove(QStringLiteral("A"));

    CHECK(workspaces.names() == QStringList{QStringLiteral("B")});
    CHECK(workspaces.layoutFor(QStringLiteral("A"), 6).isEmpty());
    CHECK(workspaces.layoutFor(QStringLiteral("B"), 6) == layout("b"));
}

TEST_CASE("Workspaces survive a restart", "[workspace]")
{
    const ScopedSettings settings;

    {
        Workspaces workspaces{settings.store()};
        REQUIRE(workspaces.save(QStringLiteral("Simulation"), layout("a"), 6));
    }

    SettingsStore reopened{settings.store().filePath()};
    REQUIRE(reopened.load());

    const Workspaces workspaces{reopened};

    REQUIRE(workspaces.names() == QStringList{QStringLiteral("Simulation")});
    CHECK(workspaces.layoutFor(QStringLiteral("Simulation"), 6) == layout("a"));
}

TEST_CASE("A hand-edited name list cannot produce a workspace nobody can open",
          "[workspace]")
{
    // The settings file is JSON on purpose. A name with a slash in it could
    // never be found again, and a duplicate would show twice in the menu.
    const ScopedSettings settings;

    settings.store().setStringListValue(
        QStringLiteral("ui/workspaces"),
        {QStringLiteral("Good"), QStringLiteral("bad/name"), QStringLiteral("Good"),
         QStringLiteral("  ")});

    const Workspaces workspaces{settings.store()};

    CHECK(workspaces.names() == QStringList{QStringLiteral("Good")});
}
