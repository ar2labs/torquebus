// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include <QFont>
#include <QStyledItemDelegate>

namespace torquebus::ui {

class TraceTreeDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit TraceTreeDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;

private:
    QFont m_font;
    QFont m_strongFont;
    QFont m_badgeFont;
};

} // namespace torquebus::ui
