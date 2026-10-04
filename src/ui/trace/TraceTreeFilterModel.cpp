// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TraceTreeFilterModel.h"
#include "ui/trace/TraceTreeModel.h"

namespace torquebus::ui {

TraceTreeFilterModel::TraceTreeFilterModel(QObject* parent)
    : QSortFilterProxyModel{parent}
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setRecursiveFilteringEnabled(false); // we handle custom hierarchical propagation
}

void TraceTreeFilterModel::setFilterText(const QString& text)
{
    beginFilterChange();
    m_filterText = text.trimmed();
    endFilterChange(Direction::Rows);
}

bool TraceTreeFilterModel::messageMatches(const QModelIndex& msgIndex) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    // Check CAN ID (Hex), CAN ID (Dec), Symbol (Message name), Description, Type
    for (int col :
         {ColumnIdHex, ColumnIdDec, ColumnSymbol, ColumnDescription, ColumnType, ColumnBus}) {
        const QString val =
            model->data(model->index(msgIndex.row(), col, msgIndex.parent())).toString();
        if (val.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool TraceTreeFilterModel::signalMatches(const QModelIndex& sigIndex) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    // Check Signal name (Symbol), Signal Data, Description
    for (int col : {ColumnSymbol, ColumnData, ColumnDescription}) {
        const QString val =
            model->data(model->index(sigIndex.row(), col, sigIndex.parent())).toString();
        if (val.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool TraceTreeFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    const QModelIndex curIndex = model->index(sourceRow, 0, sourceParent);
    const int nodeType = model->data(curIndex, TraceTreeModel::kNodeTypeRole).toInt();

    // 1. Group Row ("Receive" / "Transmit")
    if (nodeType == 0) {
        // Accept if any child message or grandchild signal matches
        const int childMsgCount = model->rowCount(curIndex);
        for (int r = 0; r < childMsgCount; ++r) {
            const QModelIndex msgIdx = model->index(r, 0, curIndex);
            if (messageMatches(msgIdx)) {
                return true;
            }
            const int childSigCount = model->rowCount(msgIdx);
            for (int s = 0; s < childSigCount; ++s) {
                if (signalMatches(model->index(s, 0, msgIdx))) {
                    return true;
                }
            }
        }
        return false;
    }

    // 2. Message Row
    if (nodeType == 1) {
        if (messageMatches(curIndex)) {
            return true;
        }
        // Check if any child signal matches
        const int childSigCount = model->rowCount(curIndex);
        for (int s = 0; s < childSigCount; ++s) {
            if (signalMatches(model->index(s, 0, curIndex))) {
                return true;
            }
        }
        return false;
    }

    // 3. Signal Row
    if (nodeType == 2) {
        if (signalMatches(curIndex)) {
            return true;
        }
        // If parent message matched, show the signal too
        if (messageMatches(sourceParent)) {
            return true;
        }
        return false;
    }

    return true;
}

} // namespace torquebus::ui
