// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The two pieces of KDDockWidgets chrome TorqueBus paints itself.
//
// Both are installed through ViewFactory, which is the library's supported way
// in: placement, hit testing, dragging and layout stay KDDockWidgets'. Only the
// painting changes.

#pragma once

#include "ui/common/Motion.h"

#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>

#include <QPoint>
#include <QRect>

namespace torquebus::ui {

/// The draggable bar between two docked panels, painted from the theme.
///
/// It has to be painted by hand, and the reason is worth stating because the
/// style sheet looks like it should be enough.
///
/// `configureDockingSystem` puts `CustomizableWidget_Separator` in
/// `Config::setDisabledPaintEvents`, so that the library stops drawing its own
/// chrome over ours. KDDockWidgets honours that in Separator::paintEvent like
/// this (2.2.5, src/qtwidgets/views/Separator.cpp):
///
///     if (Config::self().disabledPaintEvents() & CustomizableWidget_Separator) {
///         QWidget::paintEvent(ev);
///         return;
///     }
///
/// `QWidget::paintEvent` draws nothing. So the separator became a transparent
/// 5 px gap showing whatever was behind it - which is why it kept being
/// reported as invisible while the style sheet rule matched perfectly and the
/// palette in the chrome inspector read exactly the colour it was given.
///
/// Two earlier fixes went to the colour. Both were reasonable and neither could
/// have worked: no colour is visible if nothing paints it.
///
/// Painting from the theme directly, rather than relying on the style sheet
/// reaching a plain QWidget, also removes a Qt subtlety this project cannot
/// test for locally - and buys the hover fade, which a style sheet could not
/// have done at all.
class StyledSeparator final : public KDDockWidgets::QtWidgets::Separator {
public:
    StyledSeparator(KDDockWidgets::Core::Separator* controller, KDDockWidgets::Core::View* parent);

protected:
    void paintEvent(QPaintEvent* event) override;

    /// Enter and Leave are handled through event() rather than by overriding
    /// enterEvent, whose signature in KDDockWidgets goes through a Qt5/Qt6
    /// compatibility typedef declared in a private header.
    bool event(QEvent* event) override;

private:
    Motion m_hover;
};

/// The panel tab strip, with the active-tab marker animated.
///
/// The tabs themselves are still drawn by QTabBar and styled by
/// `torquebus.qss` - this paints *over* that, which is the same approach
/// StyledGroup takes and keeps every existing tab rule working. Reimplementing
/// tab painting outright would mean reproducing the label, the icon, the
/// elision and the close button by hand, to change a two-pixel line.
///
/// Two things are added:
///
///   - the accent marker slides from the old tab to the new one instead of
///     jumping, which is what makes a tab strip feel like one object rather
///     than a row of lamps;
///   - a click leaves a ripple from the point of the press.
///
/// The static `border-top: 2px solid @accent` was removed from the `:selected`
/// rule in the style sheet, or the two markers would both be drawn.
class StyledTabBar final : public KDDockWidgets::QtWidgets::TabBar {
public:
    StyledTabBar(KDDockWidgets::Core::TabBar* controller, QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

    /// Inserting, removing or dragging a tab reflows every rectangle beside it,
    /// so the marker's stored geometry goes stale without the current index
    /// changing. This is a protected virtual on QTabBar, not a signal.
    void tabLayoutChange() override;

private:
    /// Points the marker at `index`, easing from wherever it currently is.
    void moveMarkerTo(int index, bool immediately);

    /// The marker's rectangle right now, interpolated between the two tabs.
    [[nodiscard]] QRect markerRect() const;

    Motion m_marker;
    Motion m_ripple;

    /// Where the marker is coming from and going to, in widget coordinates.
    ///
    /// Rectangles rather than indices, because a tab being dragged, inserted or
    /// removed moves every index around it - and an animation that resolved
    /// indices while it ran would follow a tab that is no longer the one it
    /// started from.
    QRect m_markerFrom;
    QRect m_markerTo;

    /// The tab the marker points at, which is not always currentIndex().
    ///
    /// currentChanged arrives *after* the index has changed, so this is the
    /// only record of where a slide should start from.
    int m_markerIndex{-1};

    QPoint m_ripplePoint;
    QRect m_rippleTab;
};

} // namespace torquebus::ui
