// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardText.h"

#include <QCoreApplication>
#include <QStringList>

namespace torquebus::ui {
namespace {

/// A number as short as it can be written: 8031.875 and not 8031.8750000, 0 and not 0.0.
[[nodiscard]] QString plain(double value)
{
    return QString::number(value, 'g', 10);
}

[[nodiscard]] QString tr(const char* text)
{
    return QCoreApplication::translate("torquebus::ui::DashboardText", text);
}

} // namespace

QString displayNameOf(DashboardWidgetKind kind)
{
    switch (kind) {
    case DashboardWidgetKind::Gauge:
        return tr("Gauge");
    case DashboardWidgetKind::Numeric:
        return tr("Numeric");
    case DashboardWidgetKind::Lamp:
        return tr("Lamp");
    case DashboardWidgetKind::Button:
        return tr("Button");
    case DashboardWidgetKind::Switch:
        return tr("Switch");
    case DashboardWidgetKind::Slider:
        return tr("Slider");
    case DashboardWidgetKind::Knob:
        return tr("Knob");
    case DashboardWidgetKind::Label:
        return tr("Label");
    case DashboardWidgetKind::Cluster:
        return tr("Cluster");
    case DashboardWidgetKind::Bar:
        return tr("Bar");
    case DashboardWidgetKind::State:
        return tr("State");
    case DashboardWidgetKind::Image:
        return tr("Image");
    case DashboardWidgetKind::Trend:
        return tr("Trend");
    case DashboardWidgetKind::Table:
        return tr("Table");
    case DashboardWidgetKind::Group:
        return tr("Group");
    case DashboardWidgetKind::Faults:
        return tr("Faults");
    }

    return QString::fromLatin1(nameOf(kind).data(), static_cast<qsizetype>(nameOf(kind).size()));
}

QString displayNameOf(DashboardTone tone)
{
    switch (tone) {
    case DashboardTone::Neutral:
        return tr("Neutral");
    case DashboardTone::Ok:
        return tr("Good");
    case DashboardTone::Info:
        return tr("Information");
    case DashboardTone::Warning:
        return tr("Warning");
    case DashboardTone::Danger:
        return tr("Danger");
    case DashboardTone::Off:
        return tr("Quiet");
    }

    return tr("Neutral");
}

QString summaryOf(const DictionaryEntry& entry)
{
    QStringList parts;

    if (!entry.unit.empty()) {
        parts << QString::fromStdString(entry.unit);
    }

    QString range = plain(entry.minimum) + QStringLiteral(" … ") + plain(entry.maximum);

    // A range the database did not write is arithmetic and not a statement, and says so.
    if (!entry.declaredRange) {
        range += QLatin1Char(' ')
                 + QCoreApplication::translate("torquebus::ui::DashboardText", "(from the bits)");
    }

    parts << range;

    if (entry.cycleTimeMs > 0) {
        parts << QCoreApplication::translate("torquebus::ui::DashboardText", "every %1 ms")
                     .arg(entry.cycleTimeMs);
    }

    if (!entry.transmitter.empty()) {
        parts << QCoreApplication::translate("torquebus::ui::DashboardText", "from %1")
                     .arg(QString::fromStdString(entry.transmitter));
    }

    return parts.join(QStringLiteral(", "));
}

QString detailsOf(const DictionaryEntry& entry)
{
    QStringList lines;
    lines << QString::fromStdString(entry.qualified());

    if (!entry.comment.empty()) {
        lines << QString::fromStdString(entry.comment);
    }

    lines << QString{};

    lines << QCoreApplication::translate("torquebus::ui::DashboardText", "Unit: %1")
                 .arg(entry.unit.empty() ? QStringLiteral("-")
                                         : QString::fromStdString(entry.unit));

    lines << QCoreApplication::translate("torquebus::ui::DashboardText", "Range: %1 to %2%3")
                 .arg(plain(entry.minimum), plain(entry.maximum))
                 .arg(entry.declaredRange ? QString{}
                                          : QCoreApplication::translate(
                                                "torquebus::ui::DashboardText",
                                                " (the database declares none, so this is what the "
                                                "bits can hold)"));

    lines << QCoreApplication::translate("torquebus::ui::DashboardText",
                                         "%1 bits, %2, factor %3, offset %4")
                 .arg(entry.bitLength)
                 .arg(entry.isSigned
                          ? QCoreApplication::translate("torquebus::ui::DashboardText", "signed")
                          : QCoreApplication::translate("torquebus::ui::DashboardText", "unsigned"))
                 .arg(plain(entry.factor), plain(entry.offset));

    lines << QString{};

    const QString message = QString::fromStdString(entry.message);
    const QString sender = QString::fromStdString(entry.transmitter);

    lines << (entry.cycleTimeMs > 0
                  ? QCoreApplication::translate("torquebus::ui::DashboardText",
                                                "Message %1, sent every %2 ms")
                        .arg(message)
                        .arg(entry.cycleTimeMs)
                  : QCoreApplication::translate("torquebus::ui::DashboardText",
                                                "Message %1, with no cycle time in the database")
                        .arg(message));

    if (!sender.isEmpty()) {
        lines << QCoreApplication::translate("torquebus::ui::DashboardText", "Sent by %1")
                     .arg(sender);
    }

    const QString identifier =
        QStringLiteral("0x") + QString::number(entry.identifier, 16).toUpper();
    const QString width =
        entry.format == CanFrameFormat::Extended
            ? QCoreApplication::translate("torquebus::ui::DashboardText", "29-bit")
            : QCoreApplication::translate("torquebus::ui::DashboardText", "11-bit");

    lines << QCoreApplication::translate("torquebus::ui::DashboardText", "%1 (%2), from %3")
                 .arg(identifier, width, QString::fromStdString(entry.database));

    if (!entry.values.empty()) {
        lines << QString{};
        lines << QCoreApplication::translate("torquebus::ui::DashboardText", "Values:");

        for (const SignalValueName& value : entry.values) {
            lines << QStringLiteral("  %1 = %2")
                         .arg(value.value)
                         .arg(QString::fromStdString(value.name));
        }
    }

    return lines.join(QLatin1Char('\n'));
}

QString describeFinding(const BindingFinding& finding, const DashboardBinding& binding)
{
    const QString message = QString::fromStdString(binding.message);
    const QString signal = QString::fromStdString(binding.signal);
    const QString nearestMessage = QString::fromStdString(finding.suggestedMessage);
    const QString nearestSignal = QString::fromStdString(finding.suggestedSignal);

    const auto didYouMean = [](const QString& what) {
        return QCoreApplication::translate("torquebus::ui::DashboardText", " Did you mean %1?")
            .arg(what);
    };

    switch (finding.status) {
    case BindingFinding::Status::UnknownMessage: {
        QString text = QCoreApplication::translate("torquebus::ui::DashboardText",
                                                   "No message \"%1\" in the loaded databases.")
                           .arg(message);

        if (!nearestMessage.isEmpty() && !nearestSignal.isEmpty()) {
            text += didYouMean(nearestMessage + QLatin1Char('.') + nearestSignal);
        } else if (!nearestMessage.isEmpty()) {
            text += didYouMean(nearestMessage);
        }

        return text;
    }

    case BindingFinding::Status::UnknownSignal: {
        QString text =
            QCoreApplication::translate("torquebus::ui::DashboardText", "%1 has no signal \"%2\".")
                .arg(message, signal);

        if (!nearestSignal.isEmpty() && nearestMessage == message) {
            text += didYouMean(nearestSignal);
        } else if (!nearestSignal.isEmpty() && !nearestMessage.isEmpty()) {
            text += didYouMean(nearestMessage + QLatin1Char('.') + nearestSignal);
        }

        return text;
    }

    case BindingFinding::Status::NotChecked:
    case BindingFinding::Status::Found:
        break;
    }

    return {};
}

// ---------------------------------------------------------------------------
// The Faults widget
// ---------------------------------------------------------------------------

QString faultCodeText(const J1939Dtc& code)
{
    const QString spn = code.spnAssembled ? QString::number(code.spn) : QStringLiteral("?");

    return tr("SPN %1 / FMI %2").arg(spn).arg(code.fmi);
}

QString faultMeaningText(const J1939Dtc& code)
{
    const std::string_view meaning = j1939FmiName(code.fmi);

    return QString::fromUtf8(meaning.data(), static_cast<qsizetype>(meaning.size()));
}

QString faultCountText(const J1939Dtc& code)
{
    // 127 is the largest the seven bits hold, and means "at least" (J1939Dtc::occurrenceCount).
    const QString times = code.occurrenceCount >= 127U ? QStringLiteral("127+")
                                                       : QString::number(code.occurrenceCount);

    return QStringLiteral("×") + times;
}

QString faultEcuText(const DashboardFault& fault)
{
    const QString address = tr("SA %1").arg(fault.sourceAddress);

    if (!fault.ecu.has_value()) {
        return address;
    }

    return address + QStringLiteral(" · ") + QString::fromStdString(*fault.ecu);
}

QString faultDetailsText(const DashboardFault& fault)
{
    QStringList lines;
    lines << faultCodeText(fault.code) + QStringLiteral("   ") + faultCountText(fault.code);
    lines << faultMeaningText(fault.code);
    lines << faultEcuText(fault);

    if (!fault.code.spnAssembled) {
        // The honest answer for a code whose packing nobody declared: the four bytes, which
        // somebody who knows the ECU reads in a second.
        lines
            << tr("Bytes %1: the SPN is not assembled, because this code uses a packing older than "
                  "the current one. Declare it in the J1939 block's SPN reading to read it.")
                   .arg(QStringLiteral("%1 %2 %3 %4")
                            .arg(fault.code.raw[0], 2, 16, QLatin1Char('0'))
                            .arg(fault.code.raw[1], 2, 16, QLatin1Char('0'))
                            .arg(fault.code.raw[2], 2, 16, QLatin1Char('0'))
                            .arg(fault.code.raw[3], 2, 16, QLatin1Char('0'))
                            .toUpper());
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace torquebus::ui
