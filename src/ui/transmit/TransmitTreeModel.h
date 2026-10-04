// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"
#include "core/transmit/TransmitEntry.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace torquebus {
class TransmitList;
}

namespace torquebus::ui {

enum class TransmitNodeType : std::uint8_t { Root = 0, Message = 1, Signal = 2 };

struct TransmitTreeNode {
    TransmitNodeType type{TransmitNodeType::Root};
    TransmitTreeNode* parent{nullptr};
    std::vector<std::unique_ptr<TransmitTreeNode>> children;
    int rowInParent{0};

    // For Message node:
    std::size_t entryIndex{0};
    const CanMessage* dbMessage{nullptr};

    // For Signal node:
    const CanSignal* dbSignal{nullptr};

    [[nodiscard]] int childCount() const noexcept { return static_cast<int>(children.size()); }

    [[nodiscard]] TransmitTreeNode* child(int row) const noexcept
    {
        return (row >= 0 && row < childCount()) ? children[static_cast<std::size_t>(row)].get()
                                                : nullptr;
    }
};

class TransmitTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column : int {
        ColumnEnabled = 0, // Checkbox (On / Active)
        ColumnBus, // Bus / Channel (1, 2)
        ColumnType, // Type ("STD", "EXT", "FD", "J1939")
        ColumnIdentifier, // CAN-ID (Hex, e.g. "655" / "0x655")
        ColumnLength, // Length / DLC ("8")
        ColumnSymbol, // Symbol: Message ("✉ Out_RTC_SetTime") / Signal ("🏷️ RTC_SetHour")
        ColumnData, // Data: Message (Hex payload) / Signal ("= 17", "= Monday")
        ColumnCycle, // Cycle Time ("Wait" or "100 ms")
        ColumnCount, // Sent Count ("0", "1540")
        TransmitColumnCount
    };
    Q_ENUM(Column)
    static constexpr int kNodeTypeRole = Qt::UserRole + 1;
    static constexpr int kSignalPointerRole = Qt::UserRole + 2;
    static constexpr int kEntryIndexRole = Qt::UserRole + 3;

    explicit TransmitTreeModel(TransmitList& list, QObject* parent = nullptr);
    ~TransmitTreeModel() override;

    void setDatabases(std::vector<std::shared_ptr<const CanDatabase>> databases);
    void reload();
    void refreshCounters();

    [[nodiscard]] const CanMessage* messageFor(const TransmitEntry& entry) const;

    // --- QAbstractItemModel overrides -------------------------------------

    [[nodiscard]] QModelIndex
    index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    [[nodiscard]] QVariant
    headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

Q_SIGNALS:
    void reported(const QString& message);

private:
    void rebuildTree();
    void updateMessageSignals(TransmitTreeNode* node, int row);
    [[nodiscard]] QString textFor(const TransmitTreeNode* node, int column) const;
    [[nodiscard]] QString signalDecodedText(const TransmitTreeNode* node,
                                            const CanFrame& parentFrame) const;
    [[nodiscard]] QString signalPhysicalText(const TransmitTreeNode* node,
                                             const CanFrame& parentFrame) const;

    TransmitList& m_list;
    std::vector<std::shared_ptr<const CanDatabase>> m_databases;

    std::unique_ptr<TransmitTreeNode> m_root;
    QFont m_monospaceFont;
    QFont m_monospaceStrongFont;
};

} // namespace torquebus::ui
