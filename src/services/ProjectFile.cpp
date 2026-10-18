// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "services/ProjectFile.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <QJsonParseError>
#include <QJsonValue>
#include <QObject>
#include <QSaveFile>
#include <algorithm>
#include <limits>
#include <set>

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
constexpr auto kDatabases = "databases";

constexpr auto kId = "id";
constexpr auto kType = "type";
constexpr auto kEnabled = "enabled";
constexpr auto kParameters = "parameters";
constexpr auto kPosition = "position";
constexpr auto kX = "x";
constexpr auto kY = "y";

constexpr auto kTransmit = "transmit";
constexpr auto kName = "name";
constexpr auto kChannel = "channel";
constexpr auto kExtended = "extended";
constexpr auto kData = "data";
constexpr auto kTrigger = "trigger";
constexpr auto kCycleMs = "cycleMs";
constexpr auto kMessage = "message";

constexpr auto kTriggerManual = "manual";
constexpr auto kTriggerPeriodic = "periodic";

constexpr auto kDashboard = "dashboard";
constexpr auto kWidgets = "widgets";
constexpr auto kKind = "kind";
constexpr auto kBinding = "binding";
constexpr auto kSource = "source";
constexpr auto kSignal = "signal";
constexpr auto kVariable = "variable";
constexpr auto kTitle = "title";
constexpr auto kWidth = "width";
constexpr auto kHeight = "height";
constexpr auto kMinimum = "minimum";
constexpr auto kMaximum = "maximum";
constexpr auto kThreshold = "threshold";
constexpr auto kUnit = "unit";
constexpr auto kDecimals = "decimals";
constexpr auto kProfile = "profile";

// Written only when a widget uses them, so a dashboard that does not is byte for byte what it was
// before they existed - and an older build, which would drop them on its next save, is not asked to
// understand a file that has nothing in it for them.
constexpr auto kImage = "image";
constexpr auto kStates = "states";
constexpr auto kValue = "value";
constexpr auto kLabel = "label";
constexpr auto kTone = "tone";
constexpr auto kLimits = "limits";
constexpr auto kWarningBelow = "warningBelow";
constexpr auto kWarningAbove = "warningAbove";
constexpr auto kCriticalBelow = "criticalBelow";
constexpr auto kCriticalAbove = "criticalAbove";
constexpr auto kWindowSeconds = "windowSeconds";
constexpr auto kStaleMs = "staleMs";

constexpr auto kSourceSignal = "signal";
constexpr auto kSourceVariable = "variable";
constexpr auto kSourceMessage = "message";

/// What a Trend draws when the file does not say.
constexpr double kDefaultWindowSeconds = 30.0;

constexpr auto kFrom = "from";
constexpr auto kFromPort = "fromPort";
constexpr auto kTo = "to";
constexpr auto kToPort = "toPort";

/// Written only when a wire is limited to one identifier format, so a project
/// that never uses it is byte-for-byte what it was before the field existed.
constexpr auto kFrames = "frames";
constexpr auto kFramesStandard = "11-bit";
constexpr auto kFramesExtended = "29-bit";

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
        //
        // Only when it fits. Converting a double that is outside the range of an int64
        // is undefined, and a project file is exactly where one turns up: 1e300 is
        // valid JSON, and a hand edit that slipped an exponent in is how it got there.
        // The cast used to run first and the comparison second, so the value that
        // proved the number did not fit was the one the cast was applied to.
        constexpr double kBound = 9223372036854775808.0; // 2^63

        if (number >= -kBound && number < kBound) {
            const auto whole = static_cast<std::int64_t>(number);

            if (static_cast<double>(whole) == number) {
                return ParameterValue::fromInteger(whole);
            }
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

    switch (edge.frameFormat) {
    case CanFormatMatch::StandardOnly:
        json[kFrames] = QLatin1String{kFramesStandard};
        break;
    case CanFormatMatch::ExtendedOnly:
        json[kFrames] = QLatin1String{kFramesExtended};
        break;
    case CanFormatMatch::Any:
        break;
    }

    return json;
}

/// A payload as "52 03 00" - hex, because bytes have no readable decimal form.
[[nodiscard]] QString payloadToJson(const CanFrame& frame)
{
    QStringList bytes;
    for (std::size_t i = 0; i < frame.length; ++i) {
        bytes << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(' '));
}

/// Reads a payload written by payloadToJson. Whitespace is optional.
void payloadFromJson(const QString& text, CanFrame& frame)
{
    QString packed = text;
    packed.remove(QLatin1Char(' '));

    // Both arguments spelled as qsizetype, which is what QString::size returns.
    // std::min deduces one type from two, so a qsizetype and an int leave it
    // with nothing to deduce - and the error names the ambiguity rather than
    // the missing cast.
    const qsizetype count =
        std::min<qsizetype>(packed.size() / 2, static_cast<qsizetype>(kMaxCanPayload));

    frame.data = {};
    for (qsizetype i = 0; i < count; ++i) {
        frame.data[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(packed.mid(i * 2, 2).toUInt(nullptr, 16));
    }

    frame.length = static_cast<std::uint8_t>(count);
    frame.dlc = dlcFromPayloadLength(frame.length, frame.fd);
}

[[nodiscard]] QJsonObject toJson(const TransmitEntry& entry)
{
    QJsonObject json;
    json[kName] = QString::fromStdString(entry.name);
    json[kChannel] = static_cast<int>(entry.channel);

    // A decimal number, matching how the filter node's parameters already store
    // an identifier. One file, one convention - even though hex would read
    // better here, a format that switches base depending on which section you
    // are in is worse than one that is uniformly inconvenient in one place.
    json[kId] = static_cast<qint64>(entry.frame.identifier);
    json[kExtended] = entry.frame.isExtended();
    json[kData] = payloadToJson(entry.frame);

    // A word, not the enum's number. A reordered enum would silently change
    // what every saved file means.
    json[kTrigger] =
        entry.isPeriodic() ? QLatin1String(kTriggerPeriodic) : QLatin1String(kTriggerManual);
    json[kCycleMs] = static_cast<qint64>(entry.cycleMs);
    json[kEnabled] = entry.enabled;

    if (!entry.messageName.empty()) {
        json[kMessage] = QString::fromStdString(entry.messageName);
    }

    // sentCount and lastSentUs are deliberately absent. They belong to a run,
    // not to a project: saving them would mean a file that claims a row has
    // already been sent forty times before the measurement starts.
    return json;
}

[[nodiscard]] TransmitEntry transmitFromJson(const QJsonObject& json)
{
    TransmitEntry entry;
    entry.name = json.value(kName).toString().toStdString();
    // Clamped, not wrapped: a channel of 256 is not channel 0, and an identifier past
    // 32 bits is not some small one. Out of range, the identifier is kept out of range
    // - the check that follows the read refuses it, with the number in the message.
    entry.channel = static_cast<std::uint8_t>(std::clamp(json.value(kChannel).toInt(0), 0, 255));
    entry.frame.identifier = static_cast<std::uint32_t>(std::clamp<qint64>(
        json.value(kId).toInteger(0), 0, std::numeric_limits<std::uint32_t>::max()));
    entry.frame.format =
        json.value(kExtended).toBool(false) ? CanFrameFormat::Extended : CanFrameFormat::Standard;
    payloadFromJson(json.value(kData).toString(), entry.frame);

    // Anything that is not the word "periodic" is manual - including a word
    // from a future version this build has never heard of. Defaulting the other
    // way would make an unknown trigger start transmitting.
    entry.trigger = json.value(kTrigger).toString() == QLatin1String(kTriggerPeriodic)
                        ? TransmitTrigger::Periodic
                        : TransmitTrigger::Manual;

    entry.cycleMs = static_cast<std::uint32_t>(
        std::clamp<qint64>(json.value(kCycleMs).toInteger(100), 0, 86'400'000));
    entry.enabled = json.value(kEnabled).toBool(true);
    entry.messageName = json.value(kMessage).toString().toStdString();

    return entry;
}

[[nodiscard]] QJsonObject toJson(const DashboardWidget& widget)
{
    QJsonObject json;
    json[kId] = QString::fromStdString(widget.id);

    const std::string_view kind = nameOf(widget.kind);
    json[kKind] = QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()));

    QJsonObject binding;

    switch (widget.binding.source) {
    case DashboardBinding::Source::Signal:
        binding[kSource] = QString::fromLatin1(kSourceSignal);
        binding[kMessage] = QString::fromStdString(widget.binding.message);
        binding[kSignal] = QString::fromStdString(widget.binding.signal);
        break;

    case DashboardBinding::Source::Variable:
        binding[kSource] = QString::fromLatin1(kSourceVariable);
        binding[kVariable] = QString::fromStdString(widget.binding.variable);
        break;

    case DashboardBinding::Source::Message:
        binding[kSource] = QString::fromLatin1(kSourceMessage);
        binding[kMessage] = QString::fromStdString(widget.binding.message);
        break;

    case DashboardBinding::Source::None:
        // Written as an empty object rather than left out, so that a label and
        // a widget somebody has not bound yet look the same in the file - which
        // they are.
        break;
    }

    json[kBinding] = binding;

    QJsonObject position;
    position[kX] = widget.x;
    position[kY] = widget.y;
    json[kPosition] = position;

    json[kWidth] = widget.width;
    json[kHeight] = widget.height;

    // Only what differs from the default. A file where every widget spells out
    // every property is a file where the one deliberate setting is invisible -
    // the same reasoning as `enabled` on a node.
    if (!widget.title.empty()) {
        json[kTitle] = QString::fromStdString(widget.title);
    }
    if (!widget.unit.empty()) {
        json[kUnit] = QString::fromStdString(widget.unit);
    }

    json[kMinimum] = widget.minimum;
    json[kMaximum] = widget.maximum;

    if (widget.threshold != 0.5) {
        json[kThreshold] = widget.threshold;
    }
    if (widget.decimals != 1) {
        json[kDecimals] = widget.decimals;
    }

    // Only a cluster has one, and it is what stands where its binding would.
    if (!widget.profile.empty()) {
        json[kProfile] = QString::fromStdString(widget.profile);
    }

    if (!widget.image.empty()) {
        json[kImage] = QString::fromStdString(widget.image);
    }

    if (!widget.states.empty()) {
        QJsonArray states;

        for (const DashboardState& state : widget.states) {
            QJsonObject entry;
            entry[kValue] = state.value;
            entry[kLabel] = QString::fromStdString(state.label);

            if (state.tone != DashboardTone::Neutral) {
                const std::string_view tone = nameOf(state.tone);
                entry[kTone] = QString::fromUtf8(tone.data(), static_cast<qsizetype>(tone.size()));
            }

            if (!state.image.empty()) {
                entry[kImage] = QString::fromStdString(state.image);
            }

            states.append(entry);
        }

        json[kStates] = states;
    }

    if (widget.limits.any()) {
        QJsonObject limits;

        if (widget.limits.warningBelow.has_value()) {
            limits[kWarningBelow] = *widget.limits.warningBelow;
        }
        if (widget.limits.warningAbove.has_value()) {
            limits[kWarningAbove] = *widget.limits.warningAbove;
        }
        if (widget.limits.criticalBelow.has_value()) {
            limits[kCriticalBelow] = *widget.limits.criticalBelow;
        }
        if (widget.limits.criticalAbove.has_value()) {
            limits[kCriticalAbove] = *widget.limits.criticalAbove;
        }

        json[kLimits] = limits;
    }

    if (widget.windowSeconds != kDefaultWindowSeconds) {
        json[kWindowSeconds] = widget.windowSeconds;
    }

    if (widget.staleMs != 0U) {
        json[kStaleMs] = static_cast<double>(widget.staleMs);
    }

    return json;
}

/// False when the widget names a kind this build does not have.
///
/// Refused rather than defaulted: a project from a newer version that held a
/// widget kind we cannot draw would otherwise open showing a numeric readout
/// where a gauge was, which is a lie about what the file contains.
[[nodiscard]] bool widgetFromJson(const QJsonObject& json, DashboardWidget& widget)
{
    widget.id = json.value(kId).toString().toStdString();

    if (!kindFromName(json.value(kKind).toString().toStdString(), widget.kind)) {
        return false;
    }

    const QJsonObject binding = json.value(kBinding).toObject();
    const QString source = binding.value(kSource).toString();

    if (source == QLatin1String{kSourceSignal}) {
        widget.binding.source = DashboardBinding::Source::Signal;
        widget.binding.message = binding.value(kMessage).toString().toStdString();
        widget.binding.signal = binding.value(kSignal).toString().toStdString();
    } else if (source == QLatin1String{kSourceVariable}) {
        widget.binding.source = DashboardBinding::Source::Variable;
        widget.binding.variable = binding.value(kVariable).toString().toStdString();
    } else if (source == QLatin1String{kSourceMessage}) {
        widget.binding.source = DashboardBinding::Source::Message;
        widget.binding.message = binding.value(kMessage).toString().toStdString();
    } else {
        widget.binding.source = DashboardBinding::Source::None;
    }

    const QJsonObject position = json.value(kPosition).toObject();
    widget.x = position.value(kX).toDouble(0.0);
    widget.y = position.value(kY).toDouble(0.0);

    widget.width = json.value(kWidth).toDouble(160.0);
    widget.height = json.value(kHeight).toDouble(120.0);

    widget.title = json.value(kTitle).toString().toStdString();
    widget.unit = json.value(kUnit).toString().toStdString();

    widget.minimum = json.value(kMinimum).toDouble(0.0);
    widget.maximum = json.value(kMaximum).toDouble(100.0);
    widget.threshold = json.value(kThreshold).toDouble(0.5);
    widget.decimals = json.value(kDecimals).toInt(1);
    widget.profile = json.value(kProfile).toString().toStdString();

    widget.image = json.value(kImage).toString().toStdString();

    // A state with a tone this build does not know is read as having no opinion: the name is what
    // a person reads, and the colour is a hint that a newer build may have had more words for.
    widget.states.clear();

    for (const QJsonValue& entry : json.value(kStates).toArray()) {
        const QJsonObject object = entry.toObject();

        DashboardState state;
        state.value = object.value(kValue).toDouble(0.0);
        state.label = object.value(kLabel).toString().toStdString();
        state.image = object.value(kImage).toString().toStdString();

        DashboardTone tone = DashboardTone::Neutral;
        if (toneFromName(object.value(kTone).toString().toStdString(), tone)) {
            state.tone = tone;
        }

        widget.states.push_back(std::move(state));
    }

    const QJsonObject limits = json.value(kLimits).toObject();
    const auto limitOf = [&limits](const char* key) -> std::optional<double> {
        const QJsonValue value = limits.value(QLatin1String{key});
        return value.isDouble() ? std::optional<double>{value.toDouble()} : std::nullopt;
    };

    widget.limits.warningBelow = limitOf(kWarningBelow);
    widget.limits.warningAbove = limitOf(kWarningAbove);
    widget.limits.criticalBelow = limitOf(kCriticalBelow);
    widget.limits.criticalAbove = limitOf(kCriticalAbove);

    widget.windowSeconds = json.value(kWindowSeconds).toDouble(kDefaultWindowSeconds);

    // Clamped before it is a count: a number out of range is not a quiet wrap-around to something
    // small, it is the largest the type holds, and validate() refuses that one.
    constexpr double kLargestCount = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    widget.staleMs = static_cast<std::uint32_t>(
        std::clamp(json.value(kStaleMs).toDouble(0.0), 0.0, kLargestCount));

    return true;
}

} // namespace

QString ProjectFile::fileFilter()
{
    return QObject::tr("TorqueBus projects (*.%1);;All files (*)").arg(extension());
}

Result ProjectFile::save(const QString& path,
                         const GraphDescription& pipeline,
                         const TransmitList& transmit,
                         const DashboardDescription& dashboard,
                         const QStringList& databases)
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

    QJsonArray rows;
    for (const TransmitEntry& entry : transmit.entries()) {
        rows.append(toJson(entry));
    }

    QJsonArray widgets;
    for (const DashboardWidget& widget : dashboard.widgets()) {
        widgets.append(toJson(widget));
    }

    QJsonObject panel;
    panel[kWidgets] = widgets;

    if (!dashboard.name().empty()) {
        panel[kName] = QString::fromStdString(dashboard.name());
    }

    QJsonObject root;
    root[kVersion] = kFormatVersion;
    root[kApplication] = QStringLiteral("TorqueBus Studio");
    root[kPipeline] = graph;
    root[kTransmit] = rows;

    // Written even when empty, so a project that once had a dashboard and no
    // longer does says so - rather than looking like a file from before
    // dashboards existed, which is a different thing.
    root[kDashboard] = panel;

    if (!databases.isEmpty()) {
        QJsonArray dbArray;
        for (const QString& dbPath : databases) {
            if (!dbPath.trimmed().isEmpty()) {
                dbArray.append(QDir::fromNativeSeparators(dbPath.trimmed()));
            }
        }
        if (!dbArray.isEmpty()) {
            root[kDatabases] = dbArray;
        }
    }

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

Result ProjectFile::load(const QString& path,
                         GraphDescription& pipeline,
                         TransmitList& transmit,
                         DashboardDescription& dashboard,
                         QStringList* databases)
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
        return Result::error(
            ErrorCode::ParseError,
            std::format("'{}' is not a TorqueBus project: no version", path.toStdString()));
    }

    if (version > kFormatVersion) {
        return Result::error(
            ErrorCode::VersionMismatch,
            std::format("'{}' was written by a newer TorqueBus (format {}, this build "
                        "reads {}). Opening it here would silently drop whatever the "
                        "newer version added.",
                        path.toStdString(),
                        version,
                        kFormatVersion));
    }

    // Built into a local first, and only assigned on success: a project that
    // fails halfway through must not leave the canvas showing half a pipeline
    // that was never saved.
    GraphDescription loaded;
    std::set<std::string> seenIds;

    const QJsonObject graph = root.value(kPipeline).toObject();

    for (const QJsonValue& entry : graph.value(kNodes).toArray()) {
        const QJsonObject object = entry.toObject();

        NodeDescription node;
        node.id = object.value(kId).toString().toStdString();
        node.typeName = object.value(kType).toString().toStdString();
        node.enabled = object.value(kEnabled).toBool(true);

        if (node.id.empty() || node.typeName.empty()) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a node with no id or no type", path.toStdString()));
        }

        // An id is how a wire names its ends, so two nodes with one is a wire that
        // goes to either. Refused here, naming the id, rather than opening a canvas
        // where one of the two blocks is the one every wire reaches.
        if (!seenIds.insert(node.id).second) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has two nodes with the id '{}'. Ids address the wires, so each "
                            "must be used once.",
                            path.toStdString(),
                            node.id));
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
        edge.toNode = object.value(kTo).toString().toStdString();

        // Read as signed and checked, then widened: a negative number cast to size_t is
        // a port with a number no block has, and every consumer of an edge indexes with it.
        const int fromPort = object.value(kFromPort).toInt(0);
        const int toPort = object.value(kToPort).toInt(0);

        if (fromPort < 0 || toPort < 0) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a wire between '{}' and '{}' with a negative port number",
                            path.toStdString(),
                            edge.fromNode,
                            edge.toNode));
        }

        edge.fromPort = static_cast<std::size_t>(fromPort);
        edge.toPort = static_cast<std::size_t>(toPort);

        // Absent is every frame. A word this build does not know is refused,
        // not read as "every frame": a wire a newer version limited would
        // otherwise open carrying more than it was drawn to.
        if (const QString frames = object.value(kFrames).toString(); !frames.isEmpty()) {
            if (frames == QLatin1String{kFramesStandard}) {
                edge.frameFormat = CanFormatMatch::StandardOnly;
            } else if (frames == QLatin1String{kFramesExtended}) {
                edge.frameFormat = CanFormatMatch::ExtendedOnly;
            } else {
                return Result::error(
                    ErrorCode::ParseError,
                    std::format("'{}' has a wire from '{}' to '{}' limited to '{}' frames, which "
                                "this version does not know. A newer version added it.",
                                path.toStdString(),
                                edge.fromNode,
                                edge.toNode,
                                frames.toStdString()));
            }
        }

        // An edge naming a node that is not in the file would build a graph
        // that fails at Start with a message about a node the user cannot see.
        // Better to refuse the file and say which wire is wrong.
        if (loaded.find(edge.fromNode) == nullptr || loaded.find(edge.toNode) == nullptr) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a wire between '{}' and '{}', and at least one of "
                            "them is not in the file",
                            path.toStdString(),
                            edge.fromNode,
                            edge.toNode));
        }

        loaded.addEdge(std::move(edge));
    }

    // The transmit list, built into a local for the same reason the pipeline is.
    //
    // An absent section is not an error: every project written before format 2
    // has none, and a build that refused those would make the version check
    // pointless.
    std::vector<TransmitEntry> rows;
    for (const QJsonValue& entry : root.value(kTransmit).toArray()) {
        TransmitEntry row = transmitFromJson(entry.toObject());

        if (!isValidIdentifier(row.frame.identifier, row.frame.format)) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a transmit row whose identifier {} does not fit "
                            "its frame format",
                            path.toStdString(),
                            static_cast<unsigned>(row.frame.identifier)));
        }

        rows.push_back(std::move(row));
    }

    // The dashboard, into a local for the same reason. Absent in every project
    // written before format 3, which loads with an empty one rather than
    // failing - that is what the version field is for.
    DashboardDescription panel;

    const QJsonObject dashboardJson = root.value(kDashboard).toObject();
    panel.setName(dashboardJson.value(kName).toString().toStdString());

    for (const QJsonValue& entry : dashboardJson.value(kWidgets).toArray()) {
        const QJsonObject object = entry.toObject();

        DashboardWidget widget;

        if (!widgetFromJson(object, widget)) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a dashboard widget of kind '{}', which this build "
                            "does not have. Drawing something else in its place would be "
                            "a lie about what the file contains.",
                            path.toStdString(),
                            object.value(kKind).toString().toStdString()));
        }

        if (widget.id.empty()) {
            return Result::error(
                ErrorCode::ParseError,
                std::format("'{}' has a dashboard widget with no id", path.toStdString()));
        }

        panel.add(std::move(widget));
    }

    // Checked before anything is assigned, so a dashboard that could not be
    // drawn refuses the file rather than opening a project that shows an empty
    // panel and writes it back that way.
    if (Result result = panel.validate(); result.failed()) {
        return Result::error(
            result.code(),
            std::format("'{}': {}", path.toStdString(), std::string{result.message()}));
    }

    QStringList loadedDatabases;
    for (const QJsonValue& entry : root.value(kDatabases).toArray()) {
        const QString dbPath = entry.toString().trimmed();
        if (!dbPath.isEmpty()) {
            loadedDatabases.push_back(dbPath);
        }
    }

    pipeline = std::move(loaded);

    // Replaced wholesale, like the pipeline: opening a project means opening
    // its transmit list, not merging it into whatever was on screen.
    transmit.clear();
    for (TransmitEntry& row : rows) {
        (void)transmit.add(std::move(row));
    }

    dashboard = std::move(panel);
    if (databases != nullptr) {
        *databases = std::move(loadedDatabases);
    }

    return Result::ok();
}

} // namespace torquebus::services
