// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A project file has one job that matters above the rest: what comes back must
// be what went in. Everything the user drew - block ids, types, settings,
// wires, positions, which blocks are switched off - has to survive a save and a
// load unchanged, because the alternative is losing an afternoon's work to a
// field this file forgot to write.
//
// So the central test is a round trip through a real file on disk, compared
// with ==, rather than a set of assertions about individual fields that would
// pass while a newly added field was quietly dropped.

#include <catch2/catch_test_macros.hpp>

#include "core/pipeline/GraphDescription.h"
#include "services/ProjectFile.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <string>

using namespace torquebus;
using namespace torquebus::services;

namespace {

/// A pipeline using every feature the format has to carry.
[[nodiscard]] GraphDescription richPipeline()
{
    GraphDescription pipeline;

    NodeDescription source;
    source.id = "can_1";
    source.typeName = "can.source";
    source.parameters.set("channel", ParameterValue::fromInteger(0));
    source.x = -120.5;
    source.y = 40.0;
    pipeline.addNode(source);

    NodeDescription ecu;
    ecu.id = "ecu_motor";
    ecu.typeName = "lua.ecu";
    ecu.parameters.set("script",
                       ParameterValue::fromText("function on_message(id)\n"
                                                "    emit(0x101, \"\\1\\2\")\n"
                                                "end\n"));
    ecu.parameters.set("module_id", ParameterValue::fromInteger(0x18FEE500));
    ecu.parameters.set("reduction", ParameterValue::fromReal(0.125));
    ecu.parameters.set("verbose", ParameterValue::fromBoolean(true));
    ecu.parameters.set("label", ParameterValue::fromText("seed motor"));
    ecu.x = 80.0;
    ecu.y = 40.0;
    pipeline.addNode(ecu);

    NodeDescription trace;
    trace.id = "trace_1";
    trace.typeName = "trace.sink";
    trace.enabled = false; // The one node switched off.
    trace.x = 300.0;
    trace.y = 160.0;
    pipeline.addNode(trace);

    pipeline.addEdge(EdgeDescription{"can_1", 0, "ecu_motor", 0});
    pipeline.addEdge(EdgeDescription{"ecu_motor", 0, "trace_1", 0});

    return pipeline;
}

[[nodiscard]] QString pathIn(const QTemporaryDir& directory, const QString& name)
{
    return QDir{directory.path()}.filePath(name);
}

} // namespace

TEST_CASE("A pipeline survives a save and a load unchanged", "[project]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const GraphDescription original = richPipeline();
    const QString path = pathIn(directory, QStringLiteral("round-trip.tbsproj"));

    REQUIRE(ProjectFile::save(path, original).succeeded());

    GraphDescription reopened;
    const Result result = ProjectFile::load(path, reopened);

    INFO(std::string{result.message()});
    REQUIRE(result.succeeded());

    // The whole graph at once. Comparing field by field would pass on the day
    // someone adds a field to NodeDescription and forgets to write it.
    CHECK(reopened == original);
}

TEST_CASE("Saving twice produces byte-identical files", "[project]")
{
    // What makes a project file reviewable: opening a project and saving it
    // again, with nothing touched, must not produce a diff. That needs a stable
    // key order, which is why NodeParameters is an ordered map rather than a
    // hash.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const GraphDescription pipeline = richPipeline();

    const QString first = pathIn(directory, QStringLiteral("a.tbsproj"));
    const QString second = pathIn(directory, QStringLiteral("b.tbsproj"));

    REQUIRE(ProjectFile::save(first, pipeline).succeeded());

    GraphDescription reopened;
    REQUIRE(ProjectFile::load(first, reopened).succeeded());
    REQUIRE(ProjectFile::save(second, reopened).succeeded());

    QFile fileA{first};
    QFile fileB{second};
    REQUIRE(fileA.open(QIODevice::ReadOnly));
    REQUIRE(fileB.open(QIODevice::ReadOnly));

    CHECK(fileA.readAll() == fileB.readAll());
}

TEST_CASE("Parameter types survive the crossing", "[project]")
{
    // JSON has one number type, so an integer and a real are told apart on the
    // way back by whether the value is integral. A CAN identifier read back as
    // a real would be written out next time with a ".0" that was never there.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    GraphDescription pipeline;
    NodeDescription node;
    node.id = "n";
    node.typeName = "lua.ecu";
    node.parameters.set("count", ParameterValue::fromInteger(7));
    node.parameters.set("identifier", ParameterValue::fromInteger(0x18FEE500));
    node.parameters.set("factor", ParameterValue::fromReal(0.125));
    node.parameters.set("flag", ParameterValue::fromBoolean(false));
    node.parameters.set("text", ParameterValue::fromText("engine"));
    pipeline.addNode(node);

    const QString path = pathIn(directory, QStringLiteral("types.tbsproj"));
    REQUIRE(ProjectFile::save(path, pipeline).succeeded());

    GraphDescription reopened;
    REQUIRE(ProjectFile::load(path, reopened).succeeded());

    const NodeDescription* back = reopened.find("n");
    REQUIRE(back != nullptr);

    CHECK(back->parameters.values().at("count").type() == ParameterValue::Type::Integer);
    CHECK(back->parameters.integer("identifier") == 0x18FEE500);
    CHECK(back->parameters.values().at("factor").type() == ParameterValue::Type::Real);
    CHECK(back->parameters.real("factor") == 0.125);
    CHECK(back->parameters.values().at("flag").type() == ParameterValue::Type::Boolean);
    CHECK(back->parameters.boolean("flag") == false);
    CHECK(back->parameters.text("text") == "engine");
}

TEST_CASE("A file from a newer TorqueBus is refused, not half-read", "[project]")
{
    // Reading it anyway would drop whatever the newer version added, silently.
    // A project that opens with its wires missing is worse than one that
    // refuses to open.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QJsonObject root;
    root["version"] = ProjectFile::kFormatVersion + 1;
    root["pipeline"] = QJsonObject{};

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument{root}.toJson());
    file.close();

    GraphDescription pipeline;
    const Result result = ProjectFile::load(path, pipeline);

    REQUIRE(result.failed());
    CHECK(result.code() == ErrorCode::VersionMismatch);
}

TEST_CASE("A wire naming a missing block is refused", "[project]")
{
    // Otherwise the graph builds and fails at Start with a message about a node
    // the user cannot see anywhere.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("dangling.tbsproj"));

    QJsonObject node;
    node["id"] = "can_1";
    node["type"] = "can.source";

    QJsonObject edge;
    edge["from"] = "can_1";
    edge["fromPort"] = 0;
    edge["to"] = "gone";
    edge["toPort"] = 0;

    QJsonObject graph;
    graph["nodes"] = QJsonArray{node};
    graph["edges"] = QJsonArray{edge};

    QJsonObject root;
    root["version"] = ProjectFile::kFormatVersion;
    root["pipeline"] = graph;

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument{root}.toJson());
    file.close();

    GraphDescription pipeline;
    const Result result = ProjectFile::load(path, pipeline);

    REQUIRE(result.failed());
    CHECK(std::string{result.message()}.find("gone") != std::string::npos);
}

TEST_CASE("A failed load leaves the previous pipeline untouched", "[project]")
{
    // The canvas must not end up showing half a project that was never saved.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    GraphDescription pipeline = richPipeline();
    const GraphDescription before = pipeline;

    const QString path = pathIn(directory, QStringLiteral("broken.tbsproj"));
    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json");
    file.close();

    REQUIRE(ProjectFile::load(path, pipeline).failed());
    CHECK(pipeline == before);
}

TEST_CASE("Opening a file that is not there says so", "[project]")
{
    GraphDescription pipeline;
    const Result result =
        ProjectFile::load(QStringLiteral("no/such/project.tbsproj"), pipeline);

    REQUIRE(result.failed());
    CHECK(result.code() == ErrorCode::FileNotFound);
}

TEST_CASE("The saved file is readable JSON", "[project]")
{
    // Not a formatting preference: a project file is diffed in reviews and
    // hand-edited in labs. A single-line document would make both useless.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("readable.tbsproj"));
    REQUIRE(ProjectFile::save(path, richPipeline()).succeeded());

    QFile file{path};
    REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = file.readAll();

    CHECK(contents.count('\n') > 20);
    CHECK(contents.contains("\"ecu_motor\""));
    CHECK(contents.contains("\"lua.ecu\""));
}
