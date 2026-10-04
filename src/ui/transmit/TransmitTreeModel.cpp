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
                    entry.frame.length = static_cast<std::uint8_t>(l);
                    entry.frame.dlc = static_cast<std::uint8_t>(l);
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
            m_list.update(node->entryIndex, entry);
            updateMessageSignals(node, node->rowInParent);
            const QModelIndex rowStart = this->index(node->rowInParent, 0, QModelIndex());
            const QModelIndex rowEnd =
                this->index(node->rowInParent, TransmitColumnCount - 1, QModelIndex());
            Q_EMIT dataChanged(rowStart, rowEnd);
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
                return true;
            }
            break;
        }

        case ColumnSymbol:
            entry.name = value.toString().toStdString();
            m_list.update(node->entryIndex, entry);
            Q_EMIT dataChanged(index, index);
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

    const std::int64_t raw = sig->rawValue(parentFrame.data.data(), parentFrame.length);

    // Named enum description
    if (const std::string_view named = sig->nameForValue(raw); !named.empty()) {
        return QStringLiteral("= %1").arg(
            QString::fromUtf8(named.data(), static_cast<int>(named.size())));
    }

    QString text =
        QStringLiteral("= %1").arg(static_cast<double>(raw) * sig->factor + sig->offset, 0, 'g', 8);

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

    const std::int64_t raw = sig->rawValue(parentFrame.data.data(), parentFrame.length);

    if (const std::string_view named = sig->nameForValue(raw); !named.empty()) {
        return QString::fromUtf8(named.data(), static_cast<int>(named.size()));
    }

    return QString::number(static_cast<double>(raw) * sig->factor + sig->offset, 'g', 8);
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
