// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/TransmitTreeFilterModel.h"
#include "ui/transmit/TransmitTreeModel.h"

namespace torquebus::ui {

using enum TransmitTreeModel::Column;

TransmitTreeFilterModel::TransmitTreeFilterModel(QObject* parent)
    : QSortFilterProxyModel{parent}
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setRecursiveFilteringEnabled(false);
}

void TransmitTreeFilterModel::setFilterText(const QString& text)
{
    beginFilterChange();
    m_filterText = text.trimmed();
    endFilterChange(Direction::Rows);
}

bool TransmitTreeFilterModel::messageMatches(const QModelIndex& msgIndex) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    for (int col : {ColumnIdentifier, ColumnSymbol, ColumnData, ColumnCycle, ColumnBus}) {
        const QString val =
            model->data(model->index(msgIndex.row(), col, msgIndex.parent())).toString();
        if (val.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool TransmitTreeFilterModel::signalMatches(const QModelIndex& sigIndex) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    for (int col : {ColumnSymbol, ColumnData}) {
        const QString val =
            model->data(model->index(sigIndex.row(), col, sigIndex.parent())).toString();
        if (val.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool TransmitTreeFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_filterText.isEmpty()) {
        return true;
    }

    const QAbstractItemModel* model = sourceModel();
    if (model == nullptr) {
        return false;
    }

    const QModelIndex curIndex = model->index(sourceRow, 0, sourceParent);
    const int nodeType = model->data(curIndex, TransmitTreeModel::kNodeTypeRole).toInt();

    // 1. Message Row (top level)
    if (nodeType == 1) {
        if (messageMatches(curIndex)) {
            return true;
        }
        // Check if any child signal matches
        const int childCount = model->rowCount(curIndex);
        for (int s = 0; s < childCount; ++s) {
            if (signalMatches(model->index(s, 0, curIndex))) {
                return true;
            }
        }
        return false;
    }

    // 2. Signal Row
    if (nodeType == 2) {
        if (signalMatches(curIndex)) {
            return true;
        }
        if (messageMatches(sourceParent)) {
            return true;
        }
        return false;
    }

    return true;
}

} // namespace torquebus::ui
