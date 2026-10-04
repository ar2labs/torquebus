// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"
#include "core/trace/TraceStore.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class QTimer;

namespace torquebus::ui {

enum class TreeNodeType : std::uint8_t { Group = 0, Message = 1, Signal = 2, Root = 3 };

enum TreeColumn : int {
    ColumnBus = 0,
    ColumnType,
    ColumnIdHex,
    ColumnIdDec,
    ColumnLength,
    ColumnSymbol,
    ColumnData,
    ColumnCycle,
    ColumnCount,
    ColumnDescription,
    TreeColumnCount
};

struct TraceTreeNode {
    TreeNodeType type{TreeNodeType::Root};
    TraceTreeNode* parent{nullptr};
    std::vector<std::unique_ptr<TraceTreeNode>> children;
    int rowInParent{0};

    // For Group node (Receive vs Transmit):
    CanDirection direction{CanDirection::Rx};
    QString groupTitle;

    // For Message node:
    std::size_t statsIndex{0};
    std::uint64_t messageKey{0};
    std::uint8_t channel{0};
    std::uint32_t identifier{0};
    CanFrameFormat format{CanFrameFormat::Standard};
    CanDirection msgDirection{CanDirection::Rx};
    const CanMessage* dbMessage{nullptr};
    std::uint64_t lastFrameCount{0};

    // For Signal node:
    const CanSignal* dbSignal{nullptr};
    std::int64_t lastRawValue{0};
    bool hasLastValue{false};
    bool valueChanged{false};

    [[nodiscard]] int childCount() const noexcept { return static_cast<int>(children.size()); }

    [[nodiscard]] TraceTreeNode* child(int row) const noexcept
    {
        return (row >= 0 && row < childCount()) ? children[static_cast<std::size_t>(row)].get()
                                                : nullptr;
    }
};

class TraceTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    static constexpr int kChangedBytesRole = Qt::UserRole + 1;
    static constexpr int kSignalChangedRole = Qt::UserRole + 2;
    static constexpr int kNodeTypeRole = Qt::UserRole + 3;
    static constexpr int kDirectionRole = Qt::UserRole + 4;

    explicit TraceTreeModel(QObject* parent = nullptr);
    ~TraceTreeModel() override;

    void setStore(const TraceStore* store);
    [[nodiscard]] const TraceStore* store() const noexcept { return m_store; }

    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);

    void setRefreshIntervalMs(int milliseconds);
    void setDecimalIdentifiers(bool decimal);
    [[nodiscard]] bool usesDecimalIdentifiers() const noexcept { return m_decimalIdentifiers; }

    void setFrozen(bool frozen);
    [[nodiscard]] bool isFrozen() const noexcept { return m_frozen; }

    void reset();

    // --- QAbstractItemModel overrides -------------------------------------

    [[nodiscard]] QModelIndex
    index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant
    headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    [[nodiscard]] QModelIndex rxGroupIndex() const;
    [[nodiscard]] QModelIndex txGroupIndex() const;

public Q_SLOTS:
    void pollStore();

Q_SIGNALS:
    void groupPopulated(const QModelIndex& groupIndex);

private:
    void initGroups();
    [[nodiscard]] const CanMessage* definitionFor(std::uint32_t identifier,
                                                  CanFrameFormat format) const;
    void populateSignalsForMessage(TraceTreeNode* msgNode, const CanFrame& frame);
    void rebuildDatabaseAssociations();

    [[nodiscard]] QString textFor(const TraceTreeNode* node, int column) const;
    [[nodiscard]] QString signalDecodedText(const TraceTreeNode* node,
                                            const CanFrame& parentFrame) const;

    const TraceStore* m_store{nullptr};
    QTimer* m_timer{nullptr};
    QFont m_monospaceFont;
    QFont m_monospaceStrongFont;

    std::unique_ptr<TraceTreeNode> m_root;
    TraceTreeNode* m_rxGroup{nullptr};
    TraceTreeNode* m_txGroup{nullptr};

    std::size_t m_lastIdentifierCount{0};
    std::unordered_map<std::uint64_t, TraceTreeNode*> m_messageNodeMap;

    bool m_frozen{false};
    bool m_decimalIdentifiers{false};

    std::vector<std::shared_ptr<const CanDatabase>> m_databases;
};

} // namespace torquebus::ui
