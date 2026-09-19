// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/mainwindow/DockChrome.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace torquebus::ui {
namespace {

/// The current theme, or a dark one if the manager is not up yet.
///
/// Chrome is constructed while the main window is being built, and a null
/// manager there would otherwise mean painting with a default-constructed
/// palette - black on black.
[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// Thickness of the active-tab marker.
constexpr int kMarkerThickness = 2;

/// How long a ripple takes to fade out.
///
/// Longer than the hover fade on purpose: a ripple is feedback for something
/// that already happened, so it may finish after the click has been acted on.
/// A hover fade is feedback for something about to happen and must not lag.
constexpr int kRippleDurationMs = 320;

} // namespace

// ---------------------------------------------------------------------------
// StyledSeparator
// ---------------------------------------------------------------------------

StyledSeparator::StyledSeparator(KDDockWidgets::Core::Separator* controller,
                                 KDDockWidgets::Core::View* parent)
    : Separator{controller, parent}
    , m_hover{this}
{
    // Guarantees Enter and Leave without the widget having to track the mouse
    // itself. The base class already calls setMouseTracking(true) for its own
    // drag handling; this is the hover state, which is a different question.
    setAttribute(Qt::WA_Hover, true);
}

void StyledSeparator::paintEvent(QPaintEvent*)
{
    const Theme theme = currentTheme();

    // The whole rectangle, flat. A separator is 5 px of solid colour: it is a
    // gap that can be grabbed, not a bevelled control, and shading it would
    // make it the only thing in the window with a gloss on it.
    QPainter painter{this};
    painter.fillRect(rect(), mix(theme.divider, theme.accent, m_hover.value()));
}

bool StyledSeparator::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Enter:
        m_hover.setTarget(1.0);
        break;
    case QEvent::Leave:
        m_hover.setTarget(0.0);
        break;
    default:
        break;
    }

    return Separator::event(event);
}

// ---------------------------------------------------------------------------
// StyledTabBar
// ---------------------------------------------------------------------------

StyledTabBar::StyledTabBar(KDDockWidgets::Core::TabBar* controller, QWidget* parent)
    : TabBar{controller, parent}
    , m_marker{this}
    , m_ripple{this}
{
    setAttribute(Qt::WA_Hover, true);

    // currentChanged rather than a paint-time read of currentIndex(): the
    // marker has to know where it is coming *from*, and by the time a paint
    // happens the old index is gone.
    connect(
        this, &QTabBar::currentChanged, this, [this](int index) { moveMarkerTo(index, false); });

    moveMarkerTo(currentIndex(), true);
}

void StyledTabBar::tabLayoutChange()
{
    TabBar::tabLayoutChange();

    // Snapped rather than animated: nothing moved from anywhere, the strip was
    // relaid out underneath the marker.
    moveMarkerTo(currentIndex(), true);
}

void StyledTabBar::moveMarkerTo(int index, bool immediately)
{
    // Where the marker is *now*, worked out before any state is overwritten.
    //
    // Not markerRect() on its own: this runs from currentChanged, by which time
    // currentIndex() is already the new tab - so the settled branch of
    // markerRect() would answer with the destination and the slide would have
    // nowhere to come from. m_markerIndex is the tab the marker was actually
    // pointing at.
    QRect from;
    if (!immediately) {
        from = m_marker.isRunning()
                   ? markerRect()
                   : (m_markerIndex >= 0 && m_markerIndex < count() ? tabRect(m_markerIndex)
                                                                    : QRect{});
    }

    m_markerIndex = index;
    m_markerTo = index >= 0 ? tabRect(index) : QRect{};
    m_markerFrom = from;

    if (from.isNull() || m_markerTo.isNull()) {
        m_marker.jumpTo(1.0);
    } else {
        m_marker.jumpTo(0.0);
        m_marker.setTarget(1.0);
    }

    update();
}

QRect StyledTabBar::markerRect() const
{
    // Settled: ask the tab bar where the current tab is, right now.
    //
    // Stored geometry is only trustworthy for the length of one slide. Tabs are
    // relaid out by insertion, removal, dragging, eliding and by the strip
    // being resized, and QTabBar announces only some of those. Reading live
    // whenever nothing is moving makes every one of them correct without a
    // notification for each.
    if (!m_marker.isRunning() || m_markerFrom.isNull() || m_markerTo.isNull()) {
        const int index = currentIndex();
        return index >= 0 ? tabRect(index) : QRect{};
    }

    const double t = m_marker.value();

    // Only x and width are interpolated. The strip is one row, so the marker
    // never changes height, and animating a value that does not change costs a
    // repaint for nothing.
    const auto lerp = [t](int from, int to) {
        return static_cast<int>(std::lround(from + (to - from) * t));
    };

    const int left = lerp(m_markerFrom.left(), m_markerTo.left());
    const int width = lerp(m_markerFrom.width(), m_markerTo.width());

    return QRect{left, m_markerTo.top(), width, m_markerTo.height()};
}

void StyledTabBar::mousePressEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    const int index = tabAt(position);

    if (index >= 0) {
        m_ripplePoint = position;
        m_rippleTab = tabRect(index);
        m_ripple.restart(kRippleDurationMs);
    }

    TabBar::mousePressEvent(event);
}

void StyledTabBar::paintEvent(QPaintEvent* event)
{
    // The tabs first, exactly as QTabBar and the style sheet draw them. Every
    // rule in torquebus.qss still applies; what follows is drawn on top.
    TabBar::paintEvent(event);

    const Theme theme = currentTheme();

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    // --- the click ripple ---------------------------------------------------
    if (m_ripple.isRunning() && !m_rippleTab.isNull()) {
        const double progress = m_ripple.value();

        // Clipped to the tab that was clicked, so the circle reads as belonging
        // to that tab rather than washing across its neighbours.
        painter.save();
        painter.setClipRect(m_rippleTab);

        // Large enough to have crossed the whole tab by the time it fades. The
        // diagonal is the worst case for a press in a corner.
        const double reach = std::hypot(m_rippleTab.width(), m_rippleTab.height());

        QColor ink = theme.accent;

        // Fades as it grows. A ripple that keeps its opacity to the end
        // finishes as a flat wash of colour and then vanishes, which reads as a
        // flicker rather than a response.
        ink.setAlphaF(static_cast<float>(0.28 * (1.0 - progress)));

        painter.setBrush(ink);
        painter.drawEllipse(QPointF{m_ripplePoint}, reach * progress, reach * progress);
        painter.restore();
    }

    // --- the active-tab marker ---------------------------------------------
    const QRect marker = markerRect();
    if (!marker.isNull() && currentIndex() >= 0) {
        painter.setBrush(theme.accent);
        painter.drawRect(QRect{marker.left(), marker.top(), marker.width(), kMarkerThickness});
    }
}

} // namespace torquebus::ui
