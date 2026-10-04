// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace torquebus::ui {

class TraceTreeFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit TraceTreeFilterModel(QObject* parent = nullptr);

    void setFilterText(const QString& text);
    [[nodiscard]] const QString& filterText() const noexcept { return m_filterText; }

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    [[nodiscard]] bool messageMatches(const QModelIndex& msgIndex) const;
    [[nodiscard]] bool signalMatches(const QModelIndex& sigIndex) const;

    QString m_filterText;
};

} // namespace torquebus::ui
