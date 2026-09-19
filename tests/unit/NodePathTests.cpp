// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A project file that names its database as `databases/vehicle.dbc` has to mean
// "beside this project", not "beside whatever directory the process happens to
// have been launched from". The second reading makes a .tbsproj open from the
// repository root and from nowhere else, which is not a property anyone would
// guess a project file had - and it fails by building an empty graph rather
// than by saying anything.

#include <catch2/catch_test_macros.hpp>

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include "UniqueTempPath.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace torquebus;
using torquebus::tests::uniqueTempDirectory;

namespace {

/// A directory holding a .dbc, removed when the test ends.
class ProjectFolder final {
public:
    ProjectFolder()
        : m_root{uniqueTempDirectory("torquebus-path-tests")}
    {
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root / "databases");

        std::ofstream file{m_root / "databases" / "vehicle.dbc"};
        file << "BO_ 257 VehicleSpeed: 8 ECU\n"
                " SG_ SpeedKmh : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" ECM\n";
    }

    ~ProjectFolder() { std::filesystem::remove_all(m_root); }

    ProjectFolder(const ProjectFolder&) = delete;
    ProjectFolder& operator=(const ProjectFolder&) = delete;

    [[nodiscard]] std::string root() const { return m_root.string(); }

private:
    std::filesystem::path m_root;
};

/// A one-node graph whose decoder names its database by `path`.
[[nodiscard]] Result buildDecoder(const std::string& path, const std::string& basePath)
{
    GraphDescription description;

    NodeDescription node;
    node.id = "decoder";
    node.typeName = "dbc.decoder";
    node.parameters.set("database", ParameterValue::fromText(path));
    description.addNode(std::move(node));

    NodeBuildContext context;
    context.basePath = basePath;

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    PipelineGraph graph;

    return description.build(catalog, context, graph);
}

} // namespace

TEST_CASE("A relative path resolves against the project, not the working directory",
          "[project][paths]")
{
    const ProjectFolder project;

    // The whole point: this string is what a portable .tbsproj contains, and it
    // has to work with the process started from anywhere at all.
    CHECK(buildDecoder("databases/vehicle.dbc", project.root()).succeeded());
}

TEST_CASE("Without a base path a relative name still means the working directory",
          "[project][paths]")
{
    const ProjectFolder project;

    // A graph built with no project behind it - from a test, or from a headless
    // run - keeps the old behaviour rather than resolving against nothing.
    CHECK(buildDecoder("databases/vehicle.dbc", "").failed());
}

TEST_CASE("An absolute path is left exactly as it was written", "[project][paths]")
{
    const ProjectFolder project;

    // Someone who typed an absolute path meant it. Reinterpreting it against a
    // project folder would be a worse bug than the one this fixes, because it
    // would break a path that had been working.
    const std::string absolute = project.root() + "/databases/vehicle.dbc";

    CHECK(buildDecoder(absolute, project.root()).succeeded());
    CHECK(buildDecoder(absolute, "/some/other/project").succeeded());
}

TEST_CASE("A missing file still fails, and names the path it looked for", "[project][paths]")
{
    const ProjectFolder project;

    const Result result = buildDecoder("databases/absent.dbc", project.root());

    REQUIRE(result.failed());

    // The resolved path, not the one from the file: "Cannot open
    // databases/absent.dbc" sends the reader looking in the wrong directory.
    const std::string message{result.message()};
    CHECK(message.find("absent.dbc") != std::string::npos);
    CHECK(message.find("databases") != std::string::npos);
}
