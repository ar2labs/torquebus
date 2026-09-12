// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Loading plugins, and refusing them.
//
// The refusals are the point. Every way a plugin can fail to load ends the same
// way on screen - a backend that is not in the list - and somebody then checks
// the cable, the driver and the device manager, none of which is the problem.
// So each case here checks two things: that the bad plugin did not load, and
// that the reason given names something a person could act on.
//
// The libraries are real, built by this build from tests/plugins. A refused
// plugin has to genuinely be wrong; a test that described one as wrong would be
// testing the description.

#include "plugins/host/PluginLoader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace torquebus;
using namespace torquebus::plugins;

namespace {

/// Everything the host hands a plugin, plus a record of what it said.
struct Harness final {
    /// The registry is a singleton, so this is a reference to the one the
    /// process already has rather than a second one. The node catalogue is
    /// not, so each case gets its own and they cannot contaminate each other.
    CanBackendRegistry& backends = CanBackendRegistry::instance();
    NodeCatalog nodes = NodeCatalog::withBuiltinTypes();

    std::vector<std::string> messages;
    std::vector<std::string> errors;

    [[nodiscard]] PluginHost host()
    {
        PluginHost result;
        result.backends = &backends;
        result.nodes = &nodes;
        result.log = [this](std::string_view text, bool isError) {
            (isError ? errors : messages).emplace_back(text);
        };

        return result;
    }
};

[[nodiscard]] std::filesystem::path pluginDirectory()
{
    return std::filesystem::path{TORQUEBUS_TEST_PLUGIN_DIR};
}

/// The rejection whose path contains `fragment`, or nullptr.
[[nodiscard]] const RejectedPlugin* rejectionFor(const PluginLoader& loader,
                                                 std::string_view fragment)
{
    for (const RejectedPlugin& rejected : loader.rejected()) {
        if (rejected.path.find(fragment) != std::string::npos) {
            return &rejected;
        }
    }

    return nullptr;
}

[[nodiscard]] bool loadedContains(const PluginLoader& loader, std::string_view fragment)
{
    return std::any_of(loader.loaded().begin(), loader.loaded().end(),
                       [fragment](const LoadedPlugin& plugin) {
                           return plugin.path.find(fragment) != std::string::npos;
                       });
}

/// Loads the whole test plugin directory once.
[[nodiscard]] PluginLoader loadAll(Harness& harness)
{
    PluginLoader loader;
    loader.loadFrom(pluginDirectory(), harness.host());

    return loader;
}

} // namespace

TEST_CASE("A plugin registers a block the application can then build",
          "[plugins]")
{
    // The seam that matters: a type that came from outside the binary is in the
    // same catalogue as the built-in ones, registered through the same call,
    // and builds like any other.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    REQUIRE(loadedContains(loader, "good"));

    const NodeTypeInfo* info = harness.nodes.find("test.passthrough");
    REQUIRE(info != nullptr);
    CHECK(info->category == "Transforms");

    NodeBuildContext context;
    std::unique_ptr<IPipelineNode> node;

    REQUIRE(harness.nodes.create("test.passthrough", NodeParameters{}, context, "p", node)
                .succeeded());
    REQUIRE(node != nullptr);
    CHECK(node->typeName() == "test.passthrough");
}

TEST_CASE("A plugin built with another toolchain is refused, and both keys are shown",
          "[plugins]")
{
    // "Incompatible" is not something anybody can act on. The difference
    // between the two strings usually names the fix - a Debug plugin next to a
    // Release build, a compiler that moved on.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    const RejectedPlugin* rejected = rejectionFor(loader, "badkey");
    REQUIRE(rejected != nullptr);

    CHECK(rejected->reason.find("some-other-compiler") != std::string::npos);
    CHECK(rejected->reason.find(std::string{PluginLoader::hostBuildKey()})
          != std::string::npos);

    CHECK_FALSE(loadedContains(loader, "badkey"));
}

TEST_CASE("A plugin from a future version is refused by the two fields that never move",
          "[plugins]")
{
    // Its PluginInfo may be laid out in a way this build has never seen. Only
    // the ABI version and the build key have a position both sides agreed on,
    // and the refusal has to be decided from those alone.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    const RejectedPlugin* rejected = rejectionFor(loader, "badabi");
    REQUIRE(rejected != nullptr);

    CHECK(rejected->reason.find("ABI") != std::string::npos);
    CHECK_FALSE(loadedContains(loader, "badabi"));
}

TEST_CASE("A library that is not a plugin is named, not passed over", "[plugins]")
{
    // A DLL in the plugins directory that exports nothing TorqueBus knows. It
    // is somebody putting the wrong file in the right folder, and saying so is
    // the whole difference between a five-second fix and an afternoon.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    const RejectedPlugin* rejected = rejectionFor(loader, "nosymbol");
    REQUIRE(rejected != nullptr);

    CHECK(rejected->reason.find("torquebusPluginQuery") != std::string::npos);
}

TEST_CASE("A plugin that throws does not take the others with it", "[plugins]")
{
    // One bad plugin must not cost the person every other plugin, or the
    // program. The throwing one is refused and the good one is still there.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    const RejectedPlugin* rejected = rejectionFor(loader, "throws");
    REQUIRE(rejected != nullptr);
    CHECK(rejected->reason.find("threw") != std::string::npos);

    CHECK(loadedContains(loader, "good"));
    CHECK(harness.nodes.find("test.passthrough") != nullptr);
}

TEST_CASE("A plugin that declines says why itself", "[plugins]")
{
    // What a driver plugin does when its SDK is not installed. It is the only
    // one that knows why, so the loader records that it declined and does not
    // invent a second explanation on top of the plugin's own.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    const RejectedPlugin* rejected = rejectionFor(loader, "declines");
    REQUIRE(rejected != nullptr);
    CHECK(rejected->reason.find("declined") != std::string::npos);

    const bool saidSo = std::any_of(harness.errors.begin(), harness.errors.end(),
                                    [](const std::string& text) {
                                        return text.find("SDK is not installed")
                                               != std::string::npos;
                                    });
    CHECK(saidSo);
}

TEST_CASE("Every plugin in the directory is accounted for", "[plugins]")
{
    // Loaded plus refused, and nothing quietly skipped. A file that is neither
    // is a file nobody will ever ask about.
    Harness harness;
    const PluginLoader loader = loadAll(harness);

    std::size_t libraries = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{pluginDirectory()}) {
        if (entry.is_regular_file() && entry.path().extension() == PluginLoader::extension()) {
            ++libraries;
        }
    }

    REQUIRE(libraries > 0U);
    CHECK(loader.loaded().size() + loader.rejected().size() == libraries);
}

TEST_CASE("A directory with no plugins is not a failure", "[plugins]")
{
    // What a build with no plugins looks like. The program is expected to work
    // without any, so this cannot be an error and must not produce one.
    Harness harness;

    PluginLoader loader;
    loader.loadFrom(pluginDirectory() / "there-is-no-such-directory", harness.host());

    CHECK(loader.loaded().empty());
    CHECK(loader.rejected().empty());
    CHECK(harness.errors.empty());
}

TEST_CASE("Plugins are looked for beside the executable and nowhere else",
          "[plugins]")
{
    // Not the working directory, and not PATH. Loading a library from the
    // working directory is how opening a project turns into running whatever
    // was in that folder.
    const std::filesystem::path executable = "C:/Program Files/TorqueBus/TorqueBusStudio.exe";
    const std::filesystem::path directory = PluginLoader::directoryFor(executable);

    CHECK(directory.filename() == "plugins");
    CHECK(directory.parent_path() == executable.parent_path());
}

TEST_CASE("The host build key names what a plugin has to match", "[plugins]")
{
    // The three things that decide whether two binaries can pass a std::function
    // to each other. Qt is deliberately not among them: nothing crossing this
    // boundary is a Qt type.
    const std::string key{PluginLoader::hostBuildKey()};

    CHECK(key.find("torquebus-abi-") != std::string::npos);
    CHECK(key.find("stl-") != std::string::npos);
    CHECK(key.find("x64") != std::string::npos);
    CHECK(key.find("Qt") == std::string::npos);
}
