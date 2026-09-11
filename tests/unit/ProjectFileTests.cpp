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

#include "core/dashboard/DashboardDescription.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
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


/// A transmit list for the cases that are only about the pipeline.
///
/// Function-local static rather than a fresh one per call: TransmitList is not
/// copyable, and these cases neither read it nor care what is in it.
[[nodiscard]] TransmitList& scratch()
{
    static TransmitList list;
    list.clear();
    return list;
}

/// The same, for the dashboard, in the cases that are not about one.
///
/// A fresh one per call would do - DashboardDescription is copyable, unlike a
/// transmit list - but the two read alike at the call site this way, and a
/// reader should not have to work out why one is a reference and the other is
/// not.
[[nodiscard]] DashboardDescription& noDashboard()
{
    static DashboardDescription dashboard;
    dashboard.clear();
    return dashboard;
}

} // namespace

TEST_CASE("A pipeline survives a save and a load unchanged", "[project]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const GraphDescription original = richPipeline();
    const QString path = pathIn(directory, QStringLiteral("round-trip.tbsproj"));

    REQUIRE(ProjectFile::save(path, original, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    const Result result = ProjectFile::load(path, reopened, scratch(), noDashboard());

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

    REQUIRE(ProjectFile::save(first, pipeline, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    REQUIRE(ProjectFile::load(first, reopened, scratch(), noDashboard()).succeeded());
    REQUIRE(ProjectFile::save(second, reopened, scratch(), noDashboard()).succeeded());

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
    REQUIRE(ProjectFile::save(path, pipeline, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    REQUIRE(ProjectFile::load(path, reopened, scratch(), noDashboard()).succeeded());

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
    const Result result = ProjectFile::load(path, pipeline, scratch(), noDashboard());

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
    const Result result = ProjectFile::load(path, pipeline, scratch(), noDashboard());

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

    REQUIRE(ProjectFile::load(path, pipeline, scratch(), noDashboard()).failed());
    CHECK(pipeline == before);
}

TEST_CASE("Opening a file that is not there says so", "[project]")
{
    GraphDescription pipeline;
    const Result result =
        ProjectFile::load(QStringLiteral("no/such/project.tbsproj"), pipeline, scratch(), noDashboard());

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
    REQUIRE(ProjectFile::save(path, richPipeline(), scratch(), noDashboard()).succeeded());

    QFile file{path};
    REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = file.readAll();

    CHECK(contents.count('\n') > 20);
    CHECK(contents.contains("\"ecu_motor\""));
    CHECK(contents.contains("\"lua.ecu\""));
}

TEST_CASE("The shipped example project opens and validates", "[project][examples]")
{
    // An example that does not open is worse than no example: it is the first
    // thing a new user tries, and it is the one file in the repository whose
    // correctness nothing else checks. This is the check.
    const QString path =
        QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/virtual-vehicle.tbsproj");

    GraphDescription pipeline;
    const Result opened = ProjectFile::load(path, pipeline, scratch(), noDashboard());

    INFO(std::string{opened.message()});
    REQUIRE(opened.succeeded());

    // Not just parseable - buildable. Every type known, every port real, every
    // wire type-compatible, one edge per input.
    const Result valid = pipeline.validate(NodeCatalog::withBuiltinTypes());

    INFO(std::string{valid.message()});
    CHECK(valid.succeeded());

    // And it demonstrates what it claims to: a simulated ECU that transmits.
    const NodeDescription* ecu = pipeline.find("ecu_vehicle");
    REQUIRE(ecu != nullptr);
    CHECK(ecu->typeName == "lua.ecu");
    CHECK_FALSE(ecu->parameters.text("script").empty());
}

// ---------------------------------------------------------------------------
// The transmit list
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] TransmitEntry sampleRow()
{
    TransmitEntry entry;
    entry.name = "Speed, fast";
    entry.channel = 1;
    entry.frame.identifier = 0x18FEDF00;
    entry.frame.format = CanFrameFormat::Extended;
    entry.frame.length = 3;
    entry.frame.data[0] = 0x52;
    entry.frame.data[1] = 0x03;
    entry.frame.data[2] = 0xFF;
    entry.trigger = TransmitTrigger::Periodic;
    entry.cycleMs = 250;
    entry.enabled = false;
    entry.messageName = "VehicleSpeed";
    return entry;
}

} // namespace

TEST_CASE("A transmit list survives a save and a load unchanged", "[project][transmit]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    TransmitList original;
    (void)original.add(sampleRow());

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("transmit.tbsproj"));

    REQUIRE(ProjectFile::save(path, pipeline, original, noDashboard()).succeeded());

    GraphDescription reopenedPipeline;
    TransmitList reopened;
    const Result result = ProjectFile::load(path, reopenedPipeline, reopened, noDashboard());

    INFO(std::string{result.message()});
    REQUIRE(result.succeeded());
    REQUIRE(reopened.size() == 1);

    TransmitEntry row;
    REQUIRE(reopened.entryAt(0, row));

    const TransmitEntry expected = sampleRow();
    CHECK(row.name == expected.name);
    CHECK(row.channel == expected.channel);
    CHECK(row.frame.identifier == expected.frame.identifier);
    CHECK(row.frame.format == expected.frame.format);
    CHECK(row.frame.length == expected.frame.length);
    CHECK(row.frame.data[0] == 0x52);
    CHECK(row.frame.data[2] == 0xFF);
    CHECK(row.trigger == expected.trigger);
    CHECK(row.cycleMs == expected.cycleMs);
    CHECK(row.enabled == expected.enabled);
    CHECK(row.messageName == expected.messageName);
}

TEST_CASE("A saved project does not claim a row has already been sent",
          "[project][transmit]")
{
    // sentCount and lastSentUs belong to a run, not to a project. Saving them
    // would mean opening a file that says a row has gone out forty times before
    // the measurement has started.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    TransmitList original;
    const std::size_t index = original.add(sampleRow());

    // Give the row some history to lose.
    TransmitEntry row;
    REQUIRE(original.entryAt(index, row));
    row.enabled = true;
    row.trigger = TransmitTrigger::Periodic;
    original.update(index, row);

    std::vector<CanFrame> out;
    original.collectDue(0, 1, out);
    original.collectDue(1'000'000, 1, out);
    REQUIRE(out.size() == 2);

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("counters.tbsproj"));
    REQUIRE(ProjectFile::save(path, pipeline, original, noDashboard()).succeeded());

    GraphDescription reopenedPipeline;
    TransmitList reopened;
    REQUIRE(ProjectFile::load(path, reopenedPipeline, reopened, noDashboard()).succeeded());

    TransmitEntry loaded;
    REQUIRE(reopened.entryAt(0, loaded));
    CHECK(loaded.sentCount == 0);
    CHECK(loaded.lastSentUs == 0);
}

TEST_CASE("A project written before the transmit list still opens",
          "[project][transmit]")
{
    // Every project saved in format 1 has no transmit section. A build that
    // refused those would make the version check pointless - it only exists to
    // stop a *newer* file being opened by an older reader.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("old.tbsproj"));

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "application": "TorqueBus Studio",
        "version": 1,
        "pipeline": { "nodes": [], "edges": [] }
    })");
    file.close();

    GraphDescription pipeline;
    TransmitList transmit;
    (void)transmit.add(sampleRow());

    REQUIRE(ProjectFile::load(path, pipeline, transmit, noDashboard()).succeeded());

    // And the list is replaced, not merged: opening a project means opening its
    // transmit list, which in this case is an empty one.
    CHECK(transmit.size() == 0);
}

TEST_CASE("An unknown trigger word does not start transmitting",
          "[project][transmit]")
{
    // A file from a future version might name a trigger this build has never
    // heard of. Defaulting it to periodic would put traffic on a bus because
    // the reader did not understand a word.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "application": "TorqueBus Studio",
        "version": 2,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [
            { "name": "odd", "id": 256, "trigger": "onEveryFullMoon", "cycleMs": 10 }
        ]
    })");
    file.close();

    GraphDescription pipeline;
    TransmitList transmit;
    REQUIRE(ProjectFile::load(path, pipeline, transmit, noDashboard()).succeeded());

    TransmitEntry row;
    REQUIRE(transmit.entryAt(0, row));
    CHECK_FALSE(row.isPeriodic());

    std::vector<CanFrame> out;
    transmit.collectDue(0, 0, out);
    transmit.collectDue(1'000'000, 0, out);
    CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// The dashboard
// ---------------------------------------------------------------------------

TEST_CASE("A dashboard survives a save and a load unchanged", "[project][dashboard]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    DashboardDescription original;
    original.setName("Powertrain");

    DashboardWidget gauge;
    gauge.id = "speed";
    gauge.kind = DashboardWidgetKind::Gauge;
    gauge.binding.source = DashboardBinding::Source::Signal;
    gauge.binding.message = "Engine";
    gauge.binding.signal = "EngineSpeed";
    gauge.title = "Engine speed";
    gauge.unit = "rpm";
    gauge.x = 20.5;
    gauge.y = -8.0;
    gauge.width = 240.0;
    gauge.height = 200.0;
    gauge.minimum = 0.0;
    gauge.maximum = 8000.0;
    gauge.decimals = 0;
    original.add(gauge);

    DashboardWidget slider;
    slider.id = "pedal";
    slider.kind = DashboardWidgetKind::Slider;
    slider.binding.source = DashboardBinding::Source::Variable;
    slider.binding.variable = "brake_pedal";
    slider.minimum = 0.0;
    slider.maximum = 1.0;
    slider.threshold = 0.25;
    original.add(slider);

    DashboardWidget label;
    label.id = "heading";
    label.kind = DashboardWidgetKind::Label;
    label.title = "Bench 2";
    original.add(label);

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("dash.tbsproj"));

    REQUIRE(ProjectFile::save(path, pipeline, scratch(), original).succeeded());

    GraphDescription reopenedPipeline;
    DashboardDescription reopened;

    const Result result =
        ProjectFile::load(path, reopenedPipeline, scratch(), reopened);

    INFO(std::string{result.message()});
    REQUIRE(result.succeeded());

    // Whole objects, not fields: a property added to a widget and forgotten in
    // the writer is exactly what this catches, and a field-by-field comparison
    // would not.
    CHECK(reopened == original);
}

TEST_CASE("A project from before dashboards opens with an empty one",
          "[project][dashboard]")
{
    // Every project written before format 3. Refusing those would make the
    // version field pointless.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("old.tbsproj"));

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "version": 2,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": []
    })");
    file.close();

    GraphDescription pipeline;
    DashboardDescription dashboard;

    REQUIRE(ProjectFile::load(path, pipeline, scratch(), dashboard).succeeded());
    CHECK(dashboard.empty());
}

TEST_CASE("A widget kind this build does not have refuses the file",
          "[project][dashboard]")
{
    // Drawing something else in its place would be a lie about what the file
    // contains - and the next save would write that lie back.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [],
        "dashboard": { "widgets": [ { "id": "x", "kind": "hologram" } ] }
    })");
    file.close();

    GraphDescription pipeline;
    DashboardDescription dashboard;

    const Result result = ProjectFile::load(path, pipeline, scratch(), dashboard);

    REQUIRE(result.failed());
    INFO(std::string{result.message()});
    CHECK(std::string{result.message()}.find("hologram") != std::string::npos);
}

TEST_CASE("A dashboard that could not be drawn refuses the file",
          "[project][dashboard]")
{
    // Validated before anything is assigned, so the alternative - opening the
    // project, showing an empty panel, and writing it back that way - cannot
    // happen.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("broken.tbsproj"));

    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [],
        "dashboard": { "widgets": [
            { "id": "g", "kind": "gauge", "binding": {},
              "minimum": 0, "maximum": 100 }
        ] }
    })");
    file.close();

    GraphDescription pipeline;
    DashboardDescription dashboard;

    const Result result = ProjectFile::load(path, pipeline, scratch(), dashboard);

    REQUIRE(result.failed());
    CHECK(dashboard.empty());
}
