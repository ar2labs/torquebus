// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where a project finds the pictures of its dashboard, and how that survives Save As.
//
// The rule is the same one the database list follows - relative to the project when the file is in
// or near its folder, whole otherwise - and what makes it worth a test file is the move: a project
// saved to another folder has not moved its pictures, and the only way that stays true is to work
// every path out again. Each case builds a real tree in the temp directory, because a path rule
// tested on strings alone passes on the machine it was written on.

#include "UniqueTempPath.h"

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/DashboardDescription.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace torquebus;

namespace fs = std::filesystem;

namespace {

/// A path as a project stores it: UTF-8 with forward slashes.
[[nodiscard]] std::string generic(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

/// <temp>/dashboard_assets_<tag>/{project/img, other, copy}, removed when the test ends.
class AssetsTree final {
public:
    AssetsTree()
        : m_root{tests::uniqueTempDirectory("dashboard_assets")}
    {
        std::error_code ignored;
        fs::remove_all(m_root, ignored);

        fs::create_directories(m_root / "project" / "img");
        fs::create_directories(m_root / "other");
        fs::create_directories(m_root / "copy");
    }

    ~AssetsTree()
    {
        std::error_code ignored;
        fs::remove_all(m_root, ignored);
    }

    AssetsTree(const AssetsTree&) = delete;
    AssetsTree& operator=(const AssetsTree&) = delete;

    [[nodiscard]] fs::path root() const { return m_root; }
    [[nodiscard]] std::string project() const { return generic(m_root / "project"); }
    [[nodiscard]] std::string copy() const { return generic(m_root / "copy"); }

    /// Makes the file, so that it exists, and says where it is.
    [[nodiscard]] std::string touch(const fs::path& relative) const
    {
        const fs::path path = m_root / relative;

        std::ofstream file{path, std::ios::binary};
        file << "x";

        return generic(path);
    }

private:
    fs::path m_root;
};

[[nodiscard]] DashboardWidget imageWidget(std::string id, std::string image)
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Image);
    widget.id = std::move(id);
    widget.image = std::move(image);
    return widget;
}

} // namespace

TEST(DashboardAssetsTests, APictureIsRecognisedByItsExtensionAndNotByAFolderNameWithADot)
{
    EXPECT_TRUE(isImagePath("logo.png"));
    EXPECT_TRUE(isImagePath("LOGO.PNG"));
    EXPECT_TRUE(isImagePath("photo.jpg"));
    EXPECT_TRUE(isImagePath("photo.jpeg"));
    EXPECT_TRUE(isImagePath("icon.svg"));
    EXPECT_TRUE(isImagePath("anim.gif"));
    EXPECT_TRUE(isImagePath("old.bmp"));
    EXPECT_TRUE(isImagePath("some/where/else.png"));
    EXPECT_TRUE(isImagePath("C:\\pictures\\a.png"));
    EXPECT_TRUE(isImagePath("v2.0/pictures/a.png"));

    EXPECT_FALSE(isImagePath("notes.txt"));
    EXPECT_FALSE(isImagePath("program.exe"));
    EXPECT_FALSE(isImagePath("no_extension"));
    EXPECT_FALSE(isImagePath("trailing."));
    EXPECT_FALSE(isImagePath(""));

    // The dot is in the folder, and the file has none.
    EXPECT_FALSE(isImagePath("pictures.png/readme"));
    EXPECT_FALSE(isImagePath("pictures.png\\readme"));
}

TEST(DashboardAssetsTests, ARelativePathIsResolvedAgainstTheProjectsFolder)
{
    const AssetsTree tree;

    EXPECT_EQ(resolveAsset("img/a.png", tree.project()),
              generic(tree.root() / "project" / "img" / "a.png"));

    // Folded, so that two spellings of one file are one string.
    EXPECT_EQ(resolveAsset("../other/b.png", tree.project()),
              generic(tree.root() / "other" / "b.png"));
    EXPECT_EQ(resolveAsset("./img/../img/a.png", tree.project()),
              generic(tree.root() / "project" / "img" / "a.png"));

    // An absolute path is where it says.
    const std::string whole = generic(tree.root() / "other" / "b.png");
    EXPECT_EQ(resolveAsset(whole, tree.project()), whole);

    EXPECT_EQ(resolveAsset("", tree.project()), "");
}

TEST(DashboardAssetsTests, AProjectNotSavedYetMeansTheCurrentDirectory)
{
    std::error_code error;
    const fs::path here = fs::current_path(error);
    ASSERT_FALSE(error);

    EXPECT_EQ(resolveAsset("pictures/a.png", ""), generic(here / "pictures" / "a.png"));
}

TEST(DashboardAssetsTests, AStoredPathIsRelativeInsideOrNearAndWholeFarAway)
{
    const AssetsTree tree;

    EXPECT_EQ(storableAsset(generic(tree.root() / "project" / "img" / "a.png"), tree.project()),
              "img/a.png");
    EXPECT_EQ(storableAsset(generic(tree.root() / "project" / "a.png"), tree.project()), "a.png");

    // A sibling folder: a project and its assets side by side, which is how they are checked in.
    EXPECT_EQ(storableAsset(generic(tree.root() / "other" / "b.png"), tree.project()),
              "../other/b.png");

    // Further than four folders up is not "near", and a path with that many dots in it is one
    // nobody can read: it is stored whole.
    const std::string deep = generic(tree.root() / "project" / "a" / "b" / "c" / "d" / "e");
    const std::string far = generic(tree.root() / "other" / "z.png");
    EXPECT_EQ(storableAsset(far, deep), far);

    const std::string nearer = generic(tree.root() / "project" / "a" / "b" / "c");
    EXPECT_EQ(storableAsset(far, nearer), "../../../../other/z.png");

    // No folder, nothing to be relative to.
    const std::string whole = generic(tree.root() / "project" / "img" / "a.png");
    EXPECT_EQ(storableAsset(whole, ""), whole);

    EXPECT_EQ(storableAsset("", tree.project()), "");
}

#ifdef _WIN32
TEST(DashboardAssetsTests, AFileOnAnotherDriveIsStoredWholeBecauseNoRelativePathReachesIt)
{
    // Worked out on the strings: nothing asks the disk whether the drives exist.
    EXPECT_EQ(storableAsset("Z:/pictures/a.png", "C:/project"), "Z:/pictures/a.png");
    EXPECT_EQ(storableAsset("C:/project/img/a.png", "C:/project"), "img/a.png");
}
#endif

TEST(DashboardAssetsTests, StoringWhatWasResolvedGivesBackWhatWasWritten)
{
    const AssetsTree tree;

    for (const char* written : {"img/a.png", "a.png", "../other/b.png", "../../x/y.png"}) {
        SCOPED_TRACE(written);
        EXPECT_EQ(storableAsset(resolveAsset(written, tree.project()), tree.project()), written);
    }
}

TEST(DashboardAssetsTests, SaveAsKeepsEveryPictureNamingTheFileItNamedBefore)
{
    const AssetsTree tree;

    DashboardDescription dashboard;

    DashboardWidget logo = imageWidget("logo", "img/a.png");
    DashboardState on;
    on.value = 1.0;
    on.label = "On";
    on.image = "img/on.png";
    logo.states.push_back(on);
    dashboard.add(logo);

    dashboard.add(imageWidget("shared", "../other/b.png"));
    dashboard.add(imageWidget("none", ""));

    const DashboardDescription original = dashboard;

    // The project moves to a sibling folder; the pictures stay where they are.
    rebaseAssets(dashboard, tree.project(), tree.copy());

    EXPECT_EQ(dashboard.find("logo")->image, "../project/img/a.png");
    EXPECT_EQ(dashboard.find("logo")->states.front().image, "../project/img/on.png");
    EXPECT_EQ(dashboard.find("shared")->image, "../other/b.png");
    EXPECT_EQ(dashboard.find("none")->image, "");

    // Each still resolves to the file it resolved to.
    for (const DashboardWidget& before : original.widgets()) {
        EXPECT_EQ(resolveAsset(dashboard.find(before.id)->image, tree.copy()),
                  resolveAsset(before.image, tree.project()))
            << before.id;
    }

    // And back again is where it began.
    rebaseAssets(dashboard, tree.copy(), tree.project());
    EXPECT_EQ(dashboard, original);
}

TEST(DashboardAssetsTests, ANewProjectSavedForTheFirstTimeTurnsItsWholePathsIntoNearOnes)
{
    const AssetsTree tree;

    // Chosen before the project had a folder, so stored whole.
    const std::string chosen = generic(tree.root() / "project" / "img" / "a.png");

    DashboardDescription dashboard;
    dashboard.add(imageWidget("logo", chosen));

    rebaseAssets(dashboard, "", tree.project());

    EXPECT_EQ(dashboard.find("logo")->image, "img/a.png");
}

TEST(DashboardAssetsTests, RebasingToTheSameFolderChangesNothing)
{
    const AssetsTree tree;

    DashboardDescription dashboard;
    dashboard.add(imageWidget("logo", "img/a.png"));
    dashboard.add(imageWidget("odd", "./img/../img/a.png"));

    const DashboardDescription before = dashboard;

    rebaseAssets(dashboard, tree.project(), tree.project());
    EXPECT_EQ(dashboard, before);
}

TEST(DashboardAssetsTests, EachPictureIsListedOnceInTheOrderItIsFirstMet)
{
    DashboardDescription dashboard;

    DashboardWidget first = imageWidget("first", "a.png");
    DashboardState state;
    state.value = 0.0;
    state.label = "Off";
    state.image = "b.png";
    first.states.push_back(state);
    dashboard.add(first);

    dashboard.add(imageWidget("second", "a.png"));
    dashboard.add(imageWidget("third", "c.png"));
    dashboard.add(imageWidget("empty", ""));

    EXPECT_EQ(assetsUsedBy(dashboard), (std::vector<std::string>{"a.png", "b.png", "c.png"}));
}

TEST(DashboardAssetsTests, AMissingPictureNamesItsWidgetAndTheWayItWasWritten)
{
    const AssetsTree tree;
    (void)tree.touch(fs::path{"project"} / "img" / "present.png");

    DashboardDescription dashboard;
    dashboard.add(imageWidget("here", "img/present.png"));
    dashboard.add(imageWidget("gone", "img/missing.png"));

    DashboardWidget lamp = defaultWidgetOf(DashboardWidgetKind::Lamp);
    lamp.id = "lamp";
    DashboardState state;
    state.value = 1.0;
    state.label = "On";
    state.image = "lost.png";
    lamp.states.push_back(state);
    dashboard.add(lamp);

    const std::vector<MissingAsset> missing = missingAssets(dashboard, tree.project());

    ASSERT_EQ(missing.size(), 2U);
    EXPECT_EQ(missing[0].widgetId, "gone");
    EXPECT_EQ(missing[0].image, "img/missing.png");
    EXPECT_EQ(missing[1].widgetId, "lamp");
    EXPECT_EQ(missing[1].image, "lost.png");

    // Found from where the project is, and not from where the program was started.
    EXPECT_TRUE(missingAssets(DashboardDescription{}, tree.project()).empty());
}
