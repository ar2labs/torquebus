// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/preferences/AccentSwatchRow.h"

#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace torquebus::ui {
namespace {

constexpr double kSwatchDiameter = 22.0;
constexpr double kSwatchSpacing = 32.0;
constexpr double kMargin = 3.0;

/// How long the press ripple takes. Shorter than the 130 ms of a state change
/// because it is an acknowledgement, not a transition - it has to be over
/// before the eye goes looking for the result.
constexpr int kRippleMs = 220;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

[[nodiscard]] int swatchCount()
{
    return static_cast<int>(accentColors().size());
}

[[nodiscard]] AccentColor accentAt(int index)
{
    const std::array<AccentColor, 8> all = accentColors();
    const int clamped = std::clamp(index, 0, static_cast<int>(all.size()) - 1);
    return all[static_cast<std::size_t>(clamped)];
}

[[nodiscard]] int indexOf(AccentColor accent)
{
    const std::array<AccentColor, 8> all = accentColors();
    for (int index = 0; index < static_cast<int>(all.size()); ++index) {
        if (all[static_cast<std::size_t>(index)] == accent) {
            return index;
        }
    }
    return 0;
}

} // namespace

AccentSwatchRow::AccentSwatchRow(QWidget* parent)
    : QWidget{parent}
    , m_ring{this}
    , m_ripple{this}
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Accent colour"));

    // Jumped, not eased: on the first paint there is nowhere for the ring to
    // have come from, and easing in from swatch zero would look like the dialog
    // changing the setting as it opens.
    m_ring.jumpTo(1.0);
    m_ringFrom = indexOf(m_accent);
}

QSize AccentSwatchRow::sizeHint() const
{
    const double width = kMargin * 2.0 + kSwatchSpacing * (swatchCount() - 1) + kSwatchDiameter;
    return QSize{static_cast<int>(std::ceil(width)),
                 static_cast<int>(std::ceil(kSwatchDiameter + kMargin * 2.0))};
}

QSize AccentSwatchRow::minimumSizeHint() const
{
    return sizeHint();
}

QPointF AccentSwatchRow::centreOf(int index) const
{
    return QPointF{kMargin + kSwatchDiameter / 2.0 + index * kSwatchSpacing,
                   height() / 2.0};
}

int AccentSwatchRow::indexAt(const QPointF& position) const
{
    for (int index = 0; index < swatchCount(); ++index) {
        const QPointF delta = position - centreOf(index);

        // A generous target: the circle is 22 px but the hit area is the whole
        // spacing, so the gaps between swatches are not dead ground.
        if (std::abs(delta.x()) <= kSwatchSpacing / 2.0) {
            return index;
        }
    }
    return -1;
}

void AccentSwatchRow::setAccent(AccentColor accent)
{
    if (accent == m_accent) {
        return;
    }

    m_ringFrom = indexOf(m_accent);
    m_accent = accent;

    m_ring.jumpTo(0.0);
    m_ring.setTarget(1.0);
    update();
}

void AccentSwatchRow::choose(int index)
{
    const AccentColor accent = accentAt(index);

    m_rippleIndex = index;
    m_ripple.restart(kRippleMs);

    if (accent == m_accent) {
        update();
        return;
    }

    setAccent(accent);
    Q_EMIT accentChanged(accent);
}

void AccentSwatchRow::paintEvent(QPaintEvent* /*event*/)
{
    const Theme theme = currentTheme();

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double ringProgress = m_ring.value();
    const double ringPosition = m_ringFrom + (indexOf(m_accent) - m_ringFrom) * ringProgress;

    for (int index = 0; index < swatchCount(); ++index) {
        const QPointF centre = centreOf(index);
        const double radius = kSwatchDiameter / 2.0;

        // Each swatch shows the colour it will actually produce on the theme
        // that is running - not a nominal red or blue. Anything else would be
        // advertising a colour the user is not going to get.
        const AccentPair pair = accentPairFor(accentAt(index), theme.variant);

        const bool hovered = index == m_hovered;

        painter.setPen(Qt::NoPen);
        painter.setBrush(hovered ? pair.hover : pair.accent);
        painter.drawEllipse(centre, radius, radius);

        // The ripple: a ring expanding out of the swatch that was pressed.
        //
        // Gated on the value reaching 1, not on the animation still running.
        // The last step of a Motion repaints and then stops, so a check on
        // isRunning() can leave the final ring painted with nothing scheduled
        // to erase it. At progress 1 the alpha below is zero, which makes the
        // end state correct whether or not another paint ever arrives.
        if (index == m_rippleIndex) {
            const double progress = m_ripple.value();

            QColor wash = pair.hover;
            wash.setAlphaF(static_cast<float>((1.0 - progress) * 0.55));

            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen{wash, 2.0});
            painter.drawEllipse(centre, radius + progress * 8.0, radius + progress * 8.0);
        }
    }

    // One ring, drawn where the animation says it is rather than around the
    // selected swatch, so a change slides between the two instead of jumping.
    {
        const QPointF centre{kMargin + kSwatchDiameter / 2.0 + ringPosition * kSwatchSpacing,
                             height() / 2.0};

        // In the theme's border colour, not the swatch's own: a ring painted in
        // the colour it surrounds is a ring you cannot see.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen{theme.text, 1.6});
        painter.drawEllipse(centre, kSwatchDiameter / 2.0 + 3.0, kSwatchDiameter / 2.0 + 3.0);
    }

    if (hasFocus()) {
        QColor focus = theme.accentHover;
        focus.setAlpha(160);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen{focus, 1.0, Qt::DashLine});
        painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);
    }
}

void AccentSwatchRow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int index = indexAt(event->position());
    if (index >= 0) {
        choose(index);
    }
}

void AccentSwatchRow::mouseMoveEvent(QMouseEvent* event)
{
    const int index = indexAt(event->position());
    if (index == m_hovered) {
        return;
    }

    m_hovered = index;

    if (index >= 0) {
        // The name, because eight circles are eight circles: a user who wants
        // the one they picked last month needs it named, not recognised.
        QToolTip::showText(event->globalPosition().toPoint(),
                           accentDisplayName(accentAt(index)), this);
    }

    update();
}

void AccentSwatchRow::keyPressEvent(QKeyEvent* event)
{
    const int current = indexOf(m_accent);

    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        choose(std::max(current - 1, 0));
        return;

    case Qt::Key_Right:
    case Qt::Key_Down:
        choose(std::min(current + 1, swatchCount() - 1));
        return;

    case Qt::Key_Home:
        choose(0);
        return;

    case Qt::Key_End:
        choose(swatchCount() - 1);
        return;

    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void AccentSwatchRow::leaveEvent(QEvent* event)
{
    m_hovered = -1;
    update();
    QWidget::leaveEvent(event);
}

void AccentSwatchRow::focusInEvent(QFocusEvent* event)
{
    update();
    QWidget::focusInEvent(event);
}

void AccentSwatchRow::focusOutEvent(QFocusEvent* event)
{
    update();
    QWidget::focusOutEvent(event);
}

} // namespace torquebus::ui
