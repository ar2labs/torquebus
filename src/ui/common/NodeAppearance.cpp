// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/common/NodeAppearance.h"

#include <QStringList>

#include <algorithm>
#include <cstdint>

namespace torquebus::ui {

QString iconNameForCategory(const QString& category)
{
    if (category.compare(QLatin1String("Network"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("network");
    }
    if (category.compare(QLatin1String("Sources"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("source");
    }
    if (category.compare(QLatin1String("Transforms"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("transform");
    }
    if (category.compare(QLatin1String("Simulation"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("simulation");
    }
    if (category.compare(QLatin1String("Diagnostics"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("diagnostics");
    }
    if (category.compare(QLatin1String("Sinks"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("sink");
    }
    return QStringLiteral("project");
}

QString iconNameForNodeType(const QString& typeName)
{
    if (typeName == QLatin1String("can.bus")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("can.source")) {
        return QStringLiteral("hardware");
    }
    if (typeName == QLatin1String("transmit.list")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("log.source")) {
        return QStringLiteral("replay");
    }
    if (typeName == QLatin1String("can.filter")) {
        return QStringLiteral("filter");
    }
    if (typeName == QLatin1String("dbc.decoder")) {
        return QStringLiteral("database");
    }
    if (typeName == QLatin1String("j1939.decoder")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("lua.ecu")) {
        return QStringLiteral("script");
    }
    if (typeName == QLatin1String("sim.restbus")) {
        return QStringLiteral("restbus");
    }
    if (typeName == QLatin1String("lua.test")) {
        return QStringLiteral("test");
    }
    if (typeName == QLatin1String("tinyml.ecu")) {
        return QStringLiteral("neural");
    }
    if (typeName == QLatin1String("uds.client")) {
        return QStringLiteral("diagnostics");
    }
    if (typeName == QLatin1String("isotp.transport")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("can.transmit")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("trace.sink")) {
        return QStringLiteral("trace");
    }
    if (typeName == QLatin1String("can.log")) {
        return QStringLiteral("save");
    }
    if (typeName == QLatin1String("signal.plot")) {
        return QStringLiteral("graph");
    }
    return QStringLiteral("hardware");
}

QString shortTypeBadge(std::string_view typeName)
{
    if (typeName == "can.bus") {
        return QStringLiteral("CAN BUS");
    }
    if (typeName == "can.source") {
        return QStringLiteral("CAN RX");
    }
    if (typeName == "can.transmit") {
        return QStringLiteral("CAN TX");
    }
    if (typeName == "lua.ecu") {
        return QStringLiteral("LUA ECU");
    }
    if (typeName == "tinyml.ecu") {
        return QStringLiteral("TINYML AI");
    }
    if (typeName == "dbc.decoder") {
        return QStringLiteral("DBC");
    }
    if (typeName == "signal.plot") {
        return QStringLiteral("PLOT");
    }
    if (typeName == "trace.sink") {
        return QStringLiteral("TRACE");
    }
    if (typeName == "can.filter") {
        return QStringLiteral("FILTER");
    }
    if (typeName == "uds.client") {
        return QStringLiteral("UDS");
    }
    if (typeName == "isotp.transport") {
        return QStringLiteral("ISO-TP");
    }
    if (typeName == "j1939.decoder") {
        return QStringLiteral("J1939");
    }
    if (typeName == "sim.restbus") {
        return QStringLiteral("RESTBUS");
    }
    if (typeName == "lua.test") {
        return QStringLiteral("TEST SEQ");
    }
    if (typeName == "transmit.list") {
        return QStringLiteral("TX LIST");
    }
    if (typeName == "log.source") {
        return QStringLiteral("LOG PLAY");
    }
    if (typeName == "can.log") {
        return QStringLiteral("LOG REC");
    }
    return QStringLiteral("NODE");
}

QColor accentForNodeType(std::string_view typeName, const Theme& theme)
{
    if (typeName == "tinyml.ecu") {
        return QColor{0x10, 0xB9, 0x81}; // Emerald AI accent
    }
    if (typeName == "lua.ecu" || typeName == "sim.restbus" || typeName == "lua.test") {
        return QColor{0x8B, 0x5C, 0xF6}; // Violet simulation accent
    }
    if (typeName == "dbc.decoder" || typeName == "j1939.decoder" || typeName == "signal.plot") {
        return theme.success;
    }
    if (typeName == "can.filter" || typeName == "uds.client" || typeName == "isotp.transport") {
        return theme.warning;
    }
    return theme.accent;
}

QColor accentOnSurface(const QColor& accent, const Theme& theme, int amount)
{
    return theme.variant == ThemeVariant::Dark ? accent.lighter(amount)
                                               : accent.darker(amount + 20);
}

bool isScriptNodeType(std::string_view typeName) noexcept
{
    return typeName == "lua.ecu" || typeName == "lua.test";
}

QString nodeSubtitle(const NodeDescription& node)
{
    if (node.typeName == "can.bus" || node.typeName == "can.source"
        || node.typeName == "can.transmit") {
        const std::int64_t ch = node.parameters.integer("channel", 0);
        return QStringLiteral("CAN %1").arg(ch + 1);
    }
    if (node.typeName == "lua.ecu") {
        const std::int64_t ch = node.parameters.integer("channel", 0);
        const std::int64_t sa = node.parameters.integer("sa", -1);
        if (sa >= 0) {
            QString ecuTag;
            if (sa == 0x00 || node.id == "ecu_engine") {
                ecuTag = QStringLiteral("EMS");
            } else if (sa == 0x3D || node.id == "ecu_aftertreatment") {
                ecuTag = QStringLiteral("ACM");
            } else if (sa == 0x21 || node.id == "ecu_body") {
                ecuTag = QStringLiteral("BCM");
            } else if (sa == 0x0B || node.id == "ecu_brakes") {
                ecuTag = QStringLiteral("EBS");
            } else if (sa == 0x37 || node.id == "ecu_switches") {
                ecuTag = QStringLiteral("CAB");
            } else if (sa == 0x17 || node.id == "ecu_cluster_core") {
                ecuTag = QStringLiteral("IC");
            }
            if (!ecuTag.isEmpty()) {
                return QStringLiteral("CAN %1 • SA 0x%2 (%3)")
                    .arg(ch + 1)
                    .arg(sa, 2, 16, QLatin1Char{'0'})
                    .toUpper()
                    .arg(ecuTag);
            }
            return QStringLiteral("CAN %1 • SA 0x%2")
                .arg(ch + 1)
                .arg(sa, 2, 16, QLatin1Char{'0'})
                .toUpper();
        }
        const std::int64_t speedId = node.parameters.integer("speed_id", 0x101);
        return QStringLiteral("CAN %1 • 0x%2")
            .arg(ch + 1)
            .arg(speedId, 3, 16, QLatin1Char{'0'})
            .toUpper();
    }
    if (node.typeName == "tinyml.ecu") {
        const bool isJ1939 = node.parameters.boolean("j1939", false);
        if (isJ1939) {
            const std::int64_t outId = node.parameters.integer("outputCanId", 0x18FD0555);
            return QStringLiteral("J1939 • TX 0x%1").arg(outId, 8, 16, QLatin1Char{'0'}).toUpper();
        }
        const std::int64_t outId = node.parameters.integer("outputCanId", 0x105);
        return QStringLiteral("TX 0x%1").arg(outId, 3, 16, QLatin1Char{'0'}).toUpper();
    }
    if (node.typeName == "can.filter") {
        const std::int64_t pgn = node.parameters.integer("pgn", -1);
        if (pgn >= 0) {
            QString pgnTag;
            if (pgn == 65265) {
                pgnTag = QStringLiteral("CCVS1");
            } else if (pgn == 61444) {
                pgnTag = QStringLiteral("EEC1");
            } else if (pgn == 65226) {
                pgnTag = QStringLiteral("DM1");
            }
            if (!pgnTag.isEmpty()) {
                return QStringLiteral("PGN 0x%1 (%2)")
                    .arg(pgn, 4, 16, QLatin1Char{'0'})
                    .toUpper()
                    .arg(pgnTag);
            }
            return QStringLiteral("PGN 0x%1").arg(pgn, 4, 16, QLatin1Char{'0'}).toUpper();
        }
        const std::int64_t fromId = node.parameters.integer("from", 0x000);
        const std::int64_t toId = node.parameters.integer("to", 0x7FF);
        return QStringLiteral("0x%1..0x%2")
            .arg(fromId, 3, 16, QLatin1Char{'0'})
            .arg(toId, 3, 16, QLatin1Char{'0'})
            .toUpper();
    }
    if (node.typeName == "j1939.decoder") {
        const std::string path = node.parameters.text("database");
        if (!path.empty()) {
            const QString qpath = QString::fromStdString(path);
            const qsizetype slash =
                std::max(qpath.lastIndexOf(QLatin1Char('/')), qpath.lastIndexOf(QLatin1Char('\\')));
            return QStringLiteral("J1939 • %1").arg(slash >= 0 ? qpath.mid(slash + 1) : qpath);
        }
        return QStringLiteral("J1939 Decoder");
    }
    if (node.typeName == "dbc.decoder") {
        const std::string path = node.parameters.text("database");
        if (!path.empty()) {
            const QString qpath = QString::fromStdString(path);
            const qsizetype slash =
                std::max(qpath.lastIndexOf(QLatin1Char('/')), qpath.lastIndexOf(QLatin1Char('\\')));
            return slash >= 0 ? qpath.mid(slash + 1) : qpath;
        }
        return QStringLiteral("DBC Dictionary");
    }
    if (node.typeName == "signal.plot") {
        return QStringLiteral("Time-Series Sink");
    }
    return {};
}

} // namespace torquebus::ui
