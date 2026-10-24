// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/TransmitTreeModel.h"

#include "core/database/CanSignal.h"
#include "core/transmit/TransmitList.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace torquebus::ui {
namespace {

[[nodiscard]] QString formatIdentifier(const CanFrame& frame)
{
    return frame.format == CanFrameFormat::Extended
               ? QStringLiteral("%1").arg(frame.identifier, 8, 16, QLatin1Char('0')).toUpper()
               : QStringLiteral("%1").arg(frame.identifier, 3, 16, QLatin1Char('0')).toUpper();
}

[[nodiscard]] QString formatPayload(const CanFrame& frame)
{
    QStringList bytes;
    for (std::size_t i = 0; i < frame.length; ++i) {
        bytes << QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(' '));
}

[[nodiscard]] bool parseIdentifier(QString text, std::uint32_t& out)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }

    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok) {
        return false;
    }

    out = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool parsePayload(QString text, CanFrame& frame)
{
    text.remove(QLatin1Char(' '));
    text.remove(QLatin1Char('\t'));

    if (text.size() % 2 != 0) {
        return false;
    }

    const int count = text.size() / 2;
    if (count > static_cast<int>(kMaxCanPayload)) {
        return false;
    }

    const auto length = static_cast<std::uint8_t>(count);
    const std::uint8_t dlc = dlcFromPayloadLength(length, frame.fd);
    if (payloadLengthFromDlc(dlc, frame.fd) != length) {
        return false;
    }

    std::array<std::uint8_t, kMaxCanPayload> bytes{};
    for (int i = 0; i < count; ++i) {
        bool ok = false;
        const uint value = text.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok) {
            return false;
        }
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
    }

    frame.data = bytes;
    frame.length = length;
    frame.dlc = dlc;
    return true;
}

[[nodiscard]] bool parseSignalInput(const QString& input,
                                    const CanSignal* sig,
                                    double& outPhysical,
                                    std::int64_t& outRaw,
                                    bool& isRaw)
{
    if (sig == nullptr) {
        return false;
    }

    QString text = input.trimmed();
    if (text.startsWith(QLatin1Char('='))) {
        text = text.mid(1).trimmed();
    }

    // 1. Check for named enum (VAL_)
    for (const auto& vn : sig->valueNames) {
        if (QString::fromStdString(vn.name).compare(text, Qt::CaseInsensitive) == 0) {
            outRaw = vn.value;
            isRaw = true;
            return true;
        }
    }

    // 2. Strip signal unit if present
    if (!sig->unit.empty()) {
        const QString unitStr = QString::fromStdString(sig->unit).trimmed();
        if (text.endsWith(unitStr, Qt::CaseInsensitive)) {
            text = text.left(text.length() - unitStr.length()).trimmed();
        }
    }

    // 3. Hex input like 0x100
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        bool hexOk = false;
        const qulonglong rawHex = text.mid(2).toULongLong(&hexOk, 16);
        if (hexOk) {
            outRaw = static_cast<std::int64_t>(rawHex);
            isRaw = true;
            return true;
        }
    }

    // 4. Decimal float/int
    bool ok = false;
    double val = text.toDouble(&ok);
    if (ok) {
        outPhysical = val;
        isRaw = false;
        return true;
    }

    // 5. Try stripping any trailing unit or non-digit chars (e.g. "512rpm" without space)
    int lastDigit = -1;
    for (int i = text.length() - 1; i >= 0; --i) {
        if (text[i].isDigit()) {
            lastDigit = i;
            break;
        }
    }
    if (lastDigit >= 0) {
        const QString numPart = text.left(lastDigit + 1).trimmed();
        val = numPart.toDouble(&ok);
        if (ok) {
            outPhysical = val;
            isRaw = false;
            return true;
        }
    }

    return false;
}

[[nodiscard]] QString formatPosition(const CanSignal& signal)
{
    return QStringLiteral("%1|%2 %3 %4")
        .arg(signal.startBit)
        .arg(signal.bitLength)
        .arg(signal.byteOrder == ByteOrder::Motorola ? QStringLiteral("Motorola")
                                                     : QStringLiteral("Intel"))
        .arg(signal.isSigned ? QStringLiteral("signed") : QStringLiteral("unsigned"));
}

[[nodiscard]] QString formatScaling(const CanSignal& signal)
{
    if (signal.factor == 1.0 && signal.offset == 0.0) {
        return {};
    }

    QString text = QStringLiteral("x%1").arg(signal.factor, 0, 'g', 10);
    if (signal.offset != 0.0) {
        text += QStringLiteral(" %1%2")
                    .arg(signal.offset < 0 ? QStringLiteral("-") : QStringLiteral("+"))
                    .arg(std::abs(signal.offset), 0, 'g', 10);
    }
    return text;
}

[[nodiscard]] QString formatRange(const CanSignal& signal)
{
    if (signal.minimum == 0.0 && signal.maximum == 0.0) {
        return formatScaling(signal);
    }

    QString text =
        QStringLiteral("%1 .. %2").arg(signal.minimum, 0, 'g', 10).arg(signal.maximum, 0, 'g', 10);

    if (const QString scaling = formatScaling(signal); !scaling.isEmpty()) {
        text += QStringLiteral("  (%1)").arg(scaling);
    }
    return text;
}

[[nodiscard]] QString formatMultiplexing(const CanSignal& signal)
{
    if (signal.isMultiplexer && signal.multiplexerValue.has_value()) {
        return TransmitTreeModel::tr("switch, and present when it reads %1")
            .arg(*signal.multiplexerValue);
    }
    if (signal.isMultiplexer) {
        return TransmitTreeModel::tr("multiplexer switch");
    }
    if (signal.multiplexerValue.has_value()) {
        return TransmitTreeModel::tr("present when the switch reads %1")
            .arg(*signal.multiplexerValue);
    }
    return {};
}

constexpr int kCommentCardLineWidth = 54;

[[nodiscard]] QString wrapCommentLines(const QString& text,
                                       int maxLineChars = kCommentCardLineWidth)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QStringList wrappedParagraphs;
    const QStringList paragraphs = trimmed.split(QLatin1Char('\n'));

    for (const QString& rawParagraph : paragraphs) {
        const QString paragraph = rawParagraph.simplified();
        if (paragraph.isEmpty()) {
            continue;
        }
        if (paragraph.size() <= maxLineChars) {
            wrappedParagraphs << paragraph;
            continue;
        }

        const QStringList words = paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString currentLine;
        for (const QString& word : words) {
            if (currentLine.isEmpty()) {
                currentLine = word;
            } else if (currentLine.size() + 1 + word.size() <= maxLineChars) {
                currentLine += QLatin1Char(' ') + word;
            } else {
                wrappedParagraphs << currentLine;
                currentLine = word;
            }
        }
        if (!currentLine.isEmpty()) {
            wrappedParagraphs << currentLine;
        }
    }

    return wrappedParagraphs.join(QLatin1Char('\n'));
}

} // namespace

using enum TransmitTreeModel::Column;

TransmitTreeModel::TransmitTreeModel(TransmitList& list, QObject* parent)
    : QAbstractItemModel{parent}
    , m_list{list}
    , m_monospaceFont{ThemeManager::monospaceFont(9.5)}
    , m_monospaceStrongFont{m_monospaceFont}
{
    m_monospaceStrongFont.setWeight(QFont::Medium);
    m_root = std::make_unique<TransmitTreeNode>();
    m_root->type = TransmitNodeType::Root;
}

TransmitTreeModel::~TransmitTreeModel() = default;

void TransmitTreeModel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_databases = std::move(databases);
    reload();
}

const CanMessage* TransmitTreeModel::messageFor(const TransmitEntry& entry) const
{
    if (!entry.messageName.empty()) {
        for (const auto& database : m_databases) {
            if (const CanMessage* message = database->findByName(entry.messageName)) {
                return message;
            }
        }
    }

    for (const auto& database : m_databases) {
        if (const CanMessage* message =
                database->find(entry.frame.identifier, entry.frame.format)) {
            return message;
        }
    }

    return nullptr;
}

void TransmitTreeModel::rebuildTree()
{
    m_root = std::make_unique<TransmitTreeNode>();
    m_root->type = TransmitNodeType::Root;

    const std::vector<TransmitEntry> entries = m_list.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const TransmitEntry& entry = entries[i];
        auto msgNode = std::make_unique<TransmitTreeNode>();
        msgNode->type = TransmitNodeType::Message;
        msgNode->parent = m_root.get();
        msgNode->rowInParent = static_cast<int>(i);
        msgNode->entryIndex = i;
        msgNode->dbMessage = messageFor(entry);

        if (msgNode->dbMessage != nullptr) {
            for (const CanSignal& sig : msgNode->dbMessage->signalList) {
                auto sigNode = std::make_unique<TransmitTreeNode>();
                sigNode->type = TransmitNodeType::Signal;
                sigNode->parent = msgNode.get();
                sigNode->rowInParent = msgNode->childCount();
                sigNode->entryIndex = i;
                sigNode->dbSignal = &sig;
                msgNode->children.push_back(std::move(sigNode));
            }
        }

        m_root->children.push_back(std::move(msgNode));
    }
}

void TransmitTreeModel::updateMessageSignals(TransmitTreeNode* node, int row)
{
    if (node == nullptr || node->type != TransmitNodeType::Message) {
        return;
    }

    TransmitEntry entry;
    if (!m_list.entryAt(node->entryIndex, entry)) {
        return;
    }

    const CanMessage* newDbMsg = messageFor(entry);
    if (node->dbMessage == newDbMsg) {
        return;
    }

    const QModelIndex parentIdx = createIndex(row, 0, node);

    if (!node->children.empty()) {
        beginRemoveRows(parentIdx, 0, static_cast<int>(node->children.size()) - 1);
        node->children.clear();
        endRemoveRows();
    }

    node->dbMessage = newDbMsg;

    if (newDbMsg != nullptr && !newDbMsg->signalList.empty()) {
        beginInsertRows(parentIdx, 0, static_cast<int>(newDbMsg->signalList.size()) - 1);
        for (const auto& sig : newDbMsg->signalList) {
            auto sigNode = std::make_unique<TransmitTreeNode>();
            sigNode->type = TransmitNodeType::Signal;
            sigNode->parent = node;
            sigNode->rowInParent = node->childCount();
            sigNode->entryIndex = node->entryIndex;
            sigNode->dbSignal = &sig;
            node->children.push_back(std::move(sigNode));
        }
        endInsertRows();
    }
}

void TransmitTreeModel::reload()
{
    beginResetModel();
    rebuildTree();
    endResetModel();
}

void TransmitTreeModel::refreshCounters()
{
    const std::vector<TransmitEntry> entries = m_list.entries();
    if (entries.size() != static_cast<std::size_t>(m_root->childCount())) {
        reload();
        return;
    }

    for (int r = 0; r < m_root->childCount(); ++r) {
        const QModelIndex idx = index(r, ColumnCount, QModelIndex());
        Q_EMIT dataChanged(idx, idx, {Qt::DisplayRole});
    }
}

QModelIndex TransmitTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column < 0 || column >= TransmitColumnCount) {
        return {};
    }

    const TransmitTreeNode* parentNode =
        parent.isValid() ? static_cast<const TransmitTreeNode*>(parent.internalPointer())
                         : m_root.get();

    if (parentNode == nullptr) {
        return {};
    }

    TransmitTreeNode* childNode = parentNode->child(row);
    return childNode != nullptr ? createIndex(row, column, childNode) : QModelIndex{};
}

QModelIndex TransmitTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return {};
    }

    const auto* childNode = static_cast<const TransmitTreeNode*>(child.internalPointer());
    if (childNode == nullptr) {
        return {};
    }

    TransmitTreeNode* parentNode = childNode->parent;
    if (parentNode == nullptr || parentNode == m_root.get()) {
        return {};
    }

    return createIndex(parentNode->rowInParent, 0, parentNode);
}

int TransmitTreeModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0) {
        return 0;
    }

    const TransmitTreeNode* parentNode =
        parent.isValid() ? static_cast<const TransmitTreeNode*>(parent.internalPointer())
                         : m_root.get();

    return parentNode != nullptr ? parentNode->childCount() : 0;
}

int TransmitTreeModel::columnCount(const QModelIndex& parent) const
{
    (void)parent;
    return TransmitColumnCount;
}

QVariant TransmitTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const auto* node = static_cast<const TransmitTreeNode*>(index.internalPointer());
    if (node == nullptr) {
        return {};
    }

    switch (role) {
    case kNodeTypeRole:
        return static_cast<int>(node->type);

    case kEntryIndexRole:
        return static_cast<int>(node->entryIndex);

    case kSignalPointerRole:
        return QVariant::fromValue(reinterpret_cast<quintptr>(node->dbSignal));

    case kCardTitleRole: {
        if (node->type == TransmitNodeType::Message) {
            TransmitEntry entry;
            const bool hasEntry = m_list.entryAt(node->entryIndex, entry);
            const CanMessage* dbMsg = node->dbMessage;
            QString displayName;
            if (hasEntry && !entry.name.empty()) {
                displayName = QString::fromStdString(entry.name);
            } else if (dbMsg != nullptr && !dbMsg->name.empty()) {
                displayName = QString::fromStdString(dbMsg->name);
            } else if (hasEntry) {
                displayName = tr("Message 0x%1").arg(formatIdentifier(entry.frame));
            } else {
                displayName = tr("CAN Message");
            }
            if (hasEntry) {
                const QString hexId = formatIdentifier(entry.frame);
                if (!displayName.contains(hexId, Qt::CaseInsensitive)) {
                    return QStringLiteral("%1 (0x%2)").arg(displayName, hexId);
                }
            }
            return displayName;
        }
        if (node->type == TransmitNodeType::Signal && node->dbSignal != nullptr) {
            TransmitEntry entry;
            const bool hasEntry = m_list.entryAt(node->entryIndex, entry);
            const CanMessage* parentMsg = node->parent ? node->parent->dbMessage : nullptr;
            QString parentName;
            if (parentMsg != nullptr && !parentMsg->name.empty()) {
                parentName = QString::fromStdString(parentMsg->name);
            } else if (hasEntry && !entry.name.empty()) {
                parentName = QString::fromStdString(entry.name);
            } else {
                parentName = tr("Message");
            }
            return QStringLiteral("%1 · %2").arg(parentName,
                                                 QString::fromStdString(node->dbSignal->name));
        }
        return {};
    }

    case kCardMetaRole: {
        if (node->type == TransmitNodeType::Message) {
            TransmitEntry entry;
            if (!m_list.entryAt(node->entryIndex, entry)) {
                return {};
            }
            const CanMessage* dbMsg = node->dbMessage;
            QStringList metaParts;
            QString fmtStr = entry.frame.isExtended() ? QStringLiteral("Extended (29-bit)")
                                                      : QStringLiteral("Standard (11-bit)");
            if (entry.frame.fd) {
                fmtStr += QStringLiteral(" FD");
            }
            metaParts << fmtStr;
            metaParts << tr("Bus %1").arg(entry.channel + 1);
            metaParts << tr("%1 bytes").arg(entry.frame.length);
            if (entry.isPeriodic()) {
                metaParts << tr("Cycle: %1 ms").arg(entry.cycleMs);
            } else {
                metaParts << tr("Trigger: Manual");
            }
            if (dbMsg != nullptr && !dbMsg->transmitter.empty()
                && dbMsg->transmitter != "Vector__XXX") {
                metaParts << tr("Sent by %1").arg(QString::fromStdString(dbMsg->transmitter));
            }
            if (dbMsg != nullptr && !dbMsg->signalList.empty()) {
                metaParts << tr(
                    "%n signal(s)", nullptr, static_cast<int>(dbMsg->signalList.size()));
            }
            return metaParts.join(QStringLiteral(" · "));
        }
        if (node->type == TransmitNodeType::Signal && node->dbSignal != nullptr) {
            const CanSignal& sig = *node->dbSignal;
            QStringList signalMetaParts;
            const QString positionText = formatPosition(sig);
            const QString rangeText = formatRange(sig);
            const QString unitText = QString::fromStdString(sig.unit);

            signalMetaParts << positionText;
            if (!rangeText.isEmpty()) {
                signalMetaParts << (unitText.isEmpty()
                                        ? rangeText
                                        : QStringLiteral("%1 %2").arg(rangeText, unitText));
            } else if (!unitText.isEmpty()) {
                signalMetaParts << unitText;
            }

            if (!sig.receivers.empty()) {
                QStringList rx;
                for (const auto& r : sig.receivers) {
                    if (!r.empty() && r != "Vector__XXX") {
                        rx << QString::fromStdString(r);
                    }
                }
                if (!rx.isEmpty()) {
                    signalMetaParts << tr("Rx: %1").arg(rx.join(QStringLiteral(", ")));
                }
            }
            return signalMetaParts.join(QStringLiteral(" · "));
        }
        return {};
    }

    case kCardBodyRole: {
        if (node->type == TransmitNodeType::Message) {
            const CanMessage* dbMsg = node->dbMessage;
            if (dbMsg != nullptr && !dbMsg->comment.empty()) {
                return wrapCommentLines(QString::fromStdString(dbMsg->comment));
            }
            TransmitEntry entry;
            if (m_list.entryAt(node->entryIndex, entry) && !entry.name.empty()) {
                const QString nameStr = QString::fromStdString(entry.name);
                if (dbMsg == nullptr || nameStr != QString::fromStdString(dbMsg->name)) {
                    if (nameStr.size() > 10) {
                        return wrapCommentLines(nameStr);
                    }
                }
            }
            return {};
        }
        if (node->type == TransmitNodeType::Signal && node->dbSignal != nullptr) {
            const CanSignal& sig = *node->dbSignal;
            const QString signalComment = QString::fromStdString(sig.comment);
            const QString multiplexingText = formatMultiplexing(sig);
            const QString rawBody = !signalComment.isEmpty() ? signalComment : multiplexingText;
            return wrapCommentLines(rawBody);
        }
        return {};
    }

    case kCardValuesRole: {
        if (node->type == TransmitNodeType::Message) {
            const CanMessage* dbMsg = node->dbMessage;
            if (dbMsg != nullptr && !dbMsg->signalList.empty()) {
                QStringList sigNames;
                constexpr int kMaxPreviewSignals = 8;
                int count = 0;
                for (const CanSignal& sig : dbMsg->signalList) {
                    if (++count > kMaxPreviewSignals) {
                        break;
                    }
                    sigNames << QString::fromStdString(sig.name);
                }
                QString preview = tr("Signals: %1").arg(sigNames.join(QStringLiteral(", ")));
                if (static_cast<int>(dbMsg->signalList.size()) > kMaxPreviewSignals) {
                    preview +=
                        tr(" (+%1 more)")
                            .arg(static_cast<int>(dbMsg->signalList.size()) - kMaxPreviewSignals);
                }
                return preview;
            }
            return {};
        }
        if (node->type == TransmitNodeType::Signal && node->dbSignal != nullptr) {
            const CanSignal& sig = *node->dbSignal;
            const QString signalComment = QString::fromStdString(sig.comment);
            const QString multiplexingText = formatMultiplexing(sig);
            QStringList extraCardLines;
            if (!signalComment.isEmpty() && !multiplexingText.isEmpty()) {
                extraCardLines << multiplexingText;
            }
            for (const SignalValueName& value : sig.valueNames) {
                extraCardLines << QStringLiteral("%1 = %2")
                                      .arg(value.value)
                                      .arg(QString::fromStdString(value.name));
            }
            return extraCardLines.join(QLatin1Char('\n'));
        }
        return {};
    }

    case Qt::ToolTipRole: {
        if (index.column() == ColumnSymbol) {
            const QString title = data(index, kCardTitleRole).toString();
            const QString meta = data(index, kCardMetaRole).toString();
            const QString body = data(index, kCardBodyRole).toString();
            const QString values = data(index, kCardValuesRole).toString();

            QStringList sections;
            if (!title.isEmpty()) {
                sections << title;
            }
            if (!meta.isEmpty()) {
                sections << meta;
            }
            if (!body.isEmpty()) {
                sections << body;
            }
            if (!values.isEmpty()) {
                sections << values;
            }
            return sections.join(QStringLiteral("\n\n"));
        }
        return {};
    }

    case Qt::CheckStateRole:
        if (index.column() == ColumnEnabled && node->type == TransmitNodeType::Message) {
            TransmitEntry entry;
            if (m_list.entryAt(node->entryIndex, entry)) {
                return entry.enabled ? Qt::Checked : Qt::Unchecked;
            }
        }
        return {};

    case Qt::DisplayRole:
        return textFor(node, index.column());

    case Qt::EditRole:
        if (node->type == TransmitNodeType::Signal && index.column() == ColumnData) {
            TransmitEntry entry;
            if (m_list.entryAt(node->entryIndex, entry)) {
                return signalPhysicalText(node, entry.frame);
            }
        }
        return textFor(node, index.column());

    case Qt::TextAlignmentRole:
        switch (index.column()) {
        case ColumnEnabled:
        case ColumnBus:
        case ColumnType:
            return QVariant{Qt::AlignCenter};
        case ColumnLength:
        case ColumnCount:
            return QVariant{Qt::AlignRight | Qt::AlignVCenter};
        default:
            return QVariant{Qt::AlignLeft | Qt::AlignVCenter};
        }

    case Qt::FontRole:
        switch (index.column()) {
        case ColumnIdentifier:
        case ColumnData:
            return m_monospaceStrongFont;
        default:
            return m_monospaceFont;
        }

    case Qt::ForegroundRole: {
        const ThemeManager* themes = ThemeManager::instance();
        if (themes == nullptr) {
            return {};
        }
        const Theme& theme = themes->theme();

        if (node->type == TransmitNodeType::Message) {
            if (index.column() == ColumnIdentifier) {
                return theme.tx;
            }
            if (index.column() == ColumnCycle || index.column() == ColumnCount) {
                return theme.textMuted;
            }
        }
        return {};
    }

    default:
        return {};
    }
}

bool TransmitTreeModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid()) {
        return false;
    }

    auto* node = static_cast<TransmitTreeNode*>(index.internalPointer());
    if (node == nullptr) {
        return false;
    }

    // 1. Checkbox toggle on Message row
    if (role == Qt::CheckStateRole && index.column() == ColumnEnabled
        && node->type == TransmitNodeType::Message) {
        const bool checked = (value.toInt() == Qt::Checked);
        m_list.setEnabled(node->entryIndex, checked);
        Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
        Q_EMIT modified();
        return true;
    }

    if (role != Qt::EditRole) {
        return false;
    }

    TransmitEntry entry;
    if (!m_list.entryAt(node->entryIndex, entry)) {
        return false;
    }

    // 2. Edit Signal Row Value
    if (node->type == TransmitNodeType::Signal && index.column() == ColumnData) {
        const CanSignal* sig = node->dbSignal;
        if (sig == nullptr) {
            return false;
        }

        double physical = 0.0;
        std::int64_t raw = 0;
        bool isRaw = false;
        if (!parseSignalInput(value.toString(), sig, physical, raw, isRaw)) {
            return false;
        }

        // Expand frame length if signal doesn't fit in current length
        if (!sig->fitsIn(entry.frame.length)) {
            const std::size_t maxLen = entry.frame.fd ? 64 : 8;
            for (std::size_t l = entry.frame.length + 1; l <= maxLen; ++l) {
                if (sig->fitsIn(l)) {
                    entry.frame.dlc =
                        dlcFromPayloadLength(static_cast<std::uint8_t>(l), entry.frame.fd);
                    entry.frame.length = payloadLengthFromDlc(entry.frame.dlc, entry.frame.fd);
                    break;
                }
            }
        }

        bool encoded = false;
        if (isRaw) {
            encoded = sig->encodeRaw(raw, entry.frame.data.data(), entry.frame.length);
        } else {
            encoded = sig->encode(physical, entry.frame.data.data(), entry.frame.length);
            if (!encoded) {
                // If value was slightly out of range, clamp to min or max
                const double scaled = (physical - sig->offset) / sig->factor;
                const double rounded = std::round(scaled);
                const std::int64_t lowest = sig->minimumRaw();
                const std::int64_t highest = sig->maximumRaw();
                if (rounded <= static_cast<double>(lowest)) {
                    encoded = sig->encodeRaw(lowest, entry.frame.data.data(), entry.frame.length);
                } else if (rounded >= static_cast<double>(highest)) {
                    encoded = sig->encodeRaw(highest, entry.frame.data.data(), entry.frame.length);
                }
            }
        }

        if (encoded) {
            m_list.update(node->entryIndex, entry);

            // Notify this signal
            Q_EMIT dataChanged(index, index);

            if (node->parent != nullptr) {
                const QModelIndex parentMsgData =
                    this->index(node->parent->rowInParent, ColumnData, QModelIndex());
                const QModelIndex parentMsgLen =
                    this->index(node->parent->rowInParent, ColumnLength, QModelIndex());
                Q_EMIT dataChanged(parentMsgData, parentMsgData);
                Q_EMIT dataChanged(parentMsgLen, parentMsgLen);

                // Notify all sibling signals
                const QModelIndex parentMsgIdx =
                    this->index(node->parent->rowInParent, 0, QModelIndex());
                const QModelIndex sigStart = this->index(0, ColumnData, parentMsgIdx);
                const QModelIndex sigEnd =
                    this->index(node->parent->childCount() - 1, ColumnData, parentMsgIdx);
                Q_EMIT dataChanged(sigStart, sigEnd);
            }
            Q_EMIT modified();
            return true;
        }
        return false;
    }

    // 3. Edit Message Row Columns
    if (node->type == TransmitNodeType::Message) {
        switch (index.column()) {
        case ColumnBus: {
            bool ok = false;
            const uint ch = value.toString().toUInt(&ok);
            if (ok && ch >= 1 && ch <= 255) {
                entry.channel = static_cast<std::uint8_t>(ch - 1);
                m_list.update(node->entryIndex, entry);
                Q_EMIT dataChanged(index, index);
                Q_EMIT modified();
                return true;
            }
            break;
        }

        case ColumnType: {
            const QString text = value.toString().trimmed();
            const bool ext = text.contains(QLatin1String("EXT"), Qt::CaseInsensitive)
                             || text.contains(QLatin1String("J1939"), Qt::CaseInsensitive);
            const bool fd = text.contains(QLatin1String("FD"), Qt::CaseInsensitive);
            entry.frame.format = ext ? CanFrameFormat::Extended : CanFrameFormat::Standard;
            entry.frame.fd = fd;
            if (!fd && entry.frame.length > kMaxClassicCanPayload) {
                entry.frame.length = static_cast<std::uint8_t>(kMaxClassicCanPayload);
            }
            entry.frame.dlc = dlcFromPayloadLength(entry.frame.length, entry.frame.fd);
            entry.frame.length = payloadLengthFromDlc(entry.frame.dlc, entry.frame.fd);
            m_list.update(node->entryIndex, entry);
            updateMessageSignals(node, node->rowInParent);
            const QModelIndex rowStart = this->index(node->rowInParent, 0, QModelIndex());
            const QModelIndex rowEnd =
                this->index(node->rowInParent, TransmitColumnCount - 1, QModelIndex());
            Q_EMIT dataChanged(rowStart, rowEnd);
            Q_EMIT modified();
            return true;
        }

        case ColumnIdentifier: {
            std::uint32_t id = 0;
            if (parseIdentifier(value.toString(), id)
                && isValidIdentifier(id, entry.frame.format)) {
                entry.frame.identifier = id;
                m_list.update(node->entryIndex, entry);
                updateMessageSignals(node, node->rowInParent);
                const QModelIndex rowStart = this->index(node->rowInParent, 0, QModelIndex());
                const QModelIndex rowEnd =
                    this->index(node->rowInParent, TransmitColumnCount - 1, QModelIndex());
                Q_EMIT dataChanged(rowStart, rowEnd);
                Q_EMIT modified();
                return true;
            }
            break;
        }

        case ColumnSymbol:
            entry.name = value.toString().toStdString();
            m_list.update(node->entryIndex, entry);
            Q_EMIT dataChanged(index, index);
            Q_EMIT modified();
            return true;

        case ColumnData:
            if (parsePayload(value.toString(), entry.frame)) {
                m_list.update(node->entryIndex, entry);
                Q_EMIT dataChanged(index, index);

                // Re-decode all signals
                const QModelIndex msgIdx = this->index(node->rowInParent, 0, QModelIndex());
                if (node->childCount() > 0) {
                    const QModelIndex sigStart = this->index(0, ColumnData, msgIdx);
                    const QModelIndex sigEnd =
                        this->index(node->childCount() - 1, ColumnData, msgIdx);
                    Q_EMIT dataChanged(sigStart, sigEnd);
                }

                // Update length column
                const QModelIndex lenIdx =
                    this->index(node->rowInParent, ColumnLength, QModelIndex());
                Q_EMIT dataChanged(lenIdx, lenIdx);
                Q_EMIT modified();
                return true;
            }
            break;

        case ColumnCycle: {
            const QString text = value.toString().trimmed();
            if (text.compare(QLatin1String("Wait"), Qt::CaseInsensitive) == 0
                || text.compare(QLatin1String("Manual"), Qt::CaseInsensitive) == 0) {
                entry.trigger = TransmitTrigger::Manual;
                m_list.update(node->entryIndex, entry);
                Q_EMIT dataChanged(index, index);
                Q_EMIT modified();
                return true;
            }

            QString numStr = text;
            numStr.remove(QStringLiteral("ms"), Qt::CaseInsensitive);
            bool ok = false;
            const uint cycle = numStr.trimmed().toUInt(&ok);
            if (ok) {
                entry.trigger = TransmitTrigger::Periodic;
                entry.cycleMs = std::max<std::uint32_t>(cycle, TransmitList::kMinimumCycleMs);
                m_list.update(node->entryIndex, entry);
                Q_EMIT dataChanged(index, index);
                Q_EMIT modified();
                return true;
            }
            break;
        }

        default:
            break;
        }
    }

    return false;
}

QString TransmitTreeModel::textFor(const TransmitTreeNode* node, int column) const
{
    if (node == nullptr) {
        return {};
    }

    TransmitEntry entry;
    if (!m_list.entryAt(node->entryIndex, entry)) {
        return {};
    }

    // 1. Message Row
    if (node->type == TransmitNodeType::Message) {
        switch (column) {
        case ColumnBus:
            return QString::number(entry.channel + 1);

        case ColumnType: {
            QString type = entry.frame.isExtended() ? QStringLiteral("EXT") : QStringLiteral("STD");
            if (entry.frame.fd) {
                type.append(QStringLiteral(" FD"));
            }
            return type;
        }

        case ColumnIdentifier:
            return formatIdentifier(entry.frame);

        case ColumnLength:
            return QString::number(entry.frame.length);

        case ColumnSymbol:
            return QString::fromStdString(entry.name);

        case ColumnData:
            return formatPayload(entry.frame);

        case ColumnCycle:
            return entry.isPeriodic() ? QStringLiteral("%1 ms").arg(entry.cycleMs)
                                      : QStringLiteral("Wait");

        case ColumnCount:
            return QString::number(entry.sentCount);

        default:
            return {};
        }
    }

    // 2. Signal Row
    if (node->type == TransmitNodeType::Signal && node->dbSignal != nullptr) {
        switch (column) {
        case ColumnSymbol:
            return QString::fromStdString(node->dbSignal->name);

        case ColumnData:
            return signalDecodedText(node, entry.frame);

        default:
            return {};
        }
    }

    return {};
}

QString TransmitTreeModel::signalDecodedText(const TransmitTreeNode* node,
                                             const CanFrame& parentFrame) const
{
    if (node == nullptr || node->dbSignal == nullptr) {
        return {};
    }

    const CanSignal* sig = node->dbSignal;
    if (!sig->fitsIn(parentFrame.length)) {
        return QStringLiteral("= ?");
    }

    const std::int64_t raw = sig->rawValue(parentFrame.data.data(), parentFrame.payloadSize());

    // Named enum description
    if (const std::string_view named = sig->nameForValue(raw); !named.empty()) {
        return QStringLiteral("= %1").arg(
            QString::fromUtf8(named.data(), static_cast<int>(named.size())));
    }

    QString text = QStringLiteral("= %1").arg(
        sig->decode(parentFrame.data.data(), parentFrame.payloadSize()), 0, 'g', 8);

    if (!sig->unit.empty()) {
        text += QLatin1Char(' ') + QString::fromStdString(sig->unit);
    }
    return text;
}

QString TransmitTreeModel::signalPhysicalText(const TransmitTreeNode* node,
                                              const CanFrame& parentFrame) const
{
    if (node == nullptr || node->dbSignal == nullptr) {
        return {};
    }

    const CanSignal* sig = node->dbSignal;
    if (!sig->fitsIn(parentFrame.length)) {
        return QStringLiteral("0");
    }

    const std::int64_t raw = sig->rawValue(parentFrame.data.data(), parentFrame.payloadSize());

    if (const std::string_view named = sig->nameForValue(raw); !named.empty()) {
        return QString::fromUtf8(named.data(), static_cast<int>(named.size()));
    }

    return QString::number(sig->decode(parentFrame.data.data(), parentFrame.payloadSize()), 'g', 8);
}

QVariant TransmitTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case ColumnEnabled:
        return tr("On");
    case ColumnBus:
        return tr("Bus");
    case ColumnType:
        return tr("Type");
    case ColumnIdentifier:
        return tr("CAN-ID");
    case ColumnLength:
        return tr("Length");
    case ColumnSymbol:
        return tr("Symbol");
    case ColumnData:
        return tr("Data");
    case ColumnCycle:
        return tr("Cycle Time");
    case ColumnCount:
        return tr("Count");
    default:
        return {};
    }
}

Qt::ItemFlags TransmitTreeModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    const auto* node = static_cast<const TransmitTreeNode*>(index.internalPointer());
    if (node == nullptr) {
        return Qt::NoItemFlags;
    }

    if (node->type == TransmitNodeType::Message) {
        if (index.column() == ColumnEnabled) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        }
        if (index.column() == ColumnLength || index.column() == ColumnCount) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
    }

    if (node->type == TransmitNodeType::Signal) {
        if (index.column() == ColumnData) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
        }
        if (index.column() == ColumnSymbol) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }
    }

    return Qt::NoItemFlags;
}

} // namespace torquebus::ui
