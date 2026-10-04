// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/transmit/TransmitTreeDelegate.h"
#include "core/database/CanSignal.h"
#include "ui/theme/ThemeManager.h"
#include "ui/transmit/TransmitTreeModel.h"

#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QLineEdit>
#include <QPainter>
#include <QSpinBox>
#include <QStyle>

#include <algorithm>

namespace torquebus::ui {
namespace {

QColor typeBadgeColor(const QString& type)
{
    if (type.contains(QLatin1String("J1939"))) {
        return QColor(14, 116, 144); // Cyan 700
    }
    if (type.contains(QLatin1String("FD"))) {
        return QColor(124, 58, 237); // Purple 600
    }
    if (type.contains(QLatin1String("EXT"))) {
        return QColor(37, 99, 235); // Blue 600
    }
    return QColor(71, 85, 105); // Slate 600
}

} // namespace

using enum TransmitTreeModel::Column;

TransmitTreeDelegate::TransmitTreeDelegate(QObject* parent)
    : QStyledItemDelegate{parent}
    , m_font{ThemeManager::monospaceFont(9.5)}
    , m_strongFont{m_font}
    , m_badgeFont{m_font}
{
    m_strongFont.setWeight(QFont::Medium);
    m_badgeFont.setPointSizeF(8.0);
    m_badgeFont.setWeight(QFont::Bold);
}

QSize TransmitTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), 22));
    return size;
}

void TransmitTreeDelegate::paint(QPainter* painter,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    opt.text.clear();

    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const ThemeManager* themes = ThemeManager::instance();
    const Theme theme = themes != nullptr ? themes->theme() : Theme::dark();

    const int nodeType = index.data(TransmitTreeModel::kNodeTypeRole).toInt();
    const int col = index.column();
    const bool isSelected = opt.state.testFlag(QStyle::State_Selected);

    // High contrast text on selection: crisp white in Dark mode, dark slate in Light mode
    const QColor selectedTextColor =
        (theme.variant == ThemeVariant::Dark) ? QColor(255, 255, 255) : QColor(15, 23, 42);

    painter->save();
    painter->setClipRect(opt.rect);

    if (col == ColumnType && !text.isEmpty() && nodeType == 1) {
        // Message Type Pill Badge
        const QColor bgColor = typeBadgeColor(text);
        QFontMetrics fm(m_badgeFont);
        const int tw = fm.horizontalAdvance(text);
        const int bw = tw + 10;
        const int bh = std::min(opt.rect.height() - 4, 16);
        const QRect badgeRect(opt.rect.left() + (opt.rect.width() - bw) / 2,
                              opt.rect.top() + (opt.rect.height() - bh) / 2,
                              bw,
                              bh);

        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bgColor);
        painter->drawRoundedRect(badgeRect, 3, 3);

        painter->setFont(m_badgeFont);
        painter->setPen(QColor(255, 255, 255));
        painter->drawText(badgeRect, Qt::AlignCenter, text);
    } else if (col == ColumnSymbol) {
        // Symbol with glyph/icon
        painter->setFont(nodeType == 1 ? m_strongFont : m_font);
        QFontMetrics fm(painter->font());
        int x = opt.rect.left() + 4;
        const int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2;

        if (nodeType == 1 && !text.isEmpty()) {
            // Message: amber envelope icon
            painter->save();
            painter->setPen(QColor(245, 158, 11)); // Amber 500
            const QString iconStr = QStringLiteral("✉ ");
            painter->drawText(QRect(x, y, fm.horizontalAdvance(iconStr), fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              iconStr);
            x += fm.horizontalAdvance(iconStr);
            painter->setPen(isSelected ? selectedTextColor : theme.text);
            painter->drawText(QRect(x, y, opt.rect.right() - x - 4, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              text);
            painter->restore();
        } else if (nodeType == 2 && !text.isEmpty()) {
            // Signal: green tag icon
            painter->save();
            painter->setPen(QColor(16, 185, 129)); // Emerald 500
            const QString iconStr = QStringLiteral("🏷️ ");
            painter->drawText(QRect(x, y, fm.horizontalAdvance(iconStr), fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              iconStr);
            x += fm.horizontalAdvance(iconStr);
            painter->setPen(isSelected ? selectedTextColor : theme.text);
            painter->drawText(QRect(x, y, opt.rect.right() - x - 4, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              text);
            painter->restore();
        } else {
            painter->save();
            painter->setPen(isSelected ? selectedTextColor : theme.text);
            painter->drawText(QRect(x, y, opt.rect.right() - x - 4, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              text);
            painter->restore();
        }
    } else {
        // Standard text columns
        painter->setFont((col == ColumnIdentifier || (col == ColumnData && nodeType == 1))
                             ? m_strongFont
                             : m_font);

        QColor textColor = isSelected ? selectedTextColor : theme.text;
        const QVariant fg = index.data(Qt::ForegroundRole);
        if (fg.canConvert<QColor>() && !isSelected) {
            textColor = fg.value<QColor>();
        } else if (isSelected && col == ColumnIdentifier) {
            textColor = (theme.variant == ThemeVariant::Dark) ? QColor(255, 255, 255) : theme.tx;
        }

        const auto align = index.data(Qt::TextAlignmentRole).toInt();
        const Qt::Alignment alignment =
            align != 0 ? static_cast<Qt::Alignment>(align) : (Qt::AlignLeft | Qt::AlignVCenter);

        const QRect textRect = opt.rect.adjusted(4, 0, -4, 0);

        painter->save();
        painter->setPen(textColor);
        painter->drawText(textRect, alignment, text);
        painter->restore();
    }

    painter->restore();
}

QWidget* TransmitTreeDelegate::createEditor(QWidget* parent,
                                            const QStyleOptionViewItem& option,
                                            const QModelIndex& index) const
{
    (void)option;
    const int nodeType = index.data(TransmitTreeModel::kNodeTypeRole).toInt();

    // 1. Signal Row Value Editor
    if (nodeType == 2 && index.column() == ColumnData) {
        auto ptr = index.data(TransmitTreeModel::kSignalPointerRole).value<quintptr>();
        const auto* sig = reinterpret_cast<const CanSignal*>(ptr);

        if (sig != nullptr && !sig->valueNames.empty()) {
            auto* combo = new QComboBox(parent);
            combo->setEditable(true);
            for (const auto& vn : sig->valueNames) {
                combo->addItem(QString::fromStdString(vn.name), QVariant::fromValue(vn.value));
            }
            return combo;
        }

        auto* edit = new QLineEdit(parent);
        edit->setFont(m_font);
        return edit;
    }

    // 2. Message Row Column Editors
    if (nodeType == 1) {
        switch (index.column()) {
        case ColumnBus: {
            auto* spin = new QSpinBox(parent);
            spin->setRange(1, 255);
            return spin;
        }

        case ColumnType: {
            auto* combo = new QComboBox(parent);
            combo->addItem(QStringLiteral("Standard"));
            combo->addItem(QStringLiteral("Extended"));
            combo->addItem(QStringLiteral("Standard FD"));
            combo->addItem(QStringLiteral("Extended FD"));
            return combo;
        }

        case ColumnCycle: {
            auto* combo = new QComboBox(parent);
            combo->setEditable(true);
            combo->addItem(QStringLiteral("Wait"));
            combo->addItem(QStringLiteral("10 ms"));
            combo->addItem(QStringLiteral("20 ms"));
            combo->addItem(QStringLiteral("50 ms"));
            combo->addItem(QStringLiteral("100 ms"));
            combo->addItem(QStringLiteral("250 ms"));
            combo->addItem(QStringLiteral("500 ms"));
            combo->addItem(QStringLiteral("1000 ms"));
            return combo;
        }

        case ColumnIdentifier:
        case ColumnData:
        case ColumnSymbol: {
            auto* edit = new QLineEdit(parent);
            edit->setFont((index.column() == ColumnSymbol) ? m_font : m_strongFont);
            return edit;
        }

        default:
            break;
        }
    }

    return nullptr;
}

void TransmitTreeDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    const QString value = index.data(Qt::EditRole).toString();

    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        int idx = combo->findText(value, Qt::MatchContains);
        if (idx >= 0) {
            combo->setCurrentIndex(idx);
        } else {
            combo->setEditText(value);
        }
    } else if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
        spin->setValue(value.toInt());
    } else if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
        edit->setText(value);
        edit->selectAll();
    }
}

void TransmitTreeDelegate::setModelData(QWidget* editor,
                                        QAbstractItemModel* model,
                                        const QModelIndex& index) const
{
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        model->setData(index, combo->currentText(), Qt::EditRole);
    } else if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
        model->setData(index, spin->value(), Qt::EditRole);
    } else if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
        model->setData(index, edit->text(), Qt::EditRole);
    }
}

} // namespace torquebus::ui
