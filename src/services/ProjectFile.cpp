// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/ProjectFile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QObject>
#include <QSaveFile>

#include <format>

namespace torquebus::services {
namespace {

// Key names. Written out rather than inlined, because these are the file
// format: a typo in one place and a rename in another is how a project stops
// round-tripping, and the compiler cannot see a string literal drift.
constexpr auto kVersion = "version";
constexpr auto kApplication = "application";
constexpr auto kPipeline = "pipeline";
constexpr auto kNodes = "nodes";
constexpr auto kEdges = "edges";

constexpr auto kId = "id";
constexpr auto kType = "type";
constexpr auto kEnabled = "enabled";
constexpr auto kParameters = "parameters";
constexpr auto kPosition = "position";
constexpr auto kX = "x";
constexpr auto kY = "y";

constexpr auto kFrom = "from";
constexpr auto kFromPort = "fromPort";
constexpr auto kTo = "to";
constexpr auto kToPort = "toPort";

/// A parameter's value, as JSON.
///
/// The type is carried by the JSON value itself - a number stays a number, a
/// string stays a string - rather than by a "type" field beside it. That keeps
/// the file readable, which is the point, at the cost of one ambiguity handled
/// on the way back in: JSON has one number type, so an integer and a real are
/// told apart by whether the value is integral.
[[nodiscard]] QJsonValue toJson(const ParameterValue& value)
{
    switch (value.type()) {
    case ParameterValue::Type::Boolean:
        return value.asBoolean();
    case ParameterValue::Type::Integer:
        return static_cast<double>(value.asInteger());
    case ParameterValue::Type::Real:
        return value.asReal();
    case ParameterValue::Type::Text:
        return QString::fromStdString(value.asText());
    }

    return {};
}

[[nodiscard]] ParameterValue fromJson(const QJsonValue& value)
{
    if (value.isBool()) {
        return ParameterValue::fromBoolean(value.toBool());
    }

    if (value.isDouble()) {
        const double number = value.toDouble();

        // An integral number comes back as an integer. It has to: a CAN
        // identifier read back as 402653440.0 would be handed to a spin box
        // that shows a real, and written out next time with a ".0" that was
        // never there. The catalog's descriptor decides how the value is
        // *used*; this only decides how it is carried.
        const double truncated = static_cast<double>(static_cast<std::int64_t>(number));
        if (number == truncated) {
            return ParameterValue::fromInteger(static_cast<std::int64_t>(number));
        }

        return ParameterValue::fromReal(number);
    }

    return ParameterValue::fromText(value.toString().toStdString());
}

[[nodiscard]] QJsonObject toJson(const NodeDescription& node)
{
    QJsonObject json;
    json[kId] = QString::fromStdString(node.id);
    json[kType] = QString::fromStdString(node.typeName);

    // Only when false. A file where every node carries "enabled": true is a
    // file where the one node that is switched off is harder to spot.
    if (!node.enabled) {
        json[kEnabled] = false;
    }

    QJsonObject position;
    position[kX] = node.x;
    position[kY] = node.y;
    json[kPosition] = position;

    QJsonObject parameters;
    for (const auto& [name, value] : node.parameters.values()) {
        parameters[QString::fromStdString(name)] = toJson(value);
    }
    json[kParameters] = parameters;

    return json;
}

[[nodiscard]] QJsonObject toJson(const EdgeDescription& edge)
{
    QJsonObject json;
    json[kFrom] = QString::fromStdString(edge.fromNode);
    json[kFromPort] = static_cast<int>(edge.fromPort);
    json[kTo] = QString::fromStdString(edge.toNode);
    json[kToPort] = static_cast<int>(edge.toPort);
    return json;
}

} // namespace

QString ProjectFile::fileFilter()
{
    return QObject::tr("TorqueBus projects (*.%1);;All files (*)").arg(extension());
}

Result ProjectFile::save(const QString& path, const GraphDescription& pipeline)
{
    QJsonArray nodes;
    for (const NodeDescription& node : pipeline.nodes()) {
        nodes.append(toJson(node));
    }

    QJsonArray edges;
    for (const EdgeDescription& edge : pipeline.edges()) {
        edges.append(toJson(edge));
    }

    QJsonObject graph;
    graph[kNodes] = nodes;
    graph[kEdges] = edges;

    QJsonObject root;
    root[kVersion] = kFormatVersion;
    root[kApplication] = QStringLiteral("TorqueBus Studio");
    root[kPipeline] = graph;

    // QSaveFile writes to a temporary beside the target and renames on commit,
    // so a crash or a full disk during the save leaves yesterday's project
    // intact rather than a truncated one.
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return Result::error(ErrorCode::FileAccessDenied,
                             std::format("Cannot write '{}': {}",
                                         path.toStdString(),
                                         file.errorString().toStdString()));
    }

    // Indented, not compact. The file is meant to be read and diffed.
    file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));

    if (!file.commit()) {
        return Result::error(ErrorCode::FileAccessDenied,
                             std::format("Cannot save '{}': {}",
                                         path.toStdString(),
                                         file.errorString().toStdString()));
    }

    return Result::ok();
}

Result ProjectFile::load(const QString& path, GraphDescription& pipeline)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return Result::error(ErrorCode::FileNotFound,
                             std::format("Cannot open '{}': {}",
                                         path.toStdString(),
                                         file.errorString().toStdString()));
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);

    if (error.error != QJsonParseError::NoError) {
        // The offset is worth carrying: a hand-edited project file with a
        // trailing comma is a common way to get here, and "line 41" beats
        // "invalid".
        return Result::error(ErrorCode::ParseError,
                             std::format("'{}' is not valid JSON: {} at offset {}",
                                         path.toStdString(),
                                         error.errorString().toStdString(),
                                         error.offset));
    }

    if (!document.isObject()) {
        return Result::error(ErrorCode::ParseError,
                             std::format("'{}' is not a TorqueBus project", path.toStdString()));
    }

    const QJsonObject root = document.object();
    const int version = root.value(kVersion).toInt(0);

    if (version <= 0) {
        return Result::error(ErrorCode::ParseError,
                             std::format("'{}' is not a TorqueBus project: no version",
                                         path.toStdString()));
    }

    if (version > kFormatVersion) {
        return Result::error(
            ErrorCode::VersionMismatch,
            std::format("'{}' was written by a newer TorqueBus (format {}, this build "
                        "reads {}). Opening it here would silently drop whatever the "
                        "newer version added.",
                        path.toStdString(), version, kFormatVersion));
    }

    // Built into a local first, and only assigned on success: a project that
    // fails halfway through must not leave the canvas showing half a pipeline
    // that was never saved.
    GraphDescription loaded;

    const QJsonObject graph = root.value(kPipeline).toObject();

    for (const QJsonValue& entry : graph.value(kNodes).toArray()) {
        const QJsonObject object = entry.toObject();

        NodeDescription node;
        node.id = object.value(kId).toString().toStdString();
        node.typeName = object.value(kType).toString().toStdString();
        node.enabled = object.value(kEnabled).toBool(true);

        if (node.id.empty() || node.typeName.empty()) {
            return Result::error(ErrorCode::ParseError,
                                 std::format("'{}' has a node with no id or no type",
                                             path.toStdString()));
        }

        const QJsonObject position = object.value(kPosition).toObject();
        node.x = position.value(kX).toDouble(0.0);
        node.y = position.value(kY).toDouble(0.0);

        const QJsonObject parameters = object.value(kParameters).toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            node.parameters.set(it.key().toStdString(), fromJson(it.value()));
        }

        loaded.addNode(std::move(node));
    }

    for (const QJsonValue& entry : graph.value(kEdges).toArray()) {
        const QJsonObject object = entry.toObject();

        EdgeDescription edge;
        edge.fromNode = object.value(kFrom).toString().toStdString();
        edge.fromPort = static_cast<std::size_t>(object.value(kFromPort).toInt(0));
        edge.toNode = object.value(kTo).toString().toStdString();
        edge.toPort = static_cast<std::size_t>(object.value(kToPort).toInt(0));

        // An edge naming a node that is not in the file would build a graph
        // that fails at Start with a message about a node the user cannot see.
        // Better to refuse the file and say which wire is wrong.
        if (loaded.find(edge.fromNode) == nullptr || loaded.find(edge.toNode) == nullptr) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a wire between '{}' and '{}', and at least one of "
                            "them is not in the file",
                            path.toStdString(), edge.fromNode, edge.toNode));
        }

        loaded.addEdge(std::move(edge));
    }

    pipeline = std::move(loaded);
    return Result::ok();
}

} // namespace torquebus::services
