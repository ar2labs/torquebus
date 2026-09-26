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

#include <gtest/gtest.h>

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

/// Stands in for a KDDockWidgets layout: this class never looks inside one.
[[nodiscard]] QByteArray layout(const char* marker)
{
    return QByteArray{"layout:"} + marker;
}

} // namespace

TEST(WorkspacesTests, ASavedArrangementComesBackUnderItsName)
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    ASSERT_TRUE(workspaces.save(QStringLiteral("Vehicle Testing"), layout("a"), 6));

    EXPECT_TRUE(workspaces.names() == QStringList{QStringLiteral("Vehicle Testing")});
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Vehicle Testing"), 6) == layout("a"));
}

TEST(WorkspacesTests, WorkspacesStayInTheOrderTheyWereCreated)
{
    // Not most-recently-used. A menu whose entries move around is a menu that
    // has to be read every time instead of aimed at.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    ASSERT_TRUE(workspaces.save(QStringLiteral("Logging"), layout("a"), 6));
    ASSERT_TRUE(workspaces.save(QStringLiteral("Diagnostics"), layout("b"), 6));
    ASSERT_TRUE(workspaces.save(QStringLiteral("Logging"), layout("c"), 6));

    ASSERT_TRUE(workspaces.names().size() == 2);
    EXPECT_TRUE(workspaces.names().first() == QStringLiteral("Logging"));

    // And saving over one replaces its layout.
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Logging"), 6) == layout("c"));
}

TEST(WorkspacesTests, AWorkspaceFromABuildWithOtherPanelsIsRefusedNotHalfApplied)
{
    // The blob names every panel that existed when it was written. Restoring it
    // into a build with a panel that did not exist then leaves that panel
    // silently missing - which nobody reports as a bug, they just rearrange the
    // window again and wonder why it keeps happening.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    ASSERT_TRUE(workspaces.save(QStringLiteral("Old"), layout("a"), 5));

    EXPECT_TRUE(workspaces.contains(QStringLiteral("Old")));
    EXPECT_TRUE(workspaces.isStale(QStringLiteral("Old"), 6));
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Old"), 6).isEmpty());

    // And it is still there to be saved over, rather than having been deleted
    // behind the user's back.
    EXPECT_TRUE(workspaces.names() == QStringList{QStringLiteral("Old")});

    ASSERT_TRUE(workspaces.save(QStringLiteral("Old"), layout("new"), 6));
    EXPECT_FALSE(workspaces.isStale(QStringLiteral("Old"), 6));
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Old"), 6) == layout("new"));
}

TEST(WorkspacesTests, AWorkspaceSavedBeforeVersionsWereStoredCountsAsStale)
{
    const ScopedSettings settings;

    settings.store().setStringListValue(QStringLiteral("ui/workspaces"),
                                        {QStringLiteral("Ancient")});
    settings.store().setBinaryValue(QStringLiteral("ui/workspace/Ancient/layout"), layout("a"));

    const Workspaces workspaces{settings.store()};

    EXPECT_TRUE(workspaces.isStale(QStringLiteral("Ancient"), 6));
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Ancient"), 6).isEmpty());
}

TEST(WorkspacesTests, ANameTheSettingsFileCouldNotStoreIsRefused)
{
    // The name is the key. A slash would split it, and the workspace would read
    // back as a different one or as none - so it is refused where the user can
    // see it rather than mangled into something they did not type.
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    EXPECT_FALSE(Workspaces::isValidName(QString{}));
    EXPECT_FALSE(Workspaces::isValidName(QStringLiteral("   ")));
    EXPECT_FALSE(Workspaces::isValidName(QStringLiteral("bench/one")));
    EXPECT_TRUE(Workspaces::isValidName(QStringLiteral("Vehicle Testing")));

    EXPECT_FALSE(workspaces.save(QStringLiteral("bench/one"), layout("a"), 6));
    EXPECT_TRUE(workspaces.names().isEmpty());
}

TEST(WorkspacesTests, SurroundingSpaceInANameIsNotASecondWorkspace)
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    ASSERT_TRUE(workspaces.save(QStringLiteral("  Logging  "), layout("a"), 6));

    EXPECT_TRUE(workspaces.names() == QStringList{QStringLiteral("Logging")});
    EXPECT_TRUE(workspaces.contains(QStringLiteral("Logging")));
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Logging"), 6) == layout("a"));
}

TEST(WorkspacesTests, DeletingOneLeavesTheOthers)
{
    const ScopedSettings settings;
    Workspaces workspaces{settings.store()};

    ASSERT_TRUE(workspaces.save(QStringLiteral("A"), layout("a"), 6));
    ASSERT_TRUE(workspaces.save(QStringLiteral("B"), layout("b"), 6));

    workspaces.remove(QStringLiteral("A"));

    EXPECT_TRUE(workspaces.names() == QStringList{QStringLiteral("B")});
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("A"), 6).isEmpty());
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("B"), 6) == layout("b"));
}

TEST(WorkspacesTests, WorkspacesSurviveARestart)
{
    const ScopedSettings settings;

    {
        Workspaces workspaces{settings.store()};
        ASSERT_TRUE(workspaces.save(QStringLiteral("Simulation"), layout("a"), 6));
    }

    SettingsStore reopened{settings.store().filePath()};
    ASSERT_TRUE(reopened.load());

    const Workspaces workspaces{reopened};

    ASSERT_TRUE(workspaces.names() == QStringList{QStringLiteral("Simulation")});
    EXPECT_TRUE(workspaces.layoutFor(QStringLiteral("Simulation"), 6) == layout("a"));
}

TEST(WorkspacesTests, AHandEditedNameListCannotProduceAWorkspaceNobodyCanOpen)
{
    // The settings file is JSON on purpose. A name with a slash in it could
    // never be found again, and a duplicate would show twice in the menu.
    const ScopedSettings settings;

    settings.store().setStringListValue(QStringLiteral("ui/workspaces"),
                                        {QStringLiteral("Good"),
                                         QStringLiteral("bad/name"),
                                         QStringLiteral("Good"),
                                         QStringLiteral("  ")});

    const Workspaces workspaces{settings.store()};

    EXPECT_TRUE(workspaces.names() == QStringList{QStringLiteral("Good")});
}
