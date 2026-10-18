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

#include <gtest/gtest.h>

#include "core/dashboard/DashboardDescription.h"
#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/transmit/TransmitList.h"
#include "services/ProjectFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    ecu.parameters.set("gear_ratio", ParameterValue::fromReal(0.125));
    ecu.parameters.set("verbose", ParameterValue::fromBoolean(true));
    ecu.parameters.set("label", ParameterValue::fromText("engine ecu"));
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

TEST(ProjectFileTests, APipelineSurvivesASaveAndALoadUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const GraphDescription original = richPipeline();
    const QString path = pathIn(directory, QStringLiteral("round-trip.tbsproj"));

    ASSERT_TRUE(ProjectFile::save(path, original, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    const Result result = ProjectFile::load(path, reopened, scratch(), noDashboard());

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());

    // The whole graph at once. Comparing field by field would pass on the day
    // someone adds a field to NodeDescription and forgets to write it.
    EXPECT_TRUE(reopened == original);
}

TEST(ProjectFileTests, ABusAndAWireLimitedToOneFrameFormatSurviveASaveAndALoad)
{
    // The two things the CAN Bus added to what a project holds: a block with an
    // input that takes any number of wires, and a wire that says which frames it
    // carries. The round trip is compared with ==, so a field the file forgot to
    // write shows up as a difference and not as a missing assertion.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription original;

    NodeDescription bus;
    bus.id = "can_1";
    bus.typeName = "can.bus";
    bus.parameters.set("channel", ParameterValue::fromInteger(0));
    bus.x = 12.5;
    bus.y = -30.0;
    original.addNode(bus);

    NodeDescription decoder;
    decoder.id = "j1939";
    decoder.typeName = "j1939.decoder";
    original.addNode(decoder);

    NodeDescription trace;
    trace.id = "trace_1";
    trace.typeName = "trace.sink";
    original.addNode(trace);

    EdgeDescription extended{"can_1", 0, "j1939", 0};
    extended.frameFormat = CanFormatMatch::ExtendedOnly;
    original.addEdge(extended);

    EdgeDescription standard{"can_1", 0, "trace_1", 0};
    standard.frameFormat = CanFormatMatch::StandardOnly;
    original.addEdge(standard);

    const QString path = pathIn(directory, QStringLiteral("bus.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, original, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    const Result result = ProjectFile::load(path, reopened, scratch(), noDashboard());

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(reopened == original);

    // Written in words a person reading the file can follow.
    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = file.readAll();
    EXPECT_TRUE(contents.contains("\"can.bus\""));
    EXPECT_TRUE(contents.contains("\"29-bit\""));
    EXPECT_TRUE(contents.contains("\"11-bit\""));
}

TEST(ProjectFileTests, AWireThatCarriesEveryFrameWritesNothingAboutFormat)
{
    // So a project that never limits a wire is what it was before the field
    // existed, and opening and saving one does not make a diff.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("plain.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, richPipeline(), scratch(), noDashboard()).succeeded());

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_FALSE(file.readAll().contains("\"frames\""));
}

TEST(ProjectFileTests, AFrameFormatThisVersionDoesNotKnowIsRefusedNotReadAsEveryFrame)
{
    // A newer version limited the wire further than this one can read. Opening
    // it as unlimited would carry more than the wire was drawn to, and say
    // nothing.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription original;
    NodeDescription bus;
    bus.id = "can_1";
    bus.typeName = "can.bus";
    original.addNode(bus);
    NodeDescription trace;
    trace.id = "trace_1";
    trace.typeName = "trace.sink";
    original.addNode(trace);
    original.addEdge(EdgeDescription{"can_1", 0, "trace_1", 0});

    const QString path = pathIn(directory, QStringLiteral("future-frames.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, original, scratch(), noDashboard()).succeeded());

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    QJsonObject pipeline = root.value(QStringLiteral("pipeline")).toObject();
    QJsonArray edges = pipeline.value(QStringLiteral("edges")).toArray();
    ASSERT_EQ(edges.size(), 1);
    QJsonObject edge = edges.at(0).toObject();
    edge[QStringLiteral("frames")] = QStringLiteral("j1939-only");
    edges.replace(0, edge);
    pipeline[QStringLiteral("edges")] = edges;
    root[QStringLiteral("pipeline")] = pipeline;

    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument{root}.toJson());
    file.close();

    GraphDescription reopened;
    const Result result = ProjectFile::load(path, reopened, scratch(), noDashboard());

    ASSERT_TRUE(result.failed());
    const std::string message{result.message()};
    EXPECT_TRUE(message.find("can_1") != std::string::npos);
    EXPECT_TRUE(message.find("trace_1") != std::string::npos);
    EXPECT_TRUE(message.find("j1939-only") != std::string::npos);
}

TEST(ProjectFileTests, SavingTwiceProducesByteIdenticalFiles)
{
    // What makes a project file reviewable: opening a project and saving it
    // again, with nothing touched, must not produce a diff. That needs a stable
    // key order, which is why NodeParameters is an ordered map rather than a
    // hash.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const GraphDescription pipeline = richPipeline();

    const QString first = pathIn(directory, QStringLiteral("a.tbsproj"));
    const QString second = pathIn(directory, QStringLiteral("b.tbsproj"));

    ASSERT_TRUE(ProjectFile::save(first, pipeline, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    ASSERT_TRUE(ProjectFile::load(first, reopened, scratch(), noDashboard()).succeeded());
    ASSERT_TRUE(ProjectFile::save(second, reopened, scratch(), noDashboard()).succeeded());

    QFile fileA{first};
    QFile fileB{second};
    ASSERT_TRUE(fileA.open(QIODevice::ReadOnly));
    ASSERT_TRUE(fileB.open(QIODevice::ReadOnly));

    EXPECT_TRUE(fileA.readAll() == fileB.readAll());
}

TEST(ProjectFileTests, ParameterTypesSurviveTheCrossing)
{
    // JSON has one number type, so an integer and a real are told apart on the
    // way back by whether the value is integral. A CAN identifier read back as
    // a real would be written out next time with a ".0" that was never there.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

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
    ASSERT_TRUE(ProjectFile::save(path, pipeline, scratch(), noDashboard()).succeeded());

    GraphDescription reopened;
    ASSERT_TRUE(ProjectFile::load(path, reopened, scratch(), noDashboard()).succeeded());

    const NodeDescription* back = reopened.find("n");
    ASSERT_TRUE(back != nullptr);

    EXPECT_TRUE(back->parameters.values().at("count").type() == ParameterValue::Type::Integer);
    EXPECT_TRUE(back->parameters.integer("identifier") == 0x18FEE500);
    EXPECT_TRUE(back->parameters.values().at("factor").type() == ParameterValue::Type::Real);
    EXPECT_TRUE(back->parameters.real("factor") == 0.125);
    EXPECT_TRUE(back->parameters.values().at("flag").type() == ParameterValue::Type::Boolean);
    EXPECT_TRUE(back->parameters.boolean("flag") == false);
    EXPECT_TRUE(back->parameters.text("text") == "engine");
}

TEST(ProjectFileTests, AFileFromANewerTorqueBusIsRefusedNotHalfRead)
{
    // Reading it anyway would drop whatever the newer version added, silently.
    // A project that opens with its wires missing is worse than one that
    // refuses to open.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QJsonObject root;
    root["version"] = ProjectFile::kFormatVersion + 1;
    root["pipeline"] = QJsonObject{};

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument{root}.toJson());
    file.close();

    GraphDescription pipeline;
    const Result result = ProjectFile::load(path, pipeline, scratch(), noDashboard());

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::VersionMismatch);
}

TEST(ProjectFileTests, AWireNamingAMissingBlockIsRefused)
{
    // Otherwise the graph builds and fails at Start with a message about a node
    // the user cannot see anywhere.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

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
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument{root}.toJson());
    file.close();

    GraphDescription pipeline;
    const Result result = ProjectFile::load(path, pipeline, scratch(), noDashboard());

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(std::string{result.message()}.find("gone") != std::string::npos);
}

TEST(ProjectFileTests, AFailedLoadLeavesThePreviousPipelineUntouched)
{
    // The canvas must not end up showing half a project that was never saved.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline = richPipeline();
    const GraphDescription before = pipeline;

    const QString path = pathIn(directory, QStringLiteral("broken.tbsproj"));
    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json");
    file.close();

    ASSERT_TRUE(ProjectFile::load(path, pipeline, scratch(), noDashboard()).failed());
    EXPECT_TRUE(pipeline == before);
}

TEST(ProjectFileTests, OpeningAFileThatIsNotThereSaysSo)
{
    GraphDescription pipeline;
    const Result result = ProjectFile::load(
        QStringLiteral("no/such/project.tbsproj"), pipeline, scratch(), noDashboard());

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(result.code() == ErrorCode::FileNotFound);
}

TEST(ProjectFileTests, TheSavedFileIsReadableJSON)
{
    // Not a formatting preference: a project file is diffed in reviews and
    // hand-edited in labs. A single-line document would make both useless.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("readable.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, richPipeline(), scratch(), noDashboard()).succeeded());

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = file.readAll();

    EXPECT_TRUE(contents.count('\n') > 20);
    EXPECT_TRUE(contents.contains("\"ecu_motor\""));
    EXPECT_TRUE(contents.contains("\"lua.ecu\""));
}

TEST(ProjectFileTests, TheShippedExampleProjectOpensAndValidates)
{
    // An example that does not open is worse than no example: it is the first
    // thing a new user tries, and it is the one file in the repository whose
    // correctness nothing else checks. This is the check.
    const QString path = QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/virtual-vehicle.tbsproj");

    GraphDescription pipeline;
    const Result opened = ProjectFile::load(path, pipeline, scratch(), noDashboard());

    SCOPED_TRACE(::testing::Message() << std::string{opened.message()});
    ASSERT_TRUE(opened.succeeded());

    // Not just parseable - buildable. Every type known, every port real, every
    // wire type-compatible, one edge per input.
    const Result valid = pipeline.validate(NodeCatalog::withBuiltinTypes());

    SCOPED_TRACE(::testing::Message() << std::string{valid.message()});
    EXPECT_TRUE(valid.succeeded());

    // And it demonstrates what it claims to: a simulated ECU that transmits.
    const NodeDescription* ecu = pipeline.find("ecu_vehicle");
    ASSERT_TRUE(ecu != nullptr);
    EXPECT_TRUE(ecu->typeName == "lua.ecu");
    EXPECT_FALSE(ecu->parameters.text("script").empty());
}

TEST(ProjectFileTests, TheShippedExampleNamesItsDatabasesRelativeToItself)
{
    // The example is opened on whatever machine somebody clones the repository to. A database
    // named by the path it had on the machine that saved the project is a file that is not there on
    // any other, and opening the project says so with a dialog before anything else is seen.
    //
    // The rule is the one MainWindow applies when it opens a project: a relative path is relative
    // to the folder of the project file.
    const QString path = QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/virtual-vehicle.tbsproj");

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));

    const QJsonArray databases = QJsonDocument::fromJson(file.readAll())
                                     .object()
                                     .value(QStringLiteral("databases"))
                                     .toArray();
    ASSERT_FALSE(databases.isEmpty());

    const QDir folder = QFileInfo{path}.absoluteDir();

    for (const QJsonValue& entry : databases) {
        const QString name = entry.toString();
        SCOPED_TRACE(name.toStdString());

        EXPECT_FALSE(QFileInfo{name}.isAbsolute());
        EXPECT_TRUE(QFileInfo{folder.filePath(name)}.isFile());
    }
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

TEST(ProjectFileTests, ATransmitListSurvivesASaveAndALoadUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TransmitList original;
    (void)original.add(sampleRow());

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("transmit.tbsproj"));

    ASSERT_TRUE(ProjectFile::save(path, pipeline, original, noDashboard()).succeeded());

    GraphDescription reopenedPipeline;
    TransmitList reopened;
    const Result result = ProjectFile::load(path, reopenedPipeline, reopened, noDashboard());

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());
    ASSERT_TRUE(reopened.size() == 1);

    TransmitEntry row;
    ASSERT_TRUE(reopened.entryAt(0, row));

    const TransmitEntry expected = sampleRow();
    EXPECT_TRUE(row.name == expected.name);
    EXPECT_TRUE(row.channel == expected.channel);
    EXPECT_TRUE(row.frame.identifier == expected.frame.identifier);
    EXPECT_TRUE(row.frame.format == expected.frame.format);
    EXPECT_TRUE(row.frame.length == expected.frame.length);
    EXPECT_TRUE(row.frame.data[0] == 0x52);
    EXPECT_TRUE(row.frame.data[2] == 0xFF);
    EXPECT_TRUE(row.trigger == expected.trigger);
    EXPECT_TRUE(row.cycleMs == expected.cycleMs);
    EXPECT_TRUE(row.enabled == expected.enabled);
    EXPECT_TRUE(row.messageName == expected.messageName);
}

TEST(ProjectFileTests, ASavedProjectDoesNotClaimARowHasAlreadyBeenSent)
{
    // sentCount and lastSentUs belong to a run, not to a project. Saving them
    // would mean opening a file that says a row has gone out forty times before
    // the measurement has started.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    TransmitList original;
    const std::size_t index = original.add(sampleRow());

    // Give the row some history to lose.
    TransmitEntry row;
    ASSERT_TRUE(original.entryAt(index, row));
    row.enabled = true;
    row.trigger = TransmitTrigger::Periodic;
    original.update(index, row);

    std::vector<CanFrame> out;
    original.collectDue(0, 1, out);
    original.collectDue(1'000'000, 1, out);
    ASSERT_TRUE(out.size() == 2);

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("counters.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, pipeline, original, noDashboard()).succeeded());

    GraphDescription reopenedPipeline;
    TransmitList reopened;
    ASSERT_TRUE(ProjectFile::load(path, reopenedPipeline, reopened, noDashboard()).succeeded());

    TransmitEntry loaded;
    ASSERT_TRUE(reopened.entryAt(0, loaded));
    EXPECT_TRUE(loaded.sentCount == 0);
    EXPECT_TRUE(loaded.lastSentUs == 0);
}

TEST(ProjectFileTests, AProjectWrittenBeforeTheTransmitListStillOpens)
{
    // Every project saved in format 1 has no transmit section. A build that
    // refused those would make the version check pointless - it only exists to
    // stop a *newer* file being opened by an older reader.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("old.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "application": "TorqueBus Studio",
        "version": 1,
        "pipeline": { "nodes": [], "edges": [] }
    })");
    file.close();

    GraphDescription pipeline;
    TransmitList transmit;
    (void)transmit.add(sampleRow());

    ASSERT_TRUE(ProjectFile::load(path, pipeline, transmit, noDashboard()).succeeded());

    // And the list is replaced, not merged: opening a project means opening its
    // transmit list, which in this case is an empty one.
    EXPECT_TRUE(transmit.size() == 0);
}

TEST(ProjectFileTests, AnUnknownTriggerWordDoesNotStartTransmitting)
{
    // A file from a future version might name a trigger this build has never
    // heard of. Defaulting it to periodic would put traffic on a bus because
    // the reader did not understand a word.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
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
    ASSERT_TRUE(ProjectFile::load(path, pipeline, transmit, noDashboard()).succeeded());

    TransmitEntry row;
    ASSERT_TRUE(transmit.entryAt(0, row));
    EXPECT_FALSE(row.isPeriodic());

    std::vector<CanFrame> out;
    transmit.collectDue(0, 0, out);
    transmit.collectDue(1'000'000, 0, out);
    EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// The dashboard
// ---------------------------------------------------------------------------

TEST(ProjectFileTests, ADashboardSurvivesASaveAndALoadUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

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

    DashboardWidget cluster;
    cluster.id = "cluster";
    cluster.kind = DashboardWidgetKind::Cluster;
    cluster.profile = "example-11bit";
    cluster.width = 720.0;
    cluster.height = 300.0;
    original.add(cluster);

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("dash.tbsproj"));

    ASSERT_TRUE(ProjectFile::save(path, pipeline, scratch(), original).succeeded());

    GraphDescription reopenedPipeline;
    DashboardDescription reopened;

    const Result result = ProjectFile::load(path, reopenedPipeline, scratch(), reopened);

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());

    // Whole objects, not fields: a property added to a widget and forgotten in
    // the writer is exactly what this catches, and a field-by-field comparison
    // would not.
    EXPECT_TRUE(reopened == original);
}

namespace {

/// A project with nothing in it but the dashboard widgets given, as JSON.
void writeDashboardProject(const QString& path, const QByteArray& widgetsJson)
{
    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));

    file.write(R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [],
        "dashboard": { "widgets": )");
    file.write(widgetsJson);
    file.write(" } }");
    file.close();
}

} // namespace

TEST(ProjectFileTests, TheFieldsOfTheNewerWidgetsSurviveASaveAndALoad)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    DashboardDescription original;

    // A state with names, tones, a picture for one of them, and a time after which it is stale.
    DashboardWidget gear = defaultWidgetOf(DashboardWidgetKind::State);
    gear.id = "gear";
    gear.binding.source = DashboardBinding::Source::Signal;
    gear.binding.message = "ETC2";
    gear.binding.signal = "TransmissionCurrentGear";
    gear.decimals = 0;
    gear.staleMs = 300;

    DashboardState reverse;
    reverse.value = -1.0;
    reverse.label = "Reverse";
    reverse.tone = DashboardTone::Warning;
    reverse.image = "pictures/reverse.png";

    DashboardState neutral;
    neutral.value = 0.0;
    neutral.label = "Neutral";

    gear.states = {reverse, neutral};
    original.add(gear);

    // A bar with all four limits.
    DashboardWidget coolant = defaultWidgetOf(DashboardWidgetKind::Bar);
    coolant.id = "coolant";
    coolant.binding.source = DashboardBinding::Source::Signal;
    coolant.binding.message = "ET1";
    coolant.binding.signal = "EngineCoolantTemperature";
    coolant.minimum = -40.0;
    coolant.maximum = 210.0;
    coolant.limits.warningBelow = 60.0;
    coolant.limits.warningAbove = 100.0;
    coolant.limits.criticalBelow = 40.0;
    coolant.limits.criticalAbove = 110.5;
    original.add(coolant);

    // A trend that is not thirty seconds, and a table bound to a message and no signal.
    DashboardWidget trend = defaultWidgetOf(DashboardWidgetKind::Trend);
    trend.id = "rpm";
    trend.binding.source = DashboardBinding::Source::Variable;
    trend.binding.variable = "rpm";
    trend.windowSeconds = 120.0;
    original.add(trend);

    DashboardWidget table = defaultWidgetOf(DashboardWidgetKind::Table);
    table.id = "eec1";
    table.binding.source = DashboardBinding::Source::Message;
    table.binding.message = "EEC1";
    table.title = "Engine";
    original.add(table);

    // A picture with a name that is not ASCII, a lamp with one, and a group.
    DashboardWidget logo = defaultWidgetOf(DashboardWidgetKind::Image);
    logo.id = "logo";
    logo.image = "imagens/logotipo-ação.png";
    original.add(logo);

    DashboardWidget lamp = defaultWidgetOf(DashboardWidgetKind::Lamp);
    lamp.id = "door";
    lamp.binding.source = DashboardBinding::Source::Signal;
    lamp.binding.message = "Doors";
    lamp.binding.signal = "DoorState";
    lamp.image = "../icons/door.png";
    original.add(lamp);

    DashboardWidget group = defaultWidgetOf(DashboardWidgetKind::Group);
    group.id = "powertrain";
    group.title = "Powertrain";
    original.add(group);

    ASSERT_TRUE(original.validate().succeeded());

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("newer.tbsproj"));

    ASSERT_TRUE(ProjectFile::save(path, pipeline, scratch(), original).succeeded());

    GraphDescription reopenedPipeline;
    DashboardDescription reopened;

    const Result result = ProjectFile::load(path, reopenedPipeline, scratch(), reopened);

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());

    EXPECT_TRUE(reopened == original);
}

TEST(ProjectFileTests, ADashboardThatUsesNothingNewIsWrittenAsItWasBefore)
{
    // The fields of the newer widgets are written only when they are used, so a project that has
    // none is the file it always was - which an older build can still read without dropping
    // anything on its next save.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    DashboardDescription dashboard;

    DashboardWidget gauge = defaultWidgetOf(DashboardWidgetKind::Gauge);
    gauge.id = "speed";
    gauge.binding.source = DashboardBinding::Source::Signal;
    gauge.binding.message = "Engine";
    gauge.binding.signal = "EngineSpeed";
    dashboard.add(gauge);

    GraphDescription pipeline;
    const QString path = pathIn(directory, QStringLiteral("plain.tbsproj"));
    ASSERT_TRUE(ProjectFile::save(path, pipeline, scratch(), dashboard).succeeded());

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray widgets = root.value(QStringLiteral("dashboard"))
                                   .toObject()
                                   .value(QStringLiteral("widgets"))
                                   .toArray();
    ASSERT_EQ(widgets.size(), 1);

    const QJsonObject written = widgets.at(0).toObject();

    for (const char* key : {"image", "states", "limits", "windowSeconds", "staleMs"}) {
        EXPECT_FALSE(written.contains(QLatin1String{key})) << key;
    }
}

TEST(ProjectFileTests, AToneThisBuildDoesNotKnowIsReadAsNoOpinionAndALimitMayStandAlone)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("tones.tbsproj"));

    writeDashboardProject(path, R"([
        { "id": "mode", "kind": "state",
          "binding": { "source": "variable", "variable": "mode" },
          "states": [ { "value": 0, "label": "Quiet", "tone": "sparkly" },
                      { "value": 1, "label": "Loud", "tone": "danger" } ] },
        { "id": "oil", "kind": "bar",
          "binding": { "source": "variable", "variable": "oil" },
          "minimum": 0, "maximum": 1000,
          "limits": { "criticalBelow": 100 } }
    ])");

    GraphDescription pipeline;
    DashboardDescription dashboard;

    const Result result = ProjectFile::load(path, pipeline, scratch(), dashboard);

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());

    const DashboardWidget* mode = dashboard.find("mode");
    ASSERT_NE(mode, nullptr);
    ASSERT_EQ(mode->states.size(), 2U);
    EXPECT_EQ(mode->states[0].tone, DashboardTone::Neutral);
    EXPECT_EQ(mode->states[1].tone, DashboardTone::Danger);

    const DashboardWidget* oil = dashboard.find("oil");
    ASSERT_NE(oil, nullptr);
    EXPECT_FALSE(oil->limits.warningBelow.has_value());
    EXPECT_FALSE(oil->limits.warningAbove.has_value());
    ASSERT_TRUE(oil->limits.criticalBelow.has_value());
    EXPECT_DOUBLE_EQ(*oil->limits.criticalBelow, 100.0);
    EXPECT_FALSE(oil->limits.criticalAbove.has_value());
}

TEST(ProjectFileTests, ANewerWidgetThatCouldNotBeDrawnRefusesTheFileLikeAnyOther)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("refused.tbsproj"));

    GraphDescription pipeline;

    // A table of nothing, a gauge bound to a whole message, two names for one value, a stale time
    // that does not fit in a count, and limits that contradict each other.
    const char* const broken[] = {
        R"([ { "id": "t", "kind": "table", "binding": {} } ])",
        R"([ { "id": "g", "kind": "gauge", "minimum": 0, "maximum": 10,
               "binding": { "source": "message", "message": "EEC1" } } ])",
        R"([ { "id": "s", "kind": "state", "binding": { "source": "variable", "variable": "v" },
               "states": [ { "value": 1, "label": "A" }, { "value": 1, "label": "B" } ] } ])",
        R"([ { "id": "g", "kind": "gauge", "minimum": 0, "maximum": 10, "staleMs": 99999999999,
               "binding": { "source": "variable", "variable": "v" } } ])",
        R"([ { "id": "b", "kind": "bar", "minimum": 0, "maximum": 10,
               "binding": { "source": "variable", "variable": "v" },
               "limits": { "warningBelow": 8, "warningAbove": 2 } } ])",
    };

    for (const char* widgets : broken) {
        writeDashboardProject(path, QByteArray{widgets});

        DashboardDescription dashboard;
        const Result result = ProjectFile::load(path, pipeline, scratch(), dashboard);

        SCOPED_TRACE(widgets);
        EXPECT_TRUE(result.failed());
        EXPECT_TRUE(dashboard.empty());
    }
}

TEST(ProjectFileTests, AProjectFromBeforeDashboardsOpensWithAnEmptyOne)
{
    // Every project written before format 3. Refusing those would make the
    // version field pointless.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("old.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "version": 2,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": []
    })");
    file.close();

    GraphDescription pipeline;
    DashboardDescription dashboard;

    ASSERT_TRUE(ProjectFile::load(path, pipeline, scratch(), dashboard).succeeded());
    EXPECT_TRUE(dashboard.empty());
}

TEST(ProjectFileTests, AWidgetKindThisBuildDoesNotHaveRefusesTheFile)
{
    // Drawing something else in its place would be a lie about what the file
    // contains - and the next save would write that lie back.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
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

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("hologram") != std::string::npos);
}

TEST(ProjectFileTests, AClusterFedByAProfileThisBuildDoesNotHaveRefusesTheFile)
{
    // The cluster's equivalent of a widget kind from a newer version: the profile is what says
    // where every value on it comes from, and a cluster that falls back to another profile shows
    // another vehicle's numbers as if they were this one's.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("future-profile.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [],
        "dashboard": { "widgets": [ { "id": "cluster", "kind": "cluster", "profile": "from-the-future",
                                      "width": 720, "height": 300 } ] }
    })");
    file.close();

    GraphDescription pipeline;
    DashboardDescription dashboard;

    const Result result = ProjectFile::load(path, pipeline, scratch(), dashboard);

    ASSERT_TRUE(result.failed());
    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    EXPECT_TRUE(std::string{result.message()}.find("from-the-future") != std::string::npos);
    EXPECT_TRUE(dashboard.empty());
}

TEST(ProjectFileTests, ADashboardThatCouldNotBeDrawnRefusesTheFile)
{
    // Validated before anything is assigned, so the alternative - opening the
    // project, showing an empty panel, and writing it back that way - cannot
    // happen.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("broken.tbsproj"));

    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
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

    ASSERT_TRUE(result.failed());
    EXPECT_TRUE(dashboard.empty());
}

TEST(ProjectFileTests, DatabasesSurviveASaveAndALoadUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = pathIn(directory, QStringLiteral("databases.tbsproj"));
    const QStringList originalDatabases{
        QStringLiteral("powertrain.dbc"),
        QStringLiteral("networks/chassis_can.dbc"),
    };

    GraphDescription pipeline;
    ASSERT_TRUE(
        ProjectFile::save(path, pipeline, scratch(), noDashboard(), originalDatabases).succeeded());

    GraphDescription reopenedPipeline;
    QStringList reopenedDatabases;
    const Result result =
        ProjectFile::load(path, reopenedPipeline, scratch(), noDashboard(), &reopenedDatabases);

    SCOPED_TRACE(::testing::Message() << std::string{result.message()});
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(reopenedDatabases, originalDatabases);
}

// ---------------------------------------------------------------------------
// A file that was edited by hand, or by something else
// ---------------------------------------------------------------------------

namespace {

/// Writes `json` to a project file in `directory` and loads it, returning the result.
[[nodiscard]] Result loadText(const QTemporaryDir& directory,
                              const char* json,
                              GraphDescription& pipeline,
                              TransmitList& transmit)
{
    const QString path = pathIn(directory, QStringLiteral("edited.tbsproj"));

    QFile file{path};
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(json);
    file.close();

    DashboardDescription dashboard;
    return ProjectFile::load(path, pipeline, transmit, dashboard);
}

} // namespace

TEST(ProjectFileTests, ANumberTooLargeForAnIntegerIsKeptAsARealAndNotCastIntoGarbage)
{
    // 1e300 is valid JSON. Converting a double outside the range of an int64 is
    // undefined, and the conversion ran before the comparison that was meant to guard it.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline;
    TransmitList transmit;

    const Result result = loadText(directory,
                                   R"({
        "version": 3,
        "pipeline": { "nodes": [
            { "id": "a", "type": "can.source", "parameters": { "channel": 1e300, "other": -1e300, "ok": 7 } }
        ], "edges": [] },
        "transmit": []
    })",
                                   pipeline,
                                   transmit);

    ASSERT_TRUE(result.succeeded()) << result.message();

    const NodeDescription* node = pipeline.find("a");
    ASSERT_NE(node, nullptr);

    EXPECT_EQ(node->parameters.values().at("channel").type(), ParameterValue::Type::Real);
    EXPECT_EQ(node->parameters.values().at("other").type(), ParameterValue::Type::Real);
    EXPECT_EQ(node->parameters.values().at("ok").type(), ParameterValue::Type::Integer);

    // And a graph built from it says what is wrong with the channel instead of using a
    // channel nobody typed.
    EXPECT_TRUE(pipeline.validate(NodeCatalog::withBuiltinTypes()).failed());
}

TEST(ProjectFileTests, TheLargestExactIntegerStillComesBackAsAnInteger)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline;
    TransmitList transmit;

    // 2^53 is the last point at which a double holds every integer, and a project file
    // goes through one.
    const Result result = loadText(directory,
                                   R"({
        "version": 3,
        "pipeline": { "nodes": [
            { "id": "a", "type": "can.source", "parameters": { "big": 9007199254740992, "neg": -4096 } }
        ], "edges": [] },
        "transmit": []
    })",
                                   pipeline,
                                   transmit);

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_EQ(pipeline.find("a")->parameters.integer("big", 0), 9007199254740992LL);
    EXPECT_EQ(pipeline.find("a")->parameters.integer("neg", 0), -4096);
}

TEST(ProjectFileTests, TwoNodesWithTheSameIdRefuseTheFileAndNameTheId)
{
    // An id is how a wire names its ends. With two, a wire goes to either.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline;
    TransmitList transmit;

    const Result result = loadText(directory,
                                   R"({
        "version": 3,
        "pipeline": { "nodes": [
            { "id": "dup", "type": "can.source" },
            { "id": "dup", "type": "trace.sink" }
        ], "edges": [] },
        "transmit": []
    })",
                                   pipeline,
                                   transmit);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::ParseError);
    EXPECT_NE(std::string{result.message()}.find("'dup'"), std::string::npos) << result.message();
    EXPECT_TRUE(pipeline.nodes().empty()) << "a refused file left half a pipeline behind";
}

TEST(ProjectFileTests, ANegativePortNumberRefusesTheFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline;
    TransmitList transmit;

    const Result result = loadText(directory,
                                   R"({
        "version": 3,
        "pipeline": {
            "nodes": [ { "id": "a", "type": "can.source" }, { "id": "b", "type": "trace.sink" } ],
            "edges": [ { "from": "a", "fromPort": -1, "to": "b", "toPort": 0 } ]
        },
        "transmit": []
    })",
                                   pipeline,
                                   transmit);

    ASSERT_TRUE(result.failed());
    EXPECT_NE(std::string{result.message()}.find("negative port"), std::string::npos)
        << result.message();
}

TEST(ProjectFileTests, ATransmitRowWithNumbersOutOfRangeIsClampedOrRefusedNeverWrapped)
{
    // A channel of 256 is not channel 0, and a cycle past four billion milliseconds is
    // not a short one.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    GraphDescription pipeline;
    TransmitList transmit;

    const Result result = loadText(directory,
                                   R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [
            { "name": "row", "channel": 256, "id": 256, "extended": false, "data": "01 02",
              "trigger": "periodic", "cycleMs": 99999999999 }
        ]
    })",
                                   pipeline,
                                   transmit);

    ASSERT_TRUE(result.succeeded()) << result.message();
    ASSERT_EQ(transmit.entries().size(), 1U);

    EXPECT_EQ(transmit.entries()[0].channel, 255);
    EXPECT_EQ(transmit.entries()[0].cycleMs, 86'400'000U);

    // An identifier past 32 bits is kept out of range, so the check that follows the
    // read refuses the file - it does not become some small identifier.
    GraphDescription other;
    TransmitList otherTransmit;
    const Result refused = loadText(directory,
                                    R"({
        "version": 3,
        "pipeline": { "nodes": [], "edges": [] },
        "transmit": [ { "name": "row", "id": 4294967297, "extended": true, "data": "" } ]
    })",
                                    other,
                                    otherTransmit);

    EXPECT_TRUE(refused.failed());
}
