// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/playback/TimelineBar.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRect>
#include <QSize>

#include <algorithm>

namespace torquebus::ui {
namespace {

/// Height of the track itself. The widget is taller: the handle needs room.
constexpr int kTrackHeight = 6;

/// Radius of the handle, and therefore the inset at both ends of the track.
constexpr int kHandleRadius = 7;

/// What an arrow key moves. Ten of them cross a ten-second gap, which is the
/// scale somebody uses a keyboard for; the mouse is for the long jumps.
constexpr quint64 kArrowStepNs = 1'000'000'000ULL;

/// What Page Up and Page Down move.
constexpr quint64 kPageStepNs = 10'000'000'000ULL;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

} // namespace

TimelineBar::TimelineBar(QWidget* parent)
    : QWidget{parent}
{
    // Focusable on purpose: a person who has just pressed Pause with the
    // keyboard should be able to step back a second without reaching for the
    // mouse.
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

QSize TimelineBar::sizeHint() const
{
    return QSize{320, kHandleRadius * 2 + 6};
}

QSize TimelineBar::minimumSizeHint() const
{
    return QSize{120, kHandleRadius * 2 + 6};
}

void TimelineBar::setDurationNs(quint64 durationNs)
{
    if (m_durationNs == durationNs) {
        return;
    }

    m_durationNs = durationNs;
    update();
}

void TimelineBar::setPositionNs(quint64 positionNs)
{
    // The finger wins. A position published by the executor while somebody is
    // dragging would pull the handle out from under them several times a
    // second.
    if (m_dragging || m_positionNs == positionNs) {
        return;
    }

    m_positionNs = positionNs;
    update();
}

QRect TimelineBar::trackRect() const
{
    const int y = (height() - kTrackHeight) / 2;
    return QRect{kHandleRadius, y, std::max(1, width() - kHandleRadius * 2), kTrackHeight};
}

int TimelineBar::xForPosition(quint64 positionNs) const
{
    const QRect track = trackRect();

    if (m_durationNs == 0) {
        return track.left();
    }

    const double fraction =
        std::min(1.0, static_cast<double>(positionNs) / static_cast<double>(m_durationNs));

    return track.left() + static_cast<int>(fraction * track.width());
}

quint64 TimelineBar::positionForX(int x) const
{
    const QRect track = trackRect();

    if (m_durationNs == 0 || track.width() <= 0) {
        return 0;
    }

    const int clamped = std::clamp(x, track.left(), track.right());
    const double fraction =
        static_cast<double>(clamped - track.left()) / static_cast<double>(track.width());

    return static_cast<quint64>(fraction * static_cast<double>(m_durationNs));
}

void TimelineBar::paintEvent(QPaintEvent*)
{
    const Theme theme = currentTheme();

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect track = trackRect();
    const double radius = kTrackHeight / 2.0;

    // The whole recording, in the colour a divider uses: present, and not
    // competing with the part that carries information.
    QPainterPath background;
    background.addRoundedRect(track, radius, radius);
    painter.fillPath(background, isEnabled() ? theme.panelAlternate : theme.panel);

    if (m_durationNs > 0) {
        const int handleX = xForPosition(positionNs());

        QRect elapsed = track;
        elapsed.setRight(std::max(track.left(), handleX));

        QPainterPath played;
        played.addRoundedRect(elapsed, radius, radius);
        painter.fillPath(played, isEnabled() ? theme.accent : theme.textMuted);

        // The handle. Larger while it is being held or hovered, which is the
        // only feedback a bar like this can give that it is grabbable at all.
        const int grown = kHandleRadius - ((m_dragging || m_hovered) ? 0 : 2);

        painter.setPen(QPen{theme.panel, 2});
        painter.setBrush(isEnabled() ? theme.accent : theme.textMuted);
        painter.drawEllipse(QPoint{handleX, track.center().y() + 1}, grown, grown);
    }

    // Focus is drawn as a ring around the track rather than as a dotted
    // rectangle around the widget: the same reason every other focus indicator
    // in this application is hand-drawn - the platform's is invisible on a dark
    // panel.
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen{theme.accentHover, 1});
        painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), radius + 2, radius + 2);
    }
}

void TimelineBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_durationNs == 0 || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging = true;
    m_dragPositionNs = positionForX(static_cast<int>(event->position().x()));

    // A click anywhere on the track jumps there, rather than only the handle
    // being grabbable. Nobody aims at a seven-pixel circle when the thing they
    // mean is "about two thirds in".
    update();
    Q_EMIT seekRequested(m_dragPositionNs);
}

void TimelineBar::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const quint64 wanted = positionForX(static_cast<int>(event->position().x()));

    if (wanted == m_dragPositionNs) {
        return;
    }

    m_dragPositionNs = wanted;
    update();

    // Emitted through the drag rather than at the end of it. A seek request
    // replaces any earlier one still unserved, so the cost of a long drag is
    // one seek - and hearing the recording while scrubbing is what makes
    // finding a moment possible.
    Q_EMIT seekRequested(m_dragPositionNs);
}

void TimelineBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_dragging = false;

    // Where the finger left it, so the handle does not jump back to the last
    // published position for the fraction of a second before the next one
    // arrives.
    m_positionNs = m_dragPositionNs;
    update();
}

void TimelineBar::keyPressEvent(QKeyEvent* event)
{
    if (m_durationNs == 0 || !isEnabled()) {
        QWidget::keyPressEvent(event);
        return;
    }

    const quint64 current = positionNs();
    quint64 wanted = current;

    switch (event->key()) {
    case Qt::Key_Left:
        wanted = current > kArrowStepNs ? current - kArrowStepNs : 0;
        break;
    case Qt::Key_Right:
        wanted = std::min(m_durationNs, current + kArrowStepNs);
        break;
    case Qt::Key_PageDown:
        wanted = current > kPageStepNs ? current - kPageStepNs : 0;
        break;
    case Qt::Key_PageUp:
        wanted = std::min(m_durationNs, current + kPageStepNs);
        break;
    case Qt::Key_Home:
        wanted = 0;
        break;
    case Qt::Key_End:
        wanted = m_durationNs;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (wanted != current) {
        m_positionNs = wanted;
        update();
        Q_EMIT seekRequested(wanted);
    }
}

void TimelineBar::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void TimelineBar::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

} // namespace torquebus::ui
