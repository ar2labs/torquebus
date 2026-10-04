// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include <QFont>
#include <QStyledItemDelegate>

namespace torquebus::ui {

class TransmitTreeDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit TransmitTreeDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;

    [[nodiscard]] QWidget* createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const override;

    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor,
                      QAbstractItemModel* model,
                      const QModelIndex& index) const override;

private:
    QFont m_font;
    QFont m_strongFont;
    QFont m_badgeFont;
};

} // namespace torquebus::ui
