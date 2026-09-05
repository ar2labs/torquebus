// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/common/AnimatedToolButton.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace torquebus::ui {
namespace {

/// Matches `border-radius: 2px` on QToolButton in torquebus.qss. If that rule
/// changes, this has to change with it - a rounded wash under a square button
/// shows its corners.
constexpr qreal kCornerRadius = 2.0;

constexpr int kRippleDurationMs = 300;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

} // namespace

AnimatedToolButton::AnimatedToolButton(QAction* action, QWidget* parent)
    : QToolButton{parent}
    , m_hover{this}
    , m_ripple{this}
{
    setAttribute(Qt::WA_Hover, true);

    if (action != nullptr) {
        setDefaultAction(action);
    }
}

void AnimatedToolButton::paintEvent(QPaintEvent* event)
{
    const Theme theme = currentTheme();
    const double hover = m_hover.value();

    // Scoped so the painter is finished before QToolButton makes its own. Two
    // live QPainters on one widget is undefined, and the symptom is a widget
    // that paints intermittently rather than an error.
    {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);

        QPainterPath shape;
        shape.addRoundedRect(QRectF{rect()}, kCornerRadius, kCornerRadius);

        // A disabled button does not respond to a mouse, so it must not look as
        // though it might.
        if (hover > 0.0 && isEnabled()) {
            painter.setBrush(mix(theme.panel, theme.hover, hover));
            painter.drawPath(shape);
        }

        if (m_ripple.isRunning() && isEnabled()) {
            const double progress = m_ripple.value();

            painter.save();
            painter.setClipPath(shape);

            const double reach = std::hypot(width(), height());

            QColor ink = theme.accent;
            ink.setAlphaF(static_cast<float>(0.30 * (1.0 - progress)));

            painter.setBrush(ink);
            painter.drawEllipse(QPointF{m_ripplePoint}, reach * progress, reach * progress);
            painter.restore();
        }
    }

    // The button itself: icon, label, focus ring, and the :pressed and :checked
    // backgrounds, all still from the style sheet.
    QToolButton::paintEvent(event);
}

void AnimatedToolButton::mousePressEvent(QMouseEvent* event)
{
    if (isEnabled()) {
        m_ripplePoint = event->position().toPoint();
        m_ripple.restart(kRippleDurationMs);
    }

    QToolButton::mousePressEvent(event);
}

bool AnimatedToolButton::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Enter:
        m_hover.setTarget(1.0);
        break;

    case QEvent::Leave:
        m_hover.setTarget(0.0);
        break;

    case QEvent::EnabledChange:
        // A button disabled while the cursor is over it never receives a Leave,
        // so without this it would keep its hover wash until the mouse moved -
        // which is exactly what Start does the moment a measurement begins.
        if (!isEnabled()) {
            m_hover.jumpTo(0.0);
            update();
        }
        break;

    default:
        break;
    }

    return QToolButton::event(event);
}

} // namespace torquebus::ui
