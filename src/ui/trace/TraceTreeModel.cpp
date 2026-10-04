// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TraceTreeModel.h"

#include "core/j1939/J1939Id.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace torquebus::ui {
namespace {

constexpr int kDefaultRefreshMs = 40; // 25 Hz

[[nodiscard]] QString formatInterval(std::uint32_t microseconds)
{
    if (microseconds == 0) {
        return {};
    }
    return QStringLiteral("%1.%2 ms").arg(microseconds / 1000U).arg((microseconds % 1000U) / 100U);
}

[[nodiscard]] QString formatPayload(const CanFrame& frame)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";

    const std::size_t count = std::min<std::size_t>(frame.length, kMaxCanPayload);
    if (count == 0) {
        return {};
    }

    QString text;
    text.reserve(static_cast<qsizetype>(count * 3));

    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            text.append(QLatin1Char(' '));
        }
        const std::uint8_t byte = frame.data[index];
        text.append(QLatin1Char(kDigits[byte >> 4U]));
        text.append(QLatin1Char(kDigits[byte & 0x0FU]));
    }

    return text;
}

[[nodiscard]] std::uint64_t
keyFor(uint8_t channel, CanDirection direction, CanFrameFormat format, uint32_t identifier) noexcept
{
    return (static_cast<std::uint64_t>(direction == CanDirection::Tx ? 1U : 0U) << 48U)
           | (static_cast<std::uint64_t>(channel) << 40U)
           | (static_cast<std::uint64_t>(format == CanFrameFormat::Extended ? 1U : 0U) << 32U)
           | identifier;
}

} // namespace

TraceTreeModel::TraceTreeModel(QObject* parent)
    : QAbstractItemModel{parent}
    , m_timer{new QTimer(this)}
    , m_monospaceFont{ThemeManager::monospaceFont(9.5)}
    , m_monospaceStrongFont{m_monospaceFont}
{
    m_monospaceStrongFont.setWeight(QFont::Medium);

    initGroups();

    m_timer->setInterval(kDefaultRefreshMs);
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &TraceTreeModel::pollStore);
    m_timer->start();
}

TraceTreeModel::~TraceTreeModel() = default;

void TraceTreeModel::initGroups()
{
    m_root = std::make_unique<TraceTreeNode>();
    m_root->type = TreeNodeType::Root;

    auto rx = std::make_unique<TraceTreeNode>();
    rx->type = TreeNodeType::Group;
    rx->direction = CanDirection::Rx;
    rx->groupTitle = tr("Receive");
    rx->parent = m_root.get();
    rx->rowInParent = 0;
    m_rxGroup = rx.get();
    m_root->children.push_back(std::move(rx));

    auto tx = std::make_unique<TraceTreeNode>();
    tx->type = TreeNodeType::Group;
    tx->direction = CanDirection::Tx;
    tx->groupTitle = tr("Transmit");
    tx->parent = m_root.get();
    tx->rowInParent = 1;
    m_txGroup = tx.get();
    m_root->children.push_back(std::move(tx));
}

void TraceTreeModel::setStore(const TraceStore* store)
{
    beginResetModel();
    m_store = store;
    m_lastIdentifierCount = 0;
    m_messageNodeMap.clear();
    m_rxGroup->children.clear();
    m_txGroup->children.clear();
    endResetModel();
}

void TraceTreeModel::setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases)
{
    m_databases = std::move(databases);
    rebuildDatabaseAssociations();
}

void TraceTreeModel::rebuildDatabaseAssociations()
{
    beginResetModel();
    for (TraceTreeNode* group : {m_rxGroup, m_txGroup}) {
        if (group == nullptr) {
            continue;
        }
        for (auto& msgNode : group->children) {
            msgNode->dbMessage = definitionFor(msgNode->identifier, msgNode->format);
            msgNode->children.clear();

            if (m_store != nullptr && msgNode->statsIndex < m_store->identifiers().size()) {
                const auto& stats = m_store->identifiers()[msgNode->statsIndex];
                populateSignalsForMessage(msgNode.get(), stats.lastFrame);
            }
        }
    }
    endResetModel();
}

void TraceTreeModel::setRefreshIntervalMs(int milliseconds)
{
    m_timer->setInterval(std::max(milliseconds, 1));
}

void TraceTreeModel::setDecimalIdentifiers(bool decimal)
{
    if (decimal == m_decimalIdentifiers) {
        return;
    }
    m_decimalIdentifiers = decimal;
    if (m_rxGroup->childCount() > 0 || m_txGroup->childCount() > 0) {
        Q_EMIT dataChanged(
            index(0, ColumnIdHex), index(rowCount() - 1, ColumnIdDec), {Qt::DisplayRole});
    }
}

void TraceTreeModel::setFrozen(bool frozen)
{
    m_frozen = frozen;
    if (!frozen) {
        pollStore();
    }
}

void TraceTreeModel::reset()
{
    beginResetModel();
    m_lastIdentifierCount = 0;
    m_messageNodeMap.clear();
    if (m_rxGroup != nullptr) {
        m_rxGroup->children.clear();
    }
    if (m_txGroup != nullptr) {
        m_txGroup->children.clear();
    }
    endResetModel();
}

const CanMessage* TraceTreeModel::definitionFor(std::uint32_t identifier,
                                                CanFrameFormat format) const
{
    for (const auto& database : m_databases) {
        if (const CanMessage* message = database->find(identifier, format)) {
            return message;
        }
    }
    return nullptr;
}

void TraceTreeModel::populateSignalsForMessage(TraceTreeNode* msgNode, const CanFrame& frame)
{
    if (msgNode == nullptr || msgNode->dbMessage == nullptr) {
        return;
    }

    msgNode->children.clear();
    int row = 0;
    for (const CanSignal& sig : msgNode->dbMessage->signalList) {
        auto sigNode = std::make_unique<TraceTreeNode>();
        sigNode->type = TreeNodeType::Signal;
        sigNode->parent = msgNode;
        sigNode->rowInParent = row++;
        sigNode->dbSignal = &sig;

        if (sig.fitsIn(frame.length)) {
            sigNode->lastRawValue = sig.rawValue(frame.data.data(), frame.length);
            sigNode->hasLastValue = true;
        }
        sigNode->valueChanged = false;

        msgNode->children.push_back(std::move(sigNode));
    }
}

void TraceTreeModel::pollStore()
{
    if (m_store == nullptr || m_frozen) {
        return;
    }

    const auto& identifiers = m_store->identifiers();

    // Check if store was cleared
    if (identifiers.empty() && m_lastIdentifierCount > 0) {
        reset();
        return;
    }

    // 1. Insert any new identifiers
    if (identifiers.size() > m_lastIdentifierCount) {
        for (std::size_t i = m_lastIdentifierCount; i < identifiers.size(); ++i) {
            const TraceIdentifierStats& stats = identifiers[i];
            TraceTreeNode* group = (stats.direction == CanDirection::Tx) ? m_txGroup : m_rxGroup;
            const QModelIndex groupIdx =
                (stats.direction == CanDirection::Tx) ? txGroupIndex() : rxGroupIndex();

            const std::uint64_t key =
                keyFor(stats.channel, stats.direction, stats.format, stats.identifier);

            auto msgNode = std::make_unique<TraceTreeNode>();
            msgNode->type = TreeNodeType::Message;
            msgNode->parent = group;
            msgNode->statsIndex = i;
            msgNode->messageKey = key;
            msgNode->channel = stats.channel;
            msgNode->identifier = stats.identifier;
            msgNode->format = stats.format;
            msgNode->msgDirection = stats.direction;
            msgNode->lastFrameCount = stats.count;
            msgNode->dbMessage = definitionFor(stats.identifier, stats.format);

            populateSignalsForMessage(msgNode.get(), stats.lastFrame);

            const int newRow = group->childCount();
            msgNode->rowInParent = newRow;

            beginInsertRows(groupIdx, newRow, newRow);
            m_messageNodeMap[key] = msgNode.get();
            group->children.push_back(std::move(msgNode));
            endInsertRows();

            Q_EMIT groupPopulated(groupIdx);
        }
        m_lastIdentifierCount = identifiers.size();
    }

    // 2. Update existing identifiers that received new frames
    for (TraceTreeNode* group : {m_rxGroup, m_txGroup}) {
        if (group == nullptr) {
            continue;
        }
        const QModelIndex groupIdx = (group == m_txGroup) ? txGroupIndex() : rxGroupIndex();

        for (int r = 0; r < group->childCount(); ++r) {
            TraceTreeNode* msgNode = group->child(r);
            if (msgNode == nullptr || msgNode->statsIndex >= identifiers.size()) {
                continue;
            }

            const TraceIdentifierStats& stats = identifiers[msgNode->statsIndex];
            if (stats.count != msgNode->lastFrameCount) {
                msgNode->lastFrameCount = stats.count;

                // Update signals
                for (int s = 0; s < msgNode->childCount(); ++s) {
                    TraceTreeNode* sigNode = msgNode->child(s);
                    if (sigNode != nullptr && sigNode->dbSignal != nullptr) {
                        const CanSignal* sig = sigNode->dbSignal;
                        if (sig->fitsIn(stats.lastFrame.length)) {
                            const std::int64_t raw =
                                sig->rawValue(stats.lastFrame.data.data(), stats.lastFrame.length);
                            sigNode->valueChanged =
                                (sigNode->hasLastValue && raw != sigNode->lastRawValue);
                            sigNode->lastRawValue = raw;
                            sigNode->hasLastValue = true;
                        }
                    }
                }

                // Notify view that message and signals changed
                const QModelIndex msgIdx = index(r, 0, groupIdx);
                const QModelIndex msgEnd = index(r, TreeColumnCount - 1, groupIdx);
                Q_EMIT dataChanged(msgIdx, msgEnd);

                if (msgNode->childCount() > 0) {
                    const QModelIndex sigStart = index(0, 0, msgIdx);
                    const QModelIndex sigEnd =
                        index(msgNode->childCount() - 1, TreeColumnCount - 1, msgIdx);
                    Q_EMIT dataChanged(sigStart, sigEnd);
                }
            }
        }

        // Notify group row update (for counts)
        Q_EMIT dataChanged(groupIdx, index(group->rowInParent, TreeColumnCount - 1, QModelIndex()));
    }
}

QModelIndex TraceTreeModel::rxGroupIndex() const
{
    return (m_rxGroup != nullptr) ? createIndex(0, 0, m_rxGroup) : QModelIndex{};
}

QModelIndex TraceTreeModel::txGroupIndex() const
{
    return (m_txGroup != nullptr) ? createIndex(1, 0, m_txGroup) : QModelIndex{};
}

QModelIndex TraceTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column < 0 || column >= TreeColumnCount) {
        return {};
    }

    const TraceTreeNode* parentNode =
        parent.isValid() ? static_cast<const TraceTreeNode*>(parent.internalPointer())
                         : m_root.get();

    if (parentNode == nullptr) {
        return {};
    }

    TraceTreeNode* childNode = parentNode->child(row);
    return childNode != nullptr ? createIndex(row, column, childNode) : QModelIndex{};
}

QModelIndex TraceTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return {};
    }

    const auto* childNode = static_cast<const TraceTreeNode*>(child.internalPointer());
    if (childNode == nullptr) {
        return {};
    }

    TraceTreeNode* parentNode = childNode->parent;
    if (parentNode == nullptr || parentNode == m_root.get()) {
        return {};
    }

    return createIndex(parentNode->rowInParent, 0, parentNode);
}

int TraceTreeModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0) {
        return 0;
    }

    const TraceTreeNode* parentNode =
        parent.isValid() ? static_cast<const TraceTreeNode*>(parent.internalPointer())
                         : m_root.get();

    return parentNode != nullptr ? parentNode->childCount() : 0;
}

int TraceTreeModel::columnCount(const QModelIndex& parent) const
{
    (void)parent;
    return TreeColumnCount;
}

QVariant TraceTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const auto* node = static_cast<const TraceTreeNode*>(index.internalPointer());
    if (node == nullptr) {
        return {};
    }

    switch (role) {
    case kNodeTypeRole:
        return static_cast<int>(node->type);

    case kDirectionRole:
        if (node->type == TreeNodeType::Group) {
            return static_cast<int>(node->direction);
        }
        if (node->type == TreeNodeType::Message) {
            return static_cast<int>(node->msgDirection);
        }
        if (node->type == TreeNodeType::Signal && node->parent != nullptr) {
            return static_cast<int>(node->parent->msgDirection);
        }
        return 0;

    case kChangedBytesRole:
        if (node->type == TreeNodeType::Message && m_store != nullptr
            && node->statsIndex < m_store->identifiers().size()) {
            return static_cast<qulonglong>(m_store->identifiers()[node->statsIndex].changedBytes);
        }
        return 0ULL;

    case kSignalChangedRole:
        if (node->type == TreeNodeType::Signal) {
            return node->valueChanged;
        }
        return false;

    case Qt::DisplayRole:
        return textFor(node, index.column());

    case Qt::TextAlignmentRole:
        switch (index.column()) {
        case ColumnLength:
        case ColumnCycle:
        case ColumnCount:
            return QVariant{Qt::AlignRight | Qt::AlignVCenter};
        case ColumnType:
            return QVariant{Qt::AlignCenter};
        case ColumnBus:
        default:
            return QVariant{Qt::AlignLeft | Qt::AlignVCenter};
        }

    case Qt::FontRole:
        switch (index.column()) {
        case ColumnIdHex:
        case ColumnData:
            return m_monospaceStrongFont;
        default:
            if (node->type == TreeNodeType::Group) {
                return m_monospaceStrongFont;
            }
            return m_monospaceFont;
        }

    case Qt::ForegroundRole: {
        const ThemeManager* themes = ThemeManager::instance();
        if (themes == nullptr) {
            return {};
        }
        const Theme& theme = themes->theme();

        if (node->type == TreeNodeType::Group) {
            return theme.directionColor(node->direction == CanDirection::Tx);
        }
        if (node->type == TreeNodeType::Message) {
            if (index.column() == ColumnIdHex) {
                return theme.directionColor(node->msgDirection == CanDirection::Tx);
            }
            if (index.column() == ColumnCycle || index.column() == ColumnCount) {
                return theme.textMuted;
            }
        }
        if (node->type == TreeNodeType::Signal) {
            if (index.column() == ColumnDescription) {
                return theme.textMuted;
            }
        }
        return {};
    }

    default:
        return {};
    }
}

QString TraceTreeModel::textFor(const TraceTreeNode* node, int column) const
{
    if (node == nullptr) {
        return {};
    }

    // 1. Group rows ("Receive" / "Transmit")
    if (node->type == TreeNodeType::Group) {
        if (column == ColumnBus) {
            return node->groupTitle;
        }
        if (column == ColumnSymbol) {
            return tr("%1 messages").arg(node->childCount());
        }
        return {};
    }

    // 2. Message rows
    if (node->type == TreeNodeType::Message) {
        if (m_store == nullptr || node->statsIndex >= m_store->identifiers().size()) {
            return {};
        }

        const TraceIdentifierStats& stats = m_store->identifiers()[node->statsIndex];

        switch (column) {
        case ColumnBus:
            return QStringLiteral("CAN %1").arg(node->channel + 1);

        case ColumnType:
            if (node->format == CanFrameFormat::Extended) {
                return stats.lastFrame.fd ? QStringLiteral("FD EXT") : QStringLiteral("J1939");
            }
            return stats.lastFrame.fd ? QStringLiteral("FD") : QStringLiteral("STD");

        case ColumnIdHex:
            return m_decimalIdentifiers
                       ? QString::number(node->identifier)
                       : QString::fromStdString(toIdentifierString(stats.lastFrame));

        case ColumnIdDec:
            if (node->format == CanFrameFormat::Extended) {
                return QString::number(j1939Decompose(node->identifier).pgn());
            }
            return QString::number(node->identifier);

        case ColumnLength:
            return QString::number(stats.lastFrame.length);

        case ColumnSymbol:
            return node->dbMessage != nullptr ? QString::fromStdString(node->dbMessage->name)
                                              : QString{};

        case ColumnData:
            return formatPayload(stats.lastFrame);

        case ColumnCycle:
            return formatInterval(stats.lastCycleUs);

        case ColumnCount:
            return QString::number(stats.count);

        case ColumnDescription:
            return node->dbMessage != nullptr ? QString::fromStdString(node->dbMessage->comment)
                                              : QString{};

        default:
            return {};
        }
    }

    // 3. Signal rows
    if (node->type == TreeNodeType::Signal && node->parent != nullptr) {
        const TraceTreeNode* msgNode = node->parent;
        if (m_store == nullptr || msgNode->statsIndex >= m_store->identifiers().size()) {
            return {};
        }

        const TraceIdentifierStats& stats = m_store->identifiers()[msgNode->statsIndex];

        switch (column) {
        case ColumnSymbol:
            return node->dbSignal != nullptr
                       ? QStringLiteral("~ %1").arg(QString::fromStdString(node->dbSignal->name))
                       : QString{};

        case ColumnData:
            return signalDecodedText(node, stats.lastFrame);

        case ColumnDescription:
            if (node->dbSignal == nullptr) {
                return {};
            }
            if (!node->dbSignal->comment.empty()) {
                return QString::fromStdString(node->dbSignal->comment);
            }
            return QStringLiteral("bits %1..%2 (%3)")
                .arg(node->dbSignal->startBit)
                .arg(node->dbSignal->startBit + node->dbSignal->bitLength - 1)
                .arg(node->dbSignal->byteOrder == ByteOrder::Intel ? QStringLiteral("Intel")
                                                                   : QStringLiteral("Motorola"));

        default:
            return {};
        }
    }

    return {};
}

QString TraceTreeModel::signalDecodedText(const TraceTreeNode* node,
                                          const CanFrame& parentFrame) const
{
    if (node == nullptr || node->dbSignal == nullptr) {
        return {};
    }

    const CanSignal* signal = node->dbSignal;
    if (!signal->fitsIn(parentFrame.length)) {
        return QStringLiteral("= ?");
    }

    const std::int64_t raw = signal->rawValue(parentFrame.data.data(), parentFrame.length);

    if (const std::string_view named = signal->nameForValue(raw); !named.empty()) {
        return QStringLiteral("= %1").arg(
            QString::fromUtf8(named.data(), static_cast<int>(named.size())));
    }

    QString text = QStringLiteral("= %1").arg(
        static_cast<double>(raw) * signal->factor + signal->offset, 0, 'g', 8);

    if (!signal->unit.empty()) {
        text += QLatin1Char(' ') + QString::fromStdString(signal->unit);
    }
    return text;
}

QVariant TraceTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case ColumnBus:
        return tr("Bus");
    case ColumnType:
        return tr("Type");
    case ColumnIdHex:
        return tr("CAN-ID (Hex)");
    case ColumnIdDec:
        return tr("PGN / Dec");
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
    case ColumnDescription:
        return tr("Description");
    default:
        return {};
    }
}

} // namespace torquebus::ui
