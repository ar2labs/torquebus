// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TraceTreeDelegate.h"
#include "ui/theme/ThemeManager.h"
#include "ui/trace/TraceTreeModel.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>

#include <algorithm>

namespace torquebus::ui {
namespace {

QColor typeBadgeColor(const QString& type)
{
    if (type == QLatin1String("J1939")) {
        return QColor(14, 116, 144); // Cyan 700
    }
    if (type.contains(QLatin1String("FD"))) {
        return QColor(124, 58, 237); // Purple 600
    }
    if (type == QLatin1String("EXT")) {
        return QColor(37, 99, 235); // Blue 600
    }
    return QColor(71, 85, 105); // Slate 600
}

} // namespace

TraceTreeDelegate::TraceTreeDelegate(QObject* parent)
    : QStyledItemDelegate{parent}
    , m_font{ThemeManager::monospaceFont(9.5)}
    , m_strongFont{m_font}
    , m_badgeFont{m_font}
{
    m_strongFont.setWeight(QFont::Medium);
    m_badgeFont.setPointSizeF(8.0);
    m_badgeFont.setWeight(QFont::Bold);
}

QSize TraceTreeDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), 22));
    return size;
}

void TraceTreeDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    opt.text.clear();

    // 1. Draw base selection & background
    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const ThemeManager* themes = ThemeManager::instance();
    const Theme theme = themes != nullptr ? themes->theme() : Theme::dark();

    const int nodeType = index.data(TraceTreeModel::kNodeTypeRole).toInt();
    const int col = index.column();
    const bool isSelected = opt.state.testFlag(QStyle::State_Selected);

    // High contrast text on selection: crisp white in Dark mode, dark slate in Light mode
    const QColor selectedTextColor =
        (theme.variant == ThemeVariant::Dark) ? QColor(255, 255, 255) : QColor(15, 23, 42);

    painter->save();
    painter->setClipRect(opt.rect);

    // 2. Custom painting by column
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
    } else if (col == ColumnData && nodeType == 1) {
        // Message Payload with Byte-Change Highlighting
        const auto changedBytes = index.data(TraceTreeModel::kChangedBytesRole).toULongLong();
        const QStringList bytes = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        painter->setFont(m_strongFont);
        QFontMetrics fm(m_strongFont);
        int x = opt.rect.left() + 4;
        const int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2;
        const int spaceWidth = fm.horizontalAdvance(QLatin1Char(' '));

        for (int i = 0; i < bytes.size(); ++i) {
            const QString byteStr = bytes[i];
            const int byteWidth = fm.horizontalAdvance(byteStr);
            const bool changed = (changedBytes & (1ULL << i)) != 0;

            if (changed) {
                const QRect pillRect(x - 2,
                                     opt.rect.top() + (opt.rect.height() - fm.height()) / 2 - 1,
                                     byteWidth + 4,
                                     fm.height() + 2);

                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(16, 185, 129)); // #10b981
                painter->drawRoundedRect(pillRect, 3, 3);
                painter->setPen(QColor(15, 23, 42)); // #0f172a (dark ink for contrast)
                painter->drawText(QRect(x, y, byteWidth, fm.height()), Qt::AlignCenter, byteStr);
                painter->restore();
            } else {
                painter->save();
                painter->setPen(isSelected ? selectedTextColor : theme.text);
                painter->drawText(QRect(x, y, byteWidth, fm.height()), Qt::AlignCenter, byteStr);
                painter->restore();
            }

            x += byteWidth + spaceWidth;
            if (x > opt.rect.right() - 4) {
                break;
            }
        }
    } else if (col == ColumnData && nodeType == 2) {
        // Signal Value: "= 249.3 km/h"
        const bool changed = index.data(TraceTreeModel::kSignalChangedRole).toBool();
        painter->setFont(m_font);
        QFontMetrics fm(m_font);

        int x = opt.rect.left() + 4;
        const int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2;

        if (changed && !text.isEmpty()) {
            const int tw = fm.horizontalAdvance(text);
            const QRect pillRect(x - 2,
                                 opt.rect.top() + (opt.rect.height() - fm.height()) / 2 - 1,
                                 tw + 4,
                                 fm.height() + 2);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(16, 185, 129, 220));
            painter->drawRoundedRect(pillRect, 3, 3);
            painter->setPen(QColor(15, 23, 42));
            painter->drawText(QRect(x, y, tw, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
            painter->restore();
        } else {
            painter->save();
            painter->setPen(isSelected ? selectedTextColor : theme.text);
            painter->drawText(QRect(x, y, opt.rect.width() - 8, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              text);
            painter->restore();
        }
    } else if (col == ColumnSymbol) {
        // Symbol with glyph/icon
        painter->setFont(nodeType == 1 ? m_strongFont : m_font);
        QFontMetrics fm(painter->font());
        int x = opt.rect.left() + 4;
        const int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2;

        if (nodeType == 1 && !text.isEmpty()) {
            // Message Symbol: amber folder icon or glyph
            painter->save();
            painter->setPen(QColor(245, 158, 11)); // Amber 500
            const QString iconStr = QStringLiteral("📦 ");
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
            // Signal Symbol: green ~ or tag
            painter->save();
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
        // Standard text columns with proper alignment and colors
        painter->setFont((col == ColumnIdHex || nodeType == 0) ? m_strongFont : m_font);

        QColor textColor = isSelected ? selectedTextColor : theme.text;
        const QVariant fg = index.data(Qt::ForegroundRole);
        if (fg.canConvert<QColor>() && !isSelected) {
            textColor = fg.value<QColor>();
        } else if (isSelected && col == ColumnIdHex) {
            textColor = (theme.variant == ThemeVariant::Dark) ? QColor(255, 255, 255) : theme.rx;
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

} // namespace torquebus::ui
