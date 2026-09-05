// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A toolbar button whose hover and press states move instead of snapping.
//
// It is a QToolButton driven by a QAction, exactly as QToolBar's own buttons
// are - `setDefaultAction` gives it the icon, the text, the shortcut, the
// tooltip, the enabled state and the triggering. Nothing about what the button
// *does* changes here; only the two states a mouse can put it in.
//
// The reason it exists as a widget rather than as a style sheet rule is that
// style sheets have no transitions: `QToolButton:hover` can only be on or off.
// Whether that is worth a class is a fair question, and the answer is that the
// toolbar holds Start and Stop - the two controls in this window an engineer
// reaches for while watching something else happen, and the two where "did that
// register?" is a real question.

#pragma once

#include "ui/common/Motion.h"

#include <QPoint>
#include <QToolButton>

class QAction;

namespace torquebus::ui {

class AnimatedToolButton final : public QToolButton {
public:
    /// Builds a button that stands in for `action` on a toolbar.
    ///
    /// Add it with `QToolBar::addWidget`, not `addAction` - adding the action
    /// would make QToolBar create a second, ordinary button beside this one.
    explicit AnimatedToolButton(QAction* action, QWidget* parent = nullptr);

protected:
    /// Draws the moving states *under* the button.
    ///
    /// The wash and the ripple are painted first and QToolButton::paintEvent
    /// runs after, so the icon and the label sit on top of them at full
    /// strength. Painting over the button would tint the glyph, which is the
    /// one part of a toolbar that has to stay legible.
    ///
    /// This works because `QToolButton` in torquebus.qss has a transparent
    /// background: the style sheet draws nothing where we have already drawn.
    void paintEvent(QPaintEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;

private:
    Motion m_hover;
    Motion m_ripple;
    QPoint m_ripplePoint;
};

} // namespace torquebus::ui
