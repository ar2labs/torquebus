// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/preferences/ThemeCard.h"

#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace torquebus::ui {
namespace {

constexpr int kCardWidth = 132;
constexpr int kCardHeight = 84;
constexpr double kCornerRadius = 5.0;

/// The current theme, for the chrome *around* the card. The inside of the card
/// is painted in the theme the card advertises, which is a different thing.
[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

} // namespace

ThemeCard::ThemeCard(ThemeVariant variant, QWidget* parent)
    : QWidget{parent}
    , m_variant{variant}
    , m_hover{this}
    , m_ring{this}
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(Theme::forVariant(variant).name);
}

void ThemeCard::setAccent(AccentColor accent)
{
    if (accent == m_accent) {
        return;
    }

    m_accent = accent;
    update();
}

void ThemeCard::setSelected(bool selected)
{
    if (selected == m_selected) {
        return;
    }

    m_selected = selected;
    m_ring.setTarget(selected ? 1.0 : 0.0);
    update();
}

QSize ThemeCard::sizeHint() const
{
    return {kCardWidth, kCardHeight};
}

void ThemeCard::paintEvent(QPaintEvent* /*event*/)
{
    // The palette the card is *advertising*, with the accent the user is
    // currently considering already applied to it. That last part is what makes
    // the two cards worth looking at: choosing a hue repaints both, and the
    // difference between what it becomes on dark and on light is the whole
    // argument for deriving the tone per theme rather than storing one colour.
    Theme preview = Theme::forVariant(m_variant);
    applyAccent(preview, m_accent);

    const Theme chrome = currentTheme();

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card = QRectF{rect()}.adjusted(1.5, 1.5, -1.5, -1.5);

    QPainterPath clip;
    clip.addRoundedRect(card, kCornerRadius, kCornerRadius);
    painter.setClipPath(clip);

    // A miniature of the window: chrome strip on top, a side panel on the left,
    // rows of "data" in the middle, and the accent as the one saturated thing
    // in it - which is exactly the weight the accent carries in the real
    // window.
    painter.fillRect(card, preview.panel);

    const double left = card.left();
    const double top = card.top();
    const double width = card.width();
    const double height = card.height();

    painter.fillRect(QRectF{left, top, width, height * 0.18}, preview.toolbar);
    painter.fillRect(QRectF{left, top + height * 0.18, width * 0.28, height * 0.82},
                     preview.background);

    // Toolbar buttons.
    painter.setPen(Qt::NoPen);
    painter.setBrush(preview.accent);
    painter.drawRoundedRect(QRectF{left + 6.0, top + height * 0.055, 16.0, height * 0.07},
                            1.5, 1.5);
    painter.setBrush(preview.textMuted);
    for (int index = 1; index <= 2; ++index) {
        painter.drawRoundedRect(
            QRectF{left + 6.0 + index * 20.0, top + height * 0.055, 16.0, height * 0.07},
            1.5, 1.5);
    }

    // Side panel entries.
    painter.setBrush(preview.textMuted);
    for (int index = 0; index < 4; ++index) {
        painter.drawRect(
            QRectF{left + 6.0, top + height * (0.27 + index * 0.13), width * 0.16, 2.0});
    }

    // Rows of a table. The first is selected, in the accent, because that is
    // the accent's most visible job in the real window.
    const double rowLeft = left + width * 0.33;
    const double rowWidth = width * 0.60;

    painter.setBrush(preview.accent);
    painter.drawRect(QRectF{rowLeft, top + height * 0.28, rowWidth, 5.0});

    painter.setBrush(preview.text);
    for (int index = 1; index < 5; ++index) {
        painter.drawRect(
            QRectF{rowLeft, top + height * (0.28 + index * 0.13), rowWidth * 0.86, 2.0});
    }

    painter.setClipping(false);

    // The frame. Hover lifts it toward the accent; selection is a second,
    // thicker ring in the accent itself, so the two states read differently
    // even at a glance and even in greyscale.
    const double hover = m_hover.value();
    const double ring = m_ring.value();

    const QColor border = mix(chrome.border, chrome.accent, hover * 0.7);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{border, 1.0});
    painter.drawRoundedRect(card, kCornerRadius, kCornerRadius);

    if (ring > 0.01) {
        QColor selection = chrome.accent;
        selection.setAlphaF(static_cast<float>(ring));

        painter.setPen(QPen{selection, 2.0});
        painter.drawRoundedRect(QRectF{rect()}.adjusted(1.0, 1.0, -1.0, -1.0),
                                kCornerRadius + 0.5, kCornerRadius + 0.5);
    }

    // Keyboard focus is its own mark. A card reached by Tab is not the same
    // statement as a card that is chosen, and drawing them alike is how a
    // keyboard user loses track of where they are.
    if (hasFocus()) {
        QColor focus = chrome.accentHover;
        focus.setAlpha(150);

        painter.setPen(QPen{focus, 1.0, Qt::DashLine});
        painter.drawRoundedRect(card.adjusted(2.5, 2.5, -2.5, -2.5),
                                kCornerRadius, kCornerRadius);
    }
}

void ThemeCard::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_pressed = true;
    update();
}

void ThemeCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const bool wasPressed = m_pressed;
    m_pressed = false;

    // Only a release inside the card counts, so a press that the user changed
    // their mind about can be dragged off and cancelled - the behaviour every
    // button on the desktop has.
    if (wasPressed && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked(m_variant);
    }

    update();
}

void ThemeCard::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        Q_EMIT clicked(m_variant);
        return;
    }

    QWidget::keyPressEvent(event);
}

void ThemeCard::enterEvent(QEnterEvent* event)
{
    m_hover.setTarget(1.0);
    QWidget::enterEvent(event);
}

void ThemeCard::leaveEvent(QEvent* event)
{
    m_hover.setTarget(0.0);
    m_pressed = false;
    QWidget::leaveEvent(event);
}

void ThemeCard::focusInEvent(QFocusEvent* event)
{
    update();
    QWidget::focusInEvent(event);
}

void ThemeCard::focusOutEvent(QFocusEvent* event)
{
    update();
    QWidget::focusOutEvent(event);
}

} // namespace torquebus::ui
